// tpkbd_touchpad.c — 用户态触摸板驱动
// 设备: ThinkPad X1 Tablet Thin Keyboard Gen 3 (Chicony, VID 0x17ef PID 0x60b5)
// 原理: 该键盘"鼠标接口"上除 TrackPoint 相对鼠标外,还挂了一个标准 HID 多点触控
//       digitizer collection (Report ID 3): 4 指绝对坐标 + 接触数 + 物理按压位。
//       macOS 没有通用外接触控板驱动,本守护进程读取该报文并用 CGEvent 合成
//       光标移动 / 左键点击 / 两指滚动。
//
// 报文布局 (Report ID 3, 回调缓冲 30 字节 = 1 字节 report-ID 前缀 + 29 字节数据!):
//   r[0] = 0x03 (report-ID 前缀, len 含前缀; 回调 reportID 参数同样给出)
//   5 个指位, 每个 5 字节 (finger k 数据起始于 r[1+5k]):
//     byte0: bit0=confidence(1=真手指,非手掌) bit1=tip switch(1=接触)
//            bit2..4=contact id(实测 slot0→id0, slot1→id1) bit5..7=padding
//     byte1,2: X 小端 (0..1163)
//     byte3,4: Y 小端 (0..718)
//   r[26,27] = scan time (小端)
//   r[28]    = 接触计数
//   r[29]    = bit0 = 触摸板物理按压(左键)  ← 1480 帧实证: 按压期间 96 帧为 1
//
// (2026-10-10 实测纠错: 曾误判缓冲无前缀, 系调试打印把 r[0] 打在行首、
//  正文从 r[1] 起转储所致。带前缀解析后 X∈0..708/Y∈281..607 全程在量程内,
//  计数与按压位分布与三次测试动作完全吻合。)
//
// Input Mode 切换 (描述符 off=568..592 实证):
//   SET_FEATURE(Feature, reportID=4, buffer=[0x04,0x03]) — 缓冲首字节须为报告ID,
//   次字节为模式值 0x03(多点触控触摸板)。切换后设备由相对模式(0x02)转为发 0x03。
//
// 编译:
//   clang -O2 -framework CoreFoundation -framework IOKit \
//         -framework CoreGraphics -framework ApplicationServices -framework CoreVideo \
//         -o tpkbd_touchpad tpkbd_touchpad.c
//
// 运行:
//   ./tpkbd_touchpad --probe      仅打印报文(不需要辅助功能权限), 用于联调
//   ./tpkbd_touchpad              驱动模式(需在系统设置授予辅助功能权限)
//
// 环境变量调参:
//   TPKB_MOVE_GAIN    光标基础增益, 默认 1.8 (速度曲线默认 0.5x..2x)
//   TPKB_MOVE_ACCEL   1=按手速加速(默认), 0=固定倍率; 均保留小数位移
//   TPKB_SCROLL_GAIN  两指滚动增益(px/px), 默认 2.2
//   TPKB_SCROLL_NATURAL  1=自然滚动(默认,与 macOS 设置一致), 0=传统方向

#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <mach/mach_time.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdatomic.h>
#include <CoreVideo/CoreVideo.h>
#include "scroll_resampler.h"
#include "pointer_motion.h"
#include "two_finger_gesture.h"
#include "magnify_event.h"
#include "three_finger_drag.h"

#define VID               0x17ef
#define PID               0x60b5
#define REPORT_ID_TOUCH   0x03
#define REPORT_ID_RELMOUSE 0x02   // 触摸板实测发 0x02(相对模式); 小红点发 0x10 由 macOS 原生驱动
#define NFINGER           5
#define REPORT_BUF_SIZE   64
#define INPUT_MODE_RID    4       // Input Mode Feature 报告 ID(描述符实证)
#define INPUT_MODE_TOUCHPAD 0x03  // 多点触控触摸板模式值

static int probe_mode = 0;
static int debug = 0;
static int rel_rid = REPORT_ID_RELMOUSE;  // 可由 TPKB_REL_RID 覆盖
static float move_gain   = 1.8f;
static int move_acceleration = 1; // TPKB_MOVE_ACCEL=0: 固定倍率, 仍保留小数位移
static PointerMotion pointer_motion;
static int pointer_contact_id = -1;
static float scroll_gain = 2.2f;
static int   natural     = 1;
static float rel_gain    = 1.0f;  // 相对模式位移增益
static int   rel_y_invert = 0;    // 相对模式 Y 轴是否反转
static int   disable_rel  = 0;    // digitizer 生效后置 1, 关掉相对模式避免双倍速
static int   set_input_mode = 1;  // 打开设备后是否下发 Input Mode 切换
static int   input_mode_val = INPUT_MODE_TOUCHPAD;

// ---- 指位状态 ----
static int tip_state[NFINGER]   = {0, 0, 0, 0};
static int down_seq[NFINGER]    = {0, 0, 0, 0};
static int seq_counter          = 0;

// ---- 光标状态 ----
static int have_last = 0;
static int last_px = 0, last_py = 0;    // 跟踪指上次 pad 坐标
static int last_sx = 0, last_sy = 0;    // 跟踪指上次对应的光标屏幕坐标
static int move_slot = -1;              // 当前跟随的指槽; 换跟指时重新锚定不跳变
static int scroll_active = 0;
static uint64_t scroll_lift_ms = 0; // 双指通常先后抬起, 短暂单指不应截断惯性
static double last_cx = 0, last_cy = 0; // 保留双指质心的半个 count, 慢滚不交替丢帧
static int btn_prev = 0;
static int rel_btn_prev = 0;            // 相对模式左键上一状态
// ---- 点击状态机 ----
static int click_count = 0;             // 连击计数(单击1/双击2/三击3)
static uint64_t last_down_ms = 0;       // 上次按下时间
static double last_down_x = 0, last_down_y = 0;
static int release_streak = 0;          // 松开消抖: button=0 连续帧数

static uint64_t mach_ns(void);

static uint64_t now_ms(void) {
    return mach_ns() / 1000000;
}

// ---- --capture 模式: 抓取系统滚动事件(逆向 macOS 触控板动量手感) ----
// 复用本二进制已有的 TCC 授权(输入监控+辅助功能)。独立小工具会被 TCC
// 静默过滤(tap 创建成功但收不到任何事件), 必须借已授权进程。
static uint64_t now_us(void) {
    return (uint64_t)(CFAbsoluteTimeGetCurrent() * 1000000.0);
}

static CGEventRef cap_scroll_cb(CGEventTapProxy proxy, CGEventType type,
                                CGEventRef e, void *refcon) {
    (void)proxy;
    const char *tag = (const char *)refcon;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        fprintf(stderr, "[cap:%s] tap disabled: %d\n", tag, type);
        return e;
    }
    if (type != kCGEventScrollWheel) return e;
    int64_t cont   = CGEventGetIntegerValueField(e, kCGScrollWheelEventIsContinuous);
    int64_t phase  = CGEventGetIntegerValueField(e, kCGScrollWheelEventScrollPhase);
    int64_t mphase = CGEventGetIntegerValueField(e, kCGScrollWheelEventMomentumPhase);
    int64_t count  = CGEventGetIntegerValueField(e, kCGScrollWheelEventScrollCount);
    int64_t pd1    = CGEventGetIntegerValueField(e, kCGScrollWheelEventPointDeltaAxis1);
    int64_t pd2    = CGEventGetIntegerValueField(e, kCGScrollWheelEventPointDeltaAxis2);
    printf("%s %llu cont=%lld ph=%lld mph=%lld cnt=%lld pd=(%lld,%lld)\n",
           tag, now_us(), cont, phase, mphase, count, pd1, pd2);
    fflush(stdout);
    return e;
}

static int run_capture(void) {
    CGEventMask mask = CGEventMaskBit(kCGEventScrollWheel);
    struct { const char *tag; CGEventTapLocation loc; } taps[] = {
        { "HID  ", kCGHIDEventTap },
        { "SESS ", kCGSessionEventTap },
        { "ANNO ", kCGAnnotatedSessionEventTap },
    };
    int ok = 0;
    for (unsigned i = 0; i < sizeof(taps)/sizeof(taps[0]); i++) {
        CFMachPortRef tap = CGEventTapCreate(taps[i].loc, kCGHeadInsertEventTap,
                                             kCGEventTapOptionListenOnly, mask,
                                             cap_scroll_cb, (void*)taps[i].tag);
        if (!tap) { fprintf(stderr, "[cap:%s] 创建失败\n", taps[i].tag); continue; }
        CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
        CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopDefaultMode);
        fprintf(stderr, "[cap:%s] 已挂 (enabled=%d)\n", taps[i].tag,
                CGEventTapIsEnabled(tap));
        ok++;
    }
    if (!ok) return 1;
    fprintf(stderr, "[cap] 抓包中(仅监听滚动), Ctrl-C 停止\n");
    CFRunLoopRun();
    return 0;
}

// ---- 多屏支持: 钳制到"所有屏幕的联合矩形"(虚拟桌面)而非仅主屏 ----
// 只钳主屏会导致扩展屏(主屏右侧/上下)永远移不过去。
static CGRect screen_bounds;                     // 联合矩形
static CGDirectDisplayID g_displays[16];
static CGRect g_display_bounds[16];
static uint32_t g_display_count = 0;

static void update_display_bounds(void) {
    g_display_count = 16;
    if (CGGetActiveDisplayList(16, g_displays, &g_display_count) != kCGErrorSuccess
        || g_display_count == 0) {
        g_displays[0] = CGMainDisplayID();
        g_display_count = 1;
    }
    CGRect u = CGRectNull;
    for (uint32_t i = 0; i < g_display_count; i++) {
        g_display_bounds[i] = CGDisplayBounds(g_displays[i]);
        u = CGRectUnion(u, g_display_bounds[i]);
    }
    screen_bounds = u;
}

// 屏幕插拔/排列/分辨率变化时刷新(回调在主 runloop 上派发)。
// 实证坑(2026-10-16): 守护进程以 1 屏启动后插回扩展屏, 光标仍被钳在主屏 —
// 末次回调时 CGGetActiveDisplayList 可能尚未反映新屏(变更分阶段派发、
// 末态滞后), 因此回调里立即刷一次 + 1.5s 后复扫兜底; 日志无条件输出。
static void disp_rescan_cb(CFRunLoopTimerRef t, void *info) {
    (void)t; (void)info;
    update_display_bounds();
    fprintf(stderr, "[tpkbd] 显示器复扫: %u屏, 虚拟桌面=(%.0f,%.0f %.0fx%.0f)\n",
            g_display_count,
            screen_bounds.origin.x, screen_bounds.origin.y,
            screen_bounds.size.width, screen_bounds.size.height);
}

static void display_reconfig_cb(CGDirectDisplayID d, CGDisplayChangeSummaryFlags f, void *u) {
    (void)d; (void)u;
    update_display_bounds();
    fprintf(stderr, "[tpkbd] 显示器配置变化 flags=0x%x → %u屏, 虚拟桌面=(%.0f,%.0f %.0fx%.0f)\n",
            (unsigned)f, g_display_count,
            screen_bounds.origin.x, screen_bounds.origin.y,
            screen_bounds.size.width, screen_bounds.size.height);
    static CFRunLoopTimerRef rescan = NULL;
    if (rescan) {
        CFRunLoopRemoveTimer(CFRunLoopGetMain(), rescan, kCFRunLoopDefaultMode);
        CFRelease(rescan);
    }
    CFRunLoopTimerContext ctx = {0};
    rescan = CFRunLoopTimerCreate(kCFAllocatorDefault,
                                  CFAbsoluteTimeGetCurrent() + 1.5, 0, 0, 0,
                                  disp_rescan_cb, &ctx);
    if (rescan) CFRunLoopAddTimer(CFRunLoopGetMain(), rescan, kCFRunLoopDefaultMode);
}

static int point_on_display(int x, int y) {
    CGPoint p = CGPointMake(x, y);
    for (uint32_t i = 0; i < g_display_count; i++)
        if (CGRectContainsPoint(g_display_bounds[i], p)) return 1;
    return 0;
}

// 目标点若落在屏幕间的"空隙"(如高低错落的 L 形布局), 沿来路回退到
// 最后一个仍在实际屏幕上的点, 避免光标消失在无屏幕区域。
static void avoid_gap(int *nx, int *ny, int from_x, int from_y) {
    if (point_on_display(*nx, *ny)) return;
    double dx = *nx - from_x, dy = *ny - from_y;
    double dist = hypot(dx, dy);
    if (dist < 1.0) return;
    for (double t = 1.0; t > 0.0; t -= 2.0 / dist) {   // 2px 步长回退
        int x = (int)(from_x + dx * t), y = (int)(from_y + dy * t);
        if (point_on_display(x, y)) { *nx = x; *ny = y; return; }
    }
    *nx = from_x; *ny = from_y;
}

static CFNumberRef num_create(int v) {
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
}

// 事件注入点: 从 launchd 后台 agent 往 kCGHIDEventTap 注入鼠标移动在部分
// macOS 版本上会被静默丢弃, 会话层 kCGSessionEventTap 更可靠。可经 TPKB_TAP 切换。
static CGEventTapLocation g_tap = kCGSessionEventTap;

static void post_mouse_move(CGPoint p) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, p, 0);
    if (e) { CGEventPost(g_tap, e); CFRelease(e); }
}

static void post_click(CGPoint p, int down, int count) {
    CGEventType t = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
    CGEventRef e = CGEventCreateMouseEvent(NULL, t, p, kCGMouseButtonLeft);
    if (e) {
        CGEventSetIntegerValueField(e, kCGMouseEventClickState, count);
        CGEventPost(g_tap, e);
        CFRelease(e);
    }
}

static void post_right_click(CGPoint p, int down, int count) {
    CGEventType t = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
    CGEventRef e = CGEventCreateMouseEvent(NULL, t, p, kCGMouseButtonRight);
    if (e) {
        CGEventSetIntegerValueField(e, kCGMouseEventClickState, count);
        CGEventPost(g_tap, e);
        CFRelease(e);
    }
}

// 连击计数(500ms 内且位移 <12px 的再次按下递增, 双击=2 三击=3)
static int click_count_for(CGPoint pos) {
    uint64_t t = now_ms();
    double dd = hypot(pos.x - last_down_x, pos.y - last_down_y);
    if (t - last_down_ms < 500 && dd < 12) {
        if (click_count < 3) click_count++;
    } else {
        click_count = 1;
    }
    last_down_ms = t; last_down_x = pos.x; last_down_y = pos.y;
    return click_count;
}

// ---- 轻点点按 & 双指右键 ----
// 轻点 = 短触摸(<TPKB_TAP_MS) + 位移小 + 未产生滚动像素; 单指轻点 → 左键。
// 双指轻点 → 右键默认关(易误触, TPKB_TAP_RIGHT=1 打开); 双指物理按压始终是右键。
static int      tap_on = 1;            // TPKB_TAPCLICK
static int      tap_right_on = 0;      // TPKB_TAP_RIGHT: 双指轻点=右键(默认关)
static int      tap_ms_thresh = 250;   // TPKB_TAP_MS
static int      tap_nfid = 0;          // 本轮触摸的峰值指数
static int      tap_moved = 0;         // 主指位移超阈值
static int      tap_pointer_moved = 0; // 本轮已经移动过光标, 不能再被重新判成轻点
static int      tap_active = 0;        // 本轮触摸已记录
static uint64_t tap_down_ms = 0;
static int      tap_anchor_x = 0, tap_anchor_y = 0;
static int      btn_type = 0;          // 0=左键 1=右键(按压期间锁定)
static int      drag_armed = 0;        // 按下后拖移抑制期: 位移未过阈值前吞微移
static int      drag_ax = 0, drag_ay = 0;  // 按下瞬间跟踪指的 pad 坐标(抑制锚点)
static int      drag_slop = 12;        // TPKB_DRAG_SLOP(counts): 按下沉陷抑制阈值
static int      move_slop = 14;        // TPKB_MOVE_SLOP(counts): 落指死区, 0=关
                                        // 默认取 libinput DEFAULT_TAP_MOVE_THRESHOLD
                                        // = 1.3mm ≈ 14 counts(本板 ~11 counts/mm),
                                        // 沉降伪影(实测 7~18 counts)整体吞掉
static int      mv_slop_done = 0;      // 本轮接触已越过落指死区
static int      mv_frames = 0;         // 本轮接触已跟踪帧数
static int      snap_done = 0;         // 本轮接触已吞掉落板跳变(仅一次)

// 轻点与落指死区共用闭区间, 避免同一帧既允许点击又放行移动。
static int outside_move_slop(int dx, int dy) {
    return abs(dx) > move_slop || abs(dy) > move_slop;
}

// ---- 打字锁定 (disable-while-typing, libinput DWT 语义) ----
// 敲键后 typelock_ms 内吞掉触摸板的"新"触摸(移动/滚动/手势/轻点/新按压);
// 敲键瞬间已经在板上的手势放行继续(边滚边打字不中断)。
// 物理按键只挡新按下、不挡松开 → 按住的键永不卡死。小红点走原生驱动不受影响。
static int       typelock_ms = 500;    // TPKB_TYPELOCK_MS, 0=关
static uint64_t  last_key_ms = 0;      // 最近一次 keyDown 时刻
static int       touch_now = 0;        // 当前有"正在被跟踪"的接触(被吞掉的不算)
static int       type_grandf = 0;      // 敲键瞬间已有接触 → 该轮手势免锁定
static int       type_lock_on = 0;     // 锁定中(仅日志用)
static CFMachPortRef key_tap_port = NULL;

static CGEventRef key_tap_cb(CGEventTapProxy proxy, CGEventType type,
                             CGEventRef event, void *userInfo) {
    (void)proxy; (void)userInfo;
    if (type == kCGEventTapDisabledByTimeout) {   // 主线程偶发阻塞后自救
        if (key_tap_port) CGEventTapEnable(key_tap_port, true);
        return event;
    }
    if (type == kCGEventTapDisabledByUserInput) return event;
    if (type == kCGEventKeyDown) {
        last_key_ms = now_ms();
        type_grandf = touch_now;    // 敲键时手上已有接触 → 该手势继续可用
        if (debug) {
            static uint64_t last_klog = 0;
            if (now_ms() - last_klog > 1000) {
                fprintf(stderr, "[tpkbd] KEY down → lock %dms (grandf=%d)\n",
                        typelock_ms, type_grandf);
                last_klog = now_ms();
            }
        }
    }
    return event;   // ListenOnly: 只看不动, 不吞改任何按键
}

// 按住左键时的拖动(框选/拖拽必须用 Dragged 事件, 用 Moved 系统不认为在拖)
static void post_mouse_drag(CGPoint p) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDragged, p, kCGMouseButtonLeft);
    if (e) { CGEventPost(g_tap, e); CFRelease(e); }
}

// 按住右键时的拖动(双指按压后移动 = 右键拖移, 同原生触控板)
static void post_right_drag(CGPoint p) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, kCGEventRightMouseDragged, p, kCGMouseButtonRight);
    if (e) { CGEventPost(g_tap, e); CFRelease(e); }
}

// ---- 手势相位滚动 (移植自 Mac Mouse Fix GestureScrollSimulator) ----
// macOS 原生触控板"丝滑"的真正来源不是滚了多少像素, 而是事件带相位标记:
//  1. field 88 IsContinuous=1  → App 按"连续手势"而非"滚轮格子"处理;
//  2. field 99 IOHID相位: MayBegin→Began→Changed→Ended (手势期);
//  3. field 123 动量相位: Begin→Continue→End, 此时 field 99=Undefined;
//  4. 手势期每个滚动事件后再发一个 type29 subtype6 手势事件
//     (Safari 双指横滑翻页/页面回弹依赖它)。
// 无相位的事件被 App 当鼠标滚轮: 各自再做一层加速/平滑 → 快慢都顿挫。
// 行增量 = 像素/10 (CGEventSource pixelsPerLine 默认 10)。
enum { PH_UNDEFINED = 0, PH_BEGAN = 1, PH_CHANGED = 2,
       PH_ENDED = 4, PH_CANCELLED = 8, PH_MAY_BEGIN = 128 };
enum { MPH_NONE = 0, MPH_BEGIN = 1, MPH_CONTINUE = 2, MPH_END = 3 };

static double scroll_acc_x = 0, scroll_acc_y = 0;   // 像素亚像素累积(屏上方向)
static double line_acc_x = 0, line_acc_y = 0;       // 行(1/10px)亚像素累积
static int gesture_state = 0;   // 0=空闲 1=已发MayBegin 2=已发Began/Changed

static uint64_t mach_ns(void) {
    static mach_timebase_info_data_t tb = {0};
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

// SIGUSR1: 30 秒有界采样。信号处理只置位, 热路径只写内存, 结束才输出日志。
// R=设备触摸报文(ntips,触点槽位掩码,质心x*2,y*2,scanTime,按压),
// S=滚动(phase,momentum,dx,dy), Z=缩放(phase,delta*1e6)。不监听键盘或其他设备。
typedef struct {
    uint64_t ns;
    char kind;
    int a, b, c, d, e, f;
} ScrollTraceRow;
static ScrollTraceRow scroll_trace_rows[16384];
static size_t scroll_trace_count = 0;
static uint64_t scroll_trace_start = 0;
static uint64_t scroll_trace_duration = 30000000000ULL;
static volatile sig_atomic_t scroll_trace_requested = 0;
static volatile sig_atomic_t driver_stop_requested = 0;

static void driver_stop_signal(int signo) {
    (void)signo;
    driver_stop_requested = 1;
}

static void scroll_trace_signal(int signo) {
    scroll_trace_requested = signo == SIGUSR2 ? 2 : 1;
}

static void scroll_trace_record(char kind, int a, int b, int c, int d, int e, int f) {
    if (!scroll_trace_start || scroll_trace_count >= 16384) return;
    scroll_trace_rows[scroll_trace_count++] = (ScrollTraceRow){mach_ns(), kind, a, b, c, d, e, f};
}

// 在已授权的常驻进程内对照原生/合成手势。只在30秒采样窗口启用。
static CFMachPortRef pinch_trace_taps[2];
static CFRunLoopSourceRef pinch_trace_sources[2];
static int pinch_trace_native_fields = 0;

static CGEventRef pinch_trace_cb(CGEventTapProxy proxy, CGEventType type,
                                 CGEventRef event, void *context) {
    (void)proxy;
    int layer = (int)(intptr_t)context;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (scroll_trace_start && pinch_trace_taps[layer]) CGEventTapEnable(pinch_trace_taps[layer], true);
        return event;
    }
    if (!scroll_trace_start || (type != 29 && type != 30)) return event;
    int subtype = (int)CGEventGetIntegerValueField(event, 110);
    int phase = (int)CGEventGetIntegerValueField(event, 132);
    double delta = CGEventGetDoubleValueField(event, 113);
    int ours = CGEventGetIntegerValueField(event, kCGEventSourceUserData) == 0x54504b42;
    scroll_trace_record(layer ? 'H' : 'G', type, subtype, phase,
                        (int)lround(fmax(-2000, fmin(2000, delta)) * 1000000), ours, 0);
    if (!ours && layer == 0 && pinch_trace_native_fields < 8 && (subtype == 8 || subtype == 4)) {
        pinch_trace_native_fields++;
        fprintf(stderr, "[pinch-native] type=%u subtype=%d phase=%d delta=%.8f fields:",
                type, subtype, phase, delta);
        for (int field = 0; field < 200; field++) {
            int64_t value = CGEventGetIntegerValueField(event, field);
            double floating = CGEventGetDoubleValueField(event, field);
            if (value || floating != 0)
                fprintf(stderr, " %d=%lld/%.8g", field, (long long)value, floating);
        }
        fprintf(stderr, "\n");
        CFTypeRef (*copy_hid)(CGEventRef) = dlsym(RTLD_DEFAULT, "CGEventCopyIOHIDEvent");
        int (*get_type)(CFTypeRef) = dlsym(RTLD_DEFAULT, "IOHIDEventGetType");
        uint64_t (*get_sender)(CFTypeRef) = dlsym(RTLD_DEFAULT, "IOHIDEventGetSenderID");
        int (*get_phase)(CFTypeRef) = dlsym(RTLD_DEFAULT, "IOHIDEventGetPhase");
        double (*get_double)(CFTypeRef, int) = dlsym(RTLD_DEFAULT, "IOHIDEventGetDoubleValue");
        if (copy_hid && get_type && get_sender && get_phase && get_double) {
            CFTypeRef hid = copy_hid(event);
            if (hid) {
                int ht = get_type(hid);
                fprintf(stderr, "[pinch-hid] subtype=%d type=%d sender=%llu phase=%d fields:",
                        subtype, ht, (unsigned long long)get_sender(hid), get_phase(hid));
                for (int j = 0; j < 10; j++)
                    fprintf(stderr, " %x=%.8g", (ht << 16) | j, get_double(hid, (ht << 16) | j));
                fprintf(stderr, "\n");
                CFRelease(hid);
            } else fprintf(stderr, "[pinch-hid] subtype=%d no HID payload\n", subtype);
        }
    }
    return event;
}

static void pinch_trace_start(void) {
    pinch_trace_native_fields = 0;
    CGEventTapLocation locations[] = {kCGHIDEventTap, kCGSessionEventTap};
    for (int i = 0; i < 2; i++) {
        pinch_trace_taps[i] = CGEventTapCreate(locations[i], kCGHeadInsertEventTap,
            kCGEventTapOptionListenOnly, CGEventMaskBit(29) | CGEventMaskBit(30),
            pinch_trace_cb, (void *)(intptr_t)i);
        if (!pinch_trace_taps[i]) continue;
        pinch_trace_sources[i] = CFMachPortCreateRunLoopSource(NULL, pinch_trace_taps[i], 0);
        if (pinch_trace_sources[i])
            CFRunLoopAddSource(CFRunLoopGetMain(), pinch_trace_sources[i], kCFRunLoopDefaultMode);
        CGEventTapEnable(pinch_trace_taps[i], true);
        fprintf(stderr, "[pinch-trace] layer=%d enabled=%d\n", i, CGEventTapIsEnabled(pinch_trace_taps[i]));
    }
}

static void pinch_trace_stop(void) {
    for (int i = 0; i < 2; i++) {
        if (pinch_trace_sources[i]) {
            CFRunLoopRemoveSource(CFRunLoopGetMain(), pinch_trace_sources[i], kCFRunLoopDefaultMode);
            CFRelease(pinch_trace_sources[i]); pinch_trace_sources[i] = NULL;
        }
        if (pinch_trace_taps[i]) {
            CFMachPortInvalidate(pinch_trace_taps[i]);
            CFRelease(pinch_trace_taps[i]); pinch_trace_taps[i] = NULL;
        }
    }
}

static void scroll_trace_control(CFRunLoopTimerRef timer, void *context) {
    (void)timer; (void)context;
    if (driver_stop_requested) { CFRunLoopStop(CFRunLoopGetMain()); return; }
    if (scroll_trace_requested) {
        int request = scroll_trace_requested;
        scroll_trace_requested = 0;
        if (!scroll_trace_start) {
            scroll_trace_duration = request == 2 ? 90000000000ULL : 30000000000ULL;
            scroll_trace_count = 0;
            scroll_trace_start = mach_ns();
            pinch_trace_start();
            fprintf(stderr, "[scroll-trace] START: recording touch/gesture timing for %llu seconds\n",
                    (unsigned long long)(scroll_trace_duration / 1000000000));
        }
    }
    if (!scroll_trace_start || mach_ns() - scroll_trace_start < scroll_trace_duration) return;
    uint64_t start = scroll_trace_start;
    scroll_trace_start = 0;
    pinch_trace_stop();
    fprintf(stderr, "[scroll-trace] DATA: us,kind,a,b,c,d,e,f\n");
    for (size_t i = 0; i < scroll_trace_count; i++) {
        ScrollTraceRow *r = &scroll_trace_rows[i];
        fprintf(stderr, "[scroll-trace] %llu,%c,%d,%d,%d,%d,%d,%d\n",
                (unsigned long long)((r->ns - start) / 1000), r->kind,
                r->a, r->b, r->c, r->d, r->e, r->f);
    }
    fprintf(stderr, "[scroll-trace] END: %zu rows%s\n", scroll_trace_count,
            scroll_trace_count == 16384 ? " (buffer full)" : "");
}

// 发一个 type22 滚动事件(+手势期附带 type29)。px 增量必须已取整。
static void send_scroll_event(int32_t ipx, int32_t ipy,
                              double gdx, double gdy,
                              int phase, int mom_phase, int with_gesture) {
    // 行增量 = 像素/10, 亚像素累积后取整(MMF 同款; 动量Begin时会重置累积器)
    line_acc_x += ipx * 0.1; line_acc_y += ipy * 0.1;
    int64_t lx = (int64_t)line_acc_x, ly = (int64_t)line_acc_y;
    line_acc_x -= lx; line_acc_y -= ly;

    CGEventRef e = CGEventCreate(NULL);
    if (!e) return;
    CGEventSetIntegerValueField(e, 55, 22);    // NSEventTypeScrollWheel
    CGEventSetIntegerValueField(e, 88, 1);     // IsContinuous: 触控板式连续滚动
    CGEventSetIntegerValueField(e, 137, 0);    // directionInvertedFromDevice=NO(方向已自行换算)
    CGEventSetIntegerValueField(e, 11, ly);    // DeltaAxis1      行增量(竖)
    CGEventSetIntegerValueField(e, 12, lx);    // DeltaAxis2      行增量(横)
    CGEventSetIntegerValueField(e, 96, ipy);   // PointDeltaAxis1 像素增量(竖)
    CGEventSetIntegerValueField(e, 97, ipx);   // PointDeltaAxis2 像素增量(横)
    // FixedPt 有小数精度, 不能从已截断的整行增量生成, 否则慢滚是 0/1 行跳变。
    CGEventSetDoubleValueField(e, kCGScrollWheelEventFixedPtDeltaAxis1, ipy * 0.1);
    CGEventSetDoubleValueField(e, kCGScrollWheelEventFixedPtDeltaAxis2, ipx * 0.1);
    CGEventSetIntegerValueField(e, 99, phase);
    CGEventSetIntegerValueField(e, 123, mom_phase);
    CGEventSetTimestamp(e, mach_ns());
    scroll_trace_record('S', phase, mom_phase, ipx, ipy, 0, 0);
    CGEventPost(g_tap, e);
    CFRelease(e);

    if (phase != PH_UNDEFINED && with_gesture) {
        CGEventRef g = CGEventCreate(NULL);
        if (!g) return;
        CGEventSetIntegerValueField(g, 55, 29);   // NSEventTypeGesture
        CGEventSetIntegerValueField(g, 110, 6);   // subtype 6 = scroll 手势
        CGEventSetDoubleValueField(g, 116, gdx);  // 手势增量 = 像素×1.67 (MMF 实测值,
        CGEventSetDoubleValueField(g, 119, gdy);  //   让横滑翻页力度接近原生触控板)
        CGEventSetIntegerValueField(g, 132, phase);
        CGEventSetTimestamp(g, mach_ns());
        CGEventPost(g_tap, g);
        CFRelease(g);
    }
}

static void scroll_velocity_reset(void);

// 双指手势开始: 发 MayBegin(零增量), 重置累积器
static void gesture_send_maybegin(void) {
    scroll_velocity_reset();
    scroll_acc_x = scroll_acc_y = 0;
    line_acc_x = line_acc_y = 0;
    gesture_state = 1;
    send_scroll_event(0, 0, 0, 0, PH_MAY_BEGIN, MPH_NONE, 1);
}

// 手势滚动帧: dx/dy 为传感器方向增量; natural=1 时原样下发(实测方向手感正确)。
// 亚像素累积: 慢滚时帧增量常 <1px, 首个非零像素帧发 Began, 之后 Changed。
// 零像素帧不发(真实触控板同样合并)。
static void post_gesture_scroll(double dx, double dy) {
    if (!natural) { dx = -dx; dy = -dy; }
    scroll_acc_x += dx; scroll_acc_y += dy;
    int32_t ipx = (int32_t)scroll_acc_x, ipy = (int32_t)scroll_acc_y;
    scroll_acc_x -= ipx; scroll_acc_y -= ipy;
    if (!ipx && !ipy) return;
    int phase = (gesture_state == 1) ? PH_BEGAN : PH_CHANGED;
    gesture_state = 2;
    send_scroll_event(ipx, ipy, ipx * 1.67, ipy * 1.67, phase, MPH_NONE, 1);
}

// 触摸位移按显示刷新重采样。12ms 缓冲覆盖实测 7.1ms 扫描周期及 USB 到达抖动。
// 只在已经收到的位置间插值, 不预测未来位移, 因此停手不会继续漂移。
static double scroll_smooth_ms = 12.0; // TPKB_SCROLL_SMOOTH_MS=0 可对照直通模式
static ScrollResampler touch_resampler;
static int touch_resampling = 0;
static uint16_t touch_scan_previous = 0;
static uint64_t touch_sample_ns = 0;
static int scroll_scheduler_enabled = 0;
static void scroll_frames_start(void);
static void scroll_frames_stop_if_idle(void);

static void touch_scroll_begin(uint64_t ns, uint16_t scan) {
    touch_sample_ns = ns;
    touch_scan_previous = scan;
    touch_resampling = scroll_smooth_ms > 0;
    if (touch_resampling) {
        scroll_resampler_begin(&touch_resampler, ns);
        scroll_frames_start();
    }
}

static uint64_t touch_scroll_time(uint64_t received_ns, uint16_t scan) {
    // HID Scan Time 以 100us 为单位, 实测每帧递增 71/72; 用它排除 USB 调度抖动。
    uint16_t ticks = (uint16_t)(scan - touch_scan_previous);
    uint64_t next = touch_sample_ns + (uint64_t)ticks * 100000;
    uint64_t error = next > received_ns ? next - received_ns : received_ns - next;
    if (!ticks || ticks > 1000 || error > 20000000) next = received_ns;
    if (next <= touch_sample_ns) next = touch_sample_ns + 1;
    touch_scan_previous = scan;
    touch_sample_ns = next;
    return next;
}

static void touch_scroll_push(double dx, double dy, uint64_t ns) {
    if (touch_resampling) scroll_resampler_push(&touch_resampler, ns, dx, dy);
    else post_gesture_scroll(dx, dy);
}

static void touch_scroll_tick(uint64_t ns) {
    if (!touch_resampling) return;
    uint64_t delay = (uint64_t)(scroll_smooth_ms * 1e6);
    double dx, dy;
    scroll_resampler_read(&touch_resampler, ns > delay ? ns - delay : 0, &dx, &dy);
    post_gesture_scroll(dx, dy);
}

static void touch_scroll_finish(void) {
    if (!touch_resampling) return;
    double dx, dy;
    // 先送完最多一个缓冲窗口的剩余位移, 再发 Ended; 不遗留到下次手势。
    scroll_resampler_read(&touch_resampler, UINT64_MAX, &dx, &dy);
    post_gesture_scroll(dx, dy);
    touch_resampling = 0;
}

// ---- 双指捏合: 原生 magnify 事件, 距离比与自然滚动方向独立 ----
static int pinch_on = 1;            // TPKB_PINCH
static double pinch_gain = 1.0;     // TPKB_PINCH_GAIN
static int pinch_active = 0;
static int pinch_sent = 0;          // Began accompanies the first nonzero delta
static int pinch_tail = 0;          // 捏合后残留的一根手指不应移动光标
static double pinch_last_distance = 0;
static CGPoint pinch_location;
static ScrollResampler pinch_resampler; // x=log(distance ratio), y=0
static TwoFingerGesture two_finger;

static void post_pinch_event(double log_delta, int phase) {
    double delta = expm1(log_delta * pinch_gain);
    CGEventRef event = magnify_event_create(pinch_location, delta, phase, mach_ns());
    if (!event) return;
    scroll_trace_record('Z', phase, (int)lround(fmax(-2000, fmin(2000, delta)) * 1000000),
                        0, 0, 0, 0);
    // Match the magnification injection path used by TouchSimulator. Let the
    // WindowServer route the gesture before it reaches application event taps.
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

static void pinch_emit_at(uint64_t ns) {
    double delta, unused;
    scroll_resampler_read(&pinch_resampler, ns, &delta, &unused);
    if (fabs(delta) <= 1e-10) return;
    // Safari's TabContentView only starts its tab picker for a negative Began.
    // A zero Began followed by negative Changed events only zooms the page.
    post_pinch_event(delta, pinch_sent ? PH_CHANGED : PH_BEGAN);
    pinch_sent = 1;
}

static void pinch_tick(uint64_t ns) {
    if (!pinch_active) return;
    uint64_t delay = (uint64_t)(scroll_smooth_ms * 1e6);
    pinch_emit_at(ns > delay ? ns - delay : 0);
}

static void pinch_begin(CGPoint location, double initial_distance,
                        uint64_t initial_ns, uint64_t ns, uint16_t scan) {
    pinch_active = 1;
    pinch_sent = 0;
    pinch_tail = 0;
    pinch_last_distance = initial_distance;
    pinch_location = location;
    touch_sample_ns = ns;
    touch_scan_previous = scan;
    scroll_resampler_begin(&pinch_resampler, initial_ns);
    if (scroll_smooth_ms > 0) scroll_frames_start();
}

static void pinch_move(double distance, uint64_t ns) {
    if (distance < 40) return; // Avoid a singular ratio when fingers nearly meet.
    double delta = log(distance / pinch_last_distance);
    pinch_last_distance = distance;
    scroll_resampler_push(&pinch_resampler, ns, delta, 0);
    if (scroll_smooth_ms == 0) pinch_emit_at(UINT64_MAX);
}

static void pinch_finish(int cancelled) {
    if (!pinch_active) return;
    pinch_emit_at(UINT64_MAX);
    if (pinch_sent) post_pinch_event(0, cancelled ? PH_CANCELLED : PH_ENDED);
    pinch_active = 0;
    pinch_sent = 0;
    two_finger.mode = TWO_NONE;
    tap_moved = 1;
    scroll_frames_stop_if_idle();
}

// 惯性滚动帧: 只发 type22, field99=Undefined + 动量相位
static int post_momentum_scroll(double dx, double dy, int mom_phase) {
    if (mom_phase == MPH_BEGIN) { line_acc_x = line_acc_y = 0; }  // 同 MMF: 动量期重置行累积
    scroll_acc_x += dx; scroll_acc_y += dy;
    int32_t ipx = (int32_t)scroll_acc_x, ipy = (int32_t)scroll_acc_y;
    scroll_acc_x -= ipx; scroll_acc_y -= ipy;
    if (!ipx && !ipy && mom_phase != MPH_END) return 0;  // End 零增量也必须发(收尾信号)
    send_scroll_event(ipx, ipy, 0, 0, PH_UNDEFINED, mom_phase, 0);
    return 1;
}

// ---- 惯性滚动(松手渐停) ----
// 移植自 Mac Mouse Fix (github.com/noah-nuebling/mac-mouse-fix) 的
// "trackpad-like momentum" 参数与 DragCurve 物理模型:
//   v'(t) = -a·v^b,  a=30(TPKB_DRAG_A), b=0.7(TPKB_DRAG_B), 速度单位 px/s,
//   停止阈值 vs=1 px/s。
// 闭式解: Q=(1-b)a, u(t)=Q(c-t), c=v0^{1-b}/Q, T=(v0^{1-b}-vs^{1-b})/Q,
//         d(t) = (v0^{2-b} - u^p) / (Q·p),  p=(2-b)/(1-b)
// 参考量级: v0=2000px/s → 滑行 ~0.98s / ~500px; v0=500px/s → ~0.59s。
// 手指重新接触或物理按压立即停止。
static int   momentum_on = 1;              // TPKB_MOMENTUM, 默认开
static double mom_drag_a = 30.0;           // TPKB_DRAG_A
static double mom_drag_b = 0.7;            // TPKB_DRAG_B
static int   mom_active  = 0;
static double mom_dir_x = 0, mom_dir_y = 0;    // 滑行方向单位向量(屏上坐标)
static double mom_Q = 0, mom_p = 0;            // 曲线常数
static double mom_u0p = 0;                     // v0^(2-b), 预计算
static double mom_c = 0, mom_ustop = 0, mom_total_d = 0;
static double mom_last_d = 0;                  // 已下发距离
static uint64_t mom_start_ns = 0;
static int mom_begun = 0;                         // 已发出动量Begin事件
static double scrl_ema_x = 0, scrl_ema_y = 0;  // 松手速度EMA, 单位 px/ms(屏上方向)
static double last_scrl_ms = 0;                // 单调时钟, 保留亚毫秒精度

// 固定时间常数, 而非固定帧权重: 10ms 一帧时约等于旧的 0.3 权重。
// 同样的动作在不同报文频率下应得到相同的松手速度。
static const double scroll_velocity_tau_ms = 28.0;

static void scroll_velocity_reset(void) {
    scrl_ema_x = scrl_ema_y = 0;
    last_scrl_ms = (double)mach_ns() / 1e6;
}

static void scroll_velocity_update(double dx, double dy, double timestamp_ms) {
    double dt = timestamp_ms - last_scrl_ms;
    if (dt <= 0) return;
    if (dt > 150.0) scrl_ema_x = scrl_ema_y = 0;
    double alpha = -expm1(-dt / scroll_velocity_tau_ms);
    // 反向时丢掉上一方向的速度, 防止短促回拉后仍向原方向惯性滑行。
    if (dx * scrl_ema_x < 0) scrl_ema_x = 0;
    if (dy * scrl_ema_y < 0) scrl_ema_y = 0;
    scrl_ema_x += alpha * (dx / dt - scrl_ema_x);
    scrl_ema_y += alpha * (dy / dt - scrl_ema_y);
    last_scrl_ms = timestamp_ms;
}

static void mom_stop(void) {
    if (!mom_active) return;
    mom_active = 0;
    // 截停(手指重新接触/物理按压)也要发动量End, App 才知道滑行结束
    if (mom_begun) post_momentum_scroll(0, 0, MPH_END);
    else send_scroll_event(0, 0, 0, 0, PH_UNDEFINED, MPH_END, 0);
    mom_begun = 0;
    scrl_ema_x = scrl_ema_y = 0;
    last_scrl_ms = 0;
    scroll_frames_stop_if_idle();
}

static void mom_timer_cb(CFRunLoopTimerRef t, void *info) {
    (void)t; (void)info;
    if (!mom_active) return;
    double ts = (double)(mach_ns() - mom_start_ns) / 1e9;
    double u = mom_Q * (mom_c - ts);
    double d;
    int done = 0;
    if (u <= mom_ustop) { d = mom_total_d; done = 1; }
    else                { d = (mom_u0p - pow(u, mom_p)) / (mom_Q * mom_p); }
    double delta = d - mom_last_d;
    mom_last_d = d;
    if (delta != 0) {
        // 首帧不足一像素时会被累积, 必须等真正发出 Begin 才能转 Continue。
        if (post_momentum_scroll(mom_dir_x * delta, mom_dir_y * delta,
                                 mom_begun ? MPH_CONTINUE : MPH_BEGIN))
            mom_begun = 1;
    }
    if (done) mom_stop();
}

static void mom_start(double dirx, double diry, double v0) {
    // dirx/diry: 单位方向; v0: px/s
    double rb = 1.0 - mom_drag_b;
    mom_Q = rb * mom_drag_a;
    mom_p = (2.0 - mom_drag_b) / rb;
    mom_c = pow(v0, rb) / mom_Q;
    mom_ustop = pow(1.0, rb);                       // vs = 1 px/s
    mom_u0p = pow(v0, 2.0 - mom_drag_b);
    mom_total_d = (mom_u0p - 1.0) / (mom_Q * mom_p); // vs^(2-b)=1
    mom_dir_x = dirx; mom_dir_y = diry;
    mom_last_d = 0;
    mom_start_ns = mach_ns();
    mom_begun = 0;
    mom_active = 1;
    scroll_frames_start();
}

// 滚动手势收尾(抬指或双指变单指): 发 Ended(从未滚动则 Cancelled),
// 够快则接惯性滚动, 否则补零增量 momentumEnd 阻断 Xcode 等的自带惯性
// (MMF 实测: 仅发 Ended 时部分 App 会基于最后一个 Changed 自行续滚)。
static void end_scroll_gesture(void) {
    touch_scroll_finish();
    int start_mom = 0;
    double sp = 0;
    if (momentum_on && gesture_state >= 2 && last_scrl_ms) {
        // 有些固件静止后不继续发报文: 松手前的空档也必须让速度衰减。
        // 留出 20ms 给正常的抬指报文传输, 更长的停顿按同一时间常数衰减。
        double age = fmax(0.0, (double)mach_ns() / 1e6 - last_scrl_ms);
        double decay = exp(-fmax(0.0, age - 20.0) / scroll_velocity_tau_ms);
        scrl_ema_x *= decay; scrl_ema_y *= decay;
        sp = hypot(scrl_ema_x, scrl_ema_y) * 1000.0;
        if (sp > 60.0 && age < 150.0) start_mom = 1;
    }
    if (gesture_state >= 2) {
        send_scroll_event(0, 0, 0, 0, PH_ENDED, MPH_NONE, 1);
    } else if (gesture_state == 1) {
        // 只碰未滚: Cancelled, 不触发惯性
        send_scroll_event(0, 0, 0, 0, PH_CANCELLED, MPH_NONE, 1);
        gesture_state = 0;
        scrl_ema_x = scrl_ema_y = 0;
        last_scrl_ms = 0;
        scroll_frames_stop_if_idle();
        return;
    }
    gesture_state = 0;
    if (start_mom) {
        if (debug) fprintf(stderr, "[tpkbd] MOMENTUM start v0=%.0fpx/s dur≈%.2fs dist≈%.0fpx\n",
                           sp,
                           (pow(sp, 1.0 - mom_drag_b) - 1.0) / ((1.0 - mom_drag_b) * mom_drag_a),
                           (pow(sp, 2.0 - mom_drag_b) - 1.0) /
                               ((1.0 - mom_drag_b) * mom_drag_a *
                                (2.0 - mom_drag_b) / (1.0 - mom_drag_b)));
        mom_start(scrl_ema_x * 1000.0 / sp, scrl_ema_y * 1000.0 / sp, sp);
    } else {
        send_scroll_event(0, 0, 0, 0, PH_UNDEFINED, MPH_END, 0);
        scrl_ema_x = scrl_ema_y = 0;
        last_scrl_ms = 0;
    }
    scroll_frames_stop_if_idle();
}

// C 后台程序使用 CoreVideo 显示时钟; 回调线程只通知主循环, 不碰手势状态。
// 当前 SDK 推荐的替代接口依赖 NSView/NSWindow, 本程序没有 AppKit 窗口。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static CVDisplayLinkRef scroll_display_link = NULL;
static CFRunLoopSourceRef scroll_frame_source = NULL;
static CFRunLoopTimerRef scroll_frame_fallback = NULL;
static _Atomic uint64_t scroll_display_last_ns = 0;
static int scroll_frames_running = 0;

static void scroll_frame_perform(void *context) {
    (void)context;
    if (!scroll_frames_running) return;
    touch_scroll_tick(mach_ns());
    pinch_tick(mach_ns());
    if (mom_active) mom_timer_cb(NULL, NULL);
    scroll_frames_stop_if_idle();
}

static CVReturn scroll_display_callback(CVDisplayLinkRef link, const CVTimeStamp *now,
                                       const CVTimeStamp *output, CVOptionFlags in,
                                       CVOptionFlags *out, void *context) {
    (void)link; (void)now; (void)output; (void)in; (void)out; (void)context;
    atomic_store_explicit(&scroll_display_last_ns, mach_ns(), memory_order_relaxed);
    CFRunLoopSourceSignal(scroll_frame_source);
    CFRunLoopWakeUp(CFRunLoopGetMain());
    return kCVReturnSuccess;
}

static void scroll_frame_fallback_cb(CFRunLoopTimerRef timer, void *context) {
    (void)timer; (void)context;
    uint64_t last = atomic_load_explicit(&scroll_display_last_ns, memory_order_relaxed);
    // 显示时钟不可用/热拔屏时仍能滚动, 恢复后自动回到刷新同步。
    if (!last || mach_ns() - last > 50000000) scroll_frame_perform(NULL);
}

static void scroll_frames_init(void) {
    CFRunLoopSourceContext context = {0};
    context.perform = scroll_frame_perform;
    scroll_frame_source = CFRunLoopSourceCreate(NULL, 0, &context);
    if (scroll_frame_source) {
        CFRunLoopAddSource(CFRunLoopGetMain(), scroll_frame_source, kCFRunLoopDefaultMode);
        if (CVDisplayLinkCreateWithActiveCGDisplays(&scroll_display_link) == kCVReturnSuccess) {
            if (CVDisplayLinkSetOutputCallback(scroll_display_link, scroll_display_callback, NULL)
                != kCVReturnSuccess) {
                CVDisplayLinkRelease(scroll_display_link);
                scroll_display_link = NULL;
            }
        }
    }
    scroll_scheduler_enabled = 1;
    fprintf(stderr, "[tpkbd] 滚动重采样: %.1fms, 显示时钟=%s (不可用时120Hz回退)\n",
            scroll_smooth_ms, scroll_display_link ? "就绪" : "不可用");
}

static void scroll_frames_start(void) {
    if (!scroll_scheduler_enabled || scroll_frames_running) return;
    scroll_frames_running = 1;
    atomic_store_explicit(&scroll_display_last_ns, 0, memory_order_relaxed);
    if (scroll_display_link) {
        CGEventRef event = CGEventCreate(NULL);
        CGDirectDisplayID display = CGMainDisplayID();
        uint32_t count = 0;
        if (event) {
            CGGetDisplaysWithPoint(CGEventGetLocation(event), 1, &display, &count);
            CFRelease(event);
        }
        if (!count) display = CGMainDisplayID();
        CVDisplayLinkSetCurrentCGDisplay(scroll_display_link, display);
        CVReturn result = CVDisplayLinkStart(scroll_display_link);
        static CGDirectDisplayID last_display = 0;
        if (debug || display != last_display || result != kCVReturnSuccess) {
            CVTime period = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(scroll_display_link);
            double hz = period.timeValue > 0 ? (double)period.timeScale / period.timeValue : 0;
            fprintf(stderr, "[tpkbd] 滚动显示时钟 display=%u nominal=%.1fHz start=%d\n",
                    display, hz, result);
            last_display = display;
        }
    }
    scroll_frame_fallback = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1.0/120,
                                                1.0/120, 0, 0, scroll_frame_fallback_cb, NULL);
    if (scroll_frame_fallback)
        CFRunLoopAddTimer(CFRunLoopGetMain(), scroll_frame_fallback, kCFRunLoopDefaultMode);
    else if (!scroll_display_link || !CVDisplayLinkIsRunning(scroll_display_link)) {
        // 两种时钟都失败时恢复直通, 避免无输出。
        touch_scroll_finish();
        pinch_finish(1);
        mom_active = 0;
        scroll_frames_stop_if_idle();
    }
}

static void scroll_frames_stop_if_idle(void) {
    if (!scroll_frames_running || touch_resampling || pinch_active || mom_active) return;
    scroll_frames_running = 0;
    if (scroll_display_link) CVDisplayLinkStop(scroll_display_link);
    if (scroll_frame_fallback) {
        CFRunLoopTimerInvalidate(scroll_frame_fallback);
        CFRelease(scroll_frame_fallback);
        scroll_frame_fallback = NULL;
    }
}
#pragma clang diagnostic pop

// ---- 小红点中键: 按住+拨动=丝滑滚动, 单击=可配(默认无动作) ----
// 机制: 会话级过滤型鼠标 tap。中键按下先吞掉、语义待定:
//   - 按住期间位移累计过阈值 → 进入滚动模式, 指针位移事件被吞并转成
//     手势相位滚动(复用双指滚动全套: 相位流+EMA+惯性)。
//   - 未拨动就松开 = 干净单击 → 默认无动作;
//     TPKB_TP_CLICK=terminal 打开 Terminal / native 补发原生中键。
//   - 松开后 200ms 继续吞位移: 小红点弹簧回中会产生反向增量, 不吞会拉扯光标。
// 已知边界(实证 2026-10-16):
//  1) 事件 tap 分不清事件来自哪个指针设备 — 中键按住期间系统内一切指针
//     移动都会变成滚动(含本驱动合成的触摸板移动)。
//  2) WindowServer 在事件到达会话层 tap 之前就更新了光标 — 吞事件拦不住
//     光标位移, 中键滚动期间光标会跟着小幅漂移。试过逐帧补发反向事件抵消:
//     光标仍漂且闪烁, 已回退, 勿再试。彻底根治需抢占 HID 设备(seize), 暂不做。
//  3) tap 超时被系统禁用时自动重启, 期间事件直通(回落为原生行为, 不会卡死)。
static int       tp_scroll_on = 1;      // TPKB_TP_SCROLL, 0=整个功能关闭(tap 不建)
static float     tp_gain = 2.5f;        // TPKB_TP_GAIN: 小红点位移→滚动像素增益
static int       tp_click_action = 0;   // TPKB_TP_CLICK: 0=无动作(默认)/1=Terminal/2=原生中键
static int       tp_armed = 0;          // 中键按住中(位移未过阈值)
static int       tp_scrolling = 0;      // 已进入滚动模式
static int       tp_acc = 0;            // 按住期间累计位移
static int       tp_repost = 0;         // none 模式补发原生中键的透传计数
static int       tp_diag = 0;           // 诊断日志计数(每手势前 30 条)
static uint64_t  tp_settle_until = 0;   // 松开后的回中吞咽窗口
static uint64_t  tp_last_ms = 0;        // 上一帧时刻(EMA 用)
static CFMachPortRef mouse_tap_port = NULL;

static void spawn_open_terminal(void) {
    extern char **environ;
    pid_t pid;
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    char *argv[] = { (char *)"/usr/bin/open", (char *)"-a", (char *)"Terminal", NULL };
    int r = posix_spawn(&pid, "/usr/bin/open", NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (r == 0) {
        int st = 0; waitpid(pid, &st, 0);
        if (debug) fprintf(stderr, "[tpkbd] TP click → Terminal (st=%d)\n", st);
    } else if (debug) {
        fprintf(stderr, "[tpkbd] TP click → open 启动失败 (errno=%d)\n", r);
    }
}

static CGEventRef mouse_tap_cb(CGEventTapProxy proxy, CGEventType type,
                               CGEventRef event, void *userInfo) {
    (void)proxy; (void)userInfo;
    if (type == kCGEventTapDisabledByTimeout) {
        if (mouse_tap_port) CGEventTapEnable(mouse_tap_port, true);
        return event;
    }
    if (type == kCGEventTapDisabledByUserInput) {
        // 在回调内发事件可能触发系统禁用 tap; 诊断期记录并立即重启
        fprintf(stderr, "[tpkbd] TPD tap disabled-by-user-input, re-enabling\n");
        if (mouse_tap_port) CGEventTapEnable(mouse_tap_port, true);
        return event;
    }

    if (type == kCGEventOtherMouseDown || type == kCGEventOtherMouseUp) {
        long btn = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
        if (btn != 2) return event;
        if (tp_repost) { tp_repost--; return event; }   // none 模式补发的原生中键
        if (type == kCGEventOtherMouseDown) {
            mom_stop();                   // 新一轮中键手势截停惯性
            tp_armed = 1; tp_scrolling = 0; tp_acc = 0; tp_last_ms = 0;
            tp_diag = 0;
            fprintf(stderr, "[tpkbd] TPD mid-down seen (armed)\n");
            return NULL;                  // 语义待定: 拨动=滚动, 干净松开=单击
        }
        fprintf(stderr, "[tpkbd] TPD mid-up: scrolled=%d acc=%d\n", tp_scrolling, tp_acc);
        if (tp_scrolling) end_scroll_gesture();   // Ended + 够快接惯性
        else if (tp_armed && tp_click_action == 1) spawn_open_terminal();
        else if (tp_armed && tp_click_action == 2) {
            // none: 补发原生中键(浏览器新标签页等); 计数防重入
            CGPoint p = CGEventGetLocation(event);
            CGEventRef d = CGEventCreateMouseEvent(NULL, kCGEventOtherMouseDown, p, 2);
            CGEventRef u = CGEventCreateMouseEvent(NULL, kCGEventOtherMouseUp, p, 2);
            tp_repost = 2;
            if (d) { CGEventPost(g_tap, d); CFRelease(d); }
            if (u) { CGEventPost(g_tap, u); CFRelease(u); }
        }
        tp_armed = 0; tp_scrolling = 0; tp_acc = 0;
        tp_settle_until = now_ms() + 200;
        return NULL;
    }

    if (type != kCGEventMouseMoved && type != kCGEventLeftMouseDragged &&
        type != kCGEventRightMouseDragged && type != kCGEventOtherMouseDragged)
        return event;
    long dx = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
    long dy = CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);
    if (debug && tp_diag < 30) {
        fprintf(stderr, "[tpkbd] TPD move type=%d d=(%ld,%ld) armed=%d scrolling=%d\n",
                (int)type, dx, dy, tp_armed, tp_scrolling);
        tp_diag++;
    }
    if (!tp_armed && !tp_scrolling) {
        if (tp_settle_until && now_ms() < tp_settle_until) return NULL;
        tp_settle_until = 0;
        return event;                     // 平时零干预
    }
    if (!tp_scrolling) {
        tp_acc += (int)(labs(dx) + labs(dy));
        if (tp_acc < 8) return NULL;      // 微抖吞掉
        tp_scrolling = 1;
        gesture_send_maybegin();
    }
    // 拨下=内容下滚(与触控板自然滚动同向); 帧增量转滚动 + 维护惯性 EMA
    double sdx = (double)dx * tp_gain, sdy = (double)dy * tp_gain;
    uint64_t nms = now_ms();
    double dtms = tp_last_ms ? (double)(nms - tp_last_ms) : 10.0;
    if (dtms <= 0) dtms = 10.0;
    scrl_ema_x = scrl_ema_x * 0.7 + ((natural ? sdx : -sdx) / dtms) * 0.3;
    scrl_ema_y = scrl_ema_y * 0.7 + ((natural ? sdy : -sdy) / dtms) * 0.3;
    last_scrl_ms = nms;
    tp_last_ms = nms;
    post_gesture_scroll(sdx, sdy);
    if (debug) fprintf(stderr, "[tpkbd] TPSC d=(%ld,%ld) v=%.0fpx/s\n",
                       dx, dy, hypot(scrl_ema_x, scrl_ema_y) * 1000.0);
    return NULL;
}

// ---- 三指左右滑: 切换全屏应用/桌面空间 (Dock Swipe) ----
// 移植自 Mac Mouse Fix (TouchSimulator.m + ModifiedDragOutputThreeFingerSwipe.m),
// macOS 27 路径: CGEvent 字段被系统忽略, 必须构造私有 IOHIDEvent 挂进 CGEvent。
//   IOHIDEvent(type=23 DockSwipe): motion(0x170001)=1水平/2垂直,
//     flavor(0x170005)=3 DockPrimary, progress(0x170002)=累计偏移, phase;
//   结束时附加 type=9 Velocity 子事件(X/Y=exitSpeed=lastDelta×100);
//   挂载: CGEvent+0x18 解引用再 +0xd0 处写指针(MMF 同款, 27.0.1 验证一致);
//   Ended/Cancelled 事件在 200ms/500ms 后重发, 防 Dock 偶发卡住(MMF stuck-bug)。
// 私有符号全部运行时 dlsym, 已在本机逐一验证。
typedef struct __TPHIDEvent *TPHIDEventRef;
static TPHIDEventRef (*hid_create)(void *, int, uint64_t, uint64_t, int);
static void (*hid_set_int)(TPHIDEventRef, int, int);
static void (*hid_set_dbl)(TPHIDEventRef, int, double);
static void (*hid_set_phase)(TPHIDEventRef, int);
static void (*hid_append)(TPHIDEventRef, TPHIDEventRef, int);

// CGS 私有 API: 数主屏的 Space 数, 决定"滑满一屏 = 切一个空间"的缩放系数
static int (*cgs_main_conn)(void);
static CFArrayRef (*cgs_copy_spaces)(int);
static CFStringRef (*cgs_uuid_from_id)(uint32_t);
static CFStringRef g_cgs_main_uuid = NULL;   // 缓存主屏 UUID

static void hid_api_init(void) {
    void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (!h) return;
    hid_create    = (TPHIDEventRef (*)(void *, int, uint64_t, uint64_t, int))dlsym(h, "IOHIDEventCreate");
    hid_set_int   = (void (*)(TPHIDEventRef, int, int))dlsym(h, "IOHIDEventSetIntegerValue");
    hid_set_dbl   = (void (*)(TPHIDEventRef, int, double))dlsym(h, "IOHIDEventSetDoubleValue");
    hid_set_phase = (void (*)(TPHIDEventRef, int))dlsym(h, "IOHIDEventSetPhase");
    hid_append    = (void (*)(TPHIDEventRef, TPHIDEventRef, int))dlsym(h, "IOHIDEventAppendEvent");
    void *s = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY);
    if (s) {
        cgs_main_conn    = (int (*)(void))dlsym(s, "CGSMainConnectionID");
        cgs_copy_spaces  = (CFArrayRef (*)(int))dlsym(s, "CGSCopyManagedDisplaySpaces");
        cgs_uuid_from_id = (CFStringRef (*)(uint32_t))dlsym(s, "CGSDisplayUUIDFromDisplayID");
    }
    if (debug) fprintf(stderr, "[tpkbd] dockswipe API: hid_create=%p set_phase=%p cgs=%p/%p\n",
                       (void *)hid_create, (void *)hid_set_phase,
                       (void *)cgs_main_conn, (void *)cgs_copy_spaces);
}

// Ended/Cancelled 事件重发定时器(stuck-bug 缓解)
static CGEventRef swipe3_resend_evt = NULL;
static CFRunLoopTimerRef swipe3_rt1 = NULL, swipe3_rt2 = NULL;

static void swipe3_resend_cancel(void) {
    if (swipe3_rt1) { CFRunLoopRemoveTimer(CFRunLoopGetMain(), swipe3_rt1, kCFRunLoopCommonModes); CFRelease(swipe3_rt1); swipe3_rt1 = NULL; }
    if (swipe3_rt2) { CFRunLoopRemoveTimer(CFRunLoopGetMain(), swipe3_rt2, kCFRunLoopCommonModes); CFRelease(swipe3_rt2); swipe3_rt2 = NULL; }
    if (swipe3_resend_evt) { CFRelease(swipe3_resend_evt); swipe3_resend_evt = NULL; }
}

static void swipe3_resend_cb(CFRunLoopTimerRef t, void *info) {
    (void)info;
    if (swipe3_resend_evt) CGEventPost(kCGSessionEventTap, swipe3_resend_evt);
    if (t == swipe3_rt2 && swipe3_rt2) {   // 最后一次重发: 全部清理
        swipe3_resend_cancel();
    }
}

static void swipe3_resend_schedule(CGEventRef e30) {
    swipe3_resend_cancel();
    CFRetain(e30);
    swipe3_resend_evt = e30;
    CFRunLoopTimerContext ctx = {0};
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    swipe3_rt1 = CFRunLoopTimerCreate(kCFAllocatorDefault, now + 0.2, 0, 0, 0, swipe3_resend_cb, &ctx);
    swipe3_rt2 = CFRunLoopTimerCreate(kCFAllocatorDefault, now + 0.5, 0, 0, 0, swipe3_resend_cb, &ctx);
    if (swipe3_rt1) CFRunLoopAddTimer(CFRunLoopGetMain(), swipe3_rt1, kCFRunLoopCommonModes);
    if (swipe3_rt2) CFRunLoopAddTimer(CFRunLoopGetMain(), swipe3_rt2, kCFRunLoopCommonModes);
}

static void post_dock_swipe_event(int phase, int motion_type, double stored_progress,
                                  double exit_speed, int attach_velocity) {
    if (!hid_create || !hid_set_phase || !hid_set_int || !hid_set_dbl) return;
    TPHIDEventRef ev = hid_create(NULL, 23 /*DockSwipe*/, 0, 0, 0);
    if (!ev) return;
    hid_set_phase(ev, phase);
    hid_set_int(ev, 0x170001, motion_type);        // motion
    hid_set_int(ev, 0x170005, 3);                  // flavor = DockPrimary
    hid_set_dbl(ev, 0x170002, stored_progress);    // progress
    if (attach_velocity && hid_append) {
        TPHIDEventRef v = hid_create(NULL, 9 /*Velocity*/, 0, 0, 0);
        if (v) {
            hid_set_dbl(v, 0x90000, exit_speed);   // VelocityX
            hid_set_dbl(v, 0x90001, exit_speed);   // VelocityY
            hid_set_dbl(v, 0x90002, 0.0);          // VelocityZ
            hid_append(ev, v, 0);
        }
    }
    CGEventRef e30 = CGEventCreate(NULL);
    if (!e30) return;
    CGEventSetType(e30, 30);
    // 挂载 IOHIDEvent: 直接写 CGEvent 内部结构(MMF 同款内存偏移)
    void *rec = *(void **)((uint8_t *)e30 + 0x18);
    if (rec) {
        CFRetain(ev);   // 事件对象可能在重发定时器里再活 0.5s(少量泄漏, 同 MMF)
        *(TPHIDEventRef *)((uint8_t *)rec + 0xd0) = ev;
    }
    CGEventPost(kCGSessionEventTap, e30);
    if (phase == PH_ENDED || phase == PH_CANCELLED) swipe3_resend_schedule(e30);
    CFRelease(e30);
    CFRelease(ev);
}

// 桌面手势沿用旧的 swipe3 名称与配置; 三指拖移开启时改由四指触发。
static int drag3_on = 1;                     // TPKB_DRAG3
static ThreeFingerDrag three_drag;
static CGPoint three_drag_cursor;
static int system_swipe_tail = 0;
static int    swipe3_on = 1;                 // TPKB_SWIPE3
static double swipe3_gain = 1.0;             // TPKB_SWIPE3_GAIN 速度倍率
static int    swipe3_active = 0;             // 三指在板
static int    swipe3_sent = 0;               // 已发 Began
static int    swipe3_axis = 0;               // 0未锁 1水平 2垂直
static int    swipe3_anchor_x = 0, swipe3_anchor_y = 0;
static int    swipe3_last_x = 0, swipe3_last_y = 0;
static double swipe3_scale = 0;              // 每像素→dockswipe 进度(水平)
static double swipe3_scale_v = 0;            // 垂直: MMF 直接 1/屏高, 满屏滑=进度1.0
static double swipe3_origin = 0;             // 累计偏移(未翻转, MMF _dockSwipeOriginOffset)
static double swipe3_last_d = 0;

static int cgs_space_count_main(void) {
    if (!cgs_copy_spaces || !cgs_main_conn) return -1;
    CFArrayRef arr = cgs_copy_spaces(cgs_main_conn());
    if (!arr) return -1;
    if (!g_cgs_main_uuid && cgs_uuid_from_id)
        g_cgs_main_uuid = cgs_uuid_from_id(CGMainDisplayID());
    int n = -1;
    for (CFIndex i = 0; i < CFArrayGetCount(arr); i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(arr, i);
        if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID()) continue;
        CFArrayRef spaces = CFDictionaryGetValue(d, CFSTR("Spaces"));
        if (!spaces || CFGetTypeID(spaces) != CFArrayGetTypeID()) continue;
        CFStringRef ident = CFDictionaryGetValue(d, CFSTR("Display Identifier"));
        if (!g_cgs_main_uuid || (ident && CFGetTypeID(ident) == CFStringGetTypeID() &&
                                 CFStringCompare(ident, g_cgs_main_uuid, 0) == kCFCompareEqualTo)) {
            n = (int)CFArrayGetCount(spaces);
            break;
        }
    }
    CFRelease(arr);
    return n;
}

// MMF 实测公式: 满屏滑动切一个空间; originOffsetForOneSpace 随空间数微调
static double swipe3_scale_compute(void) {
    int n = cgs_space_count_main();
    double offset1 = (n <= 1) ? 2.0 : 1.0 + 1.0 / (n - 1);
    CGRect mb = CGDisplayBounds(CGMainDisplayID());
    return offset1 / (mb.size.width + 63.0);
}

static void swipe3_begin(int cx, int cy) {
    swipe3_resend_cancel();
    swipe3_sent = 0;
    swipe3_axis = 0;
    swipe3_origin = 0;
    swipe3_last_d = 0;
    swipe3_anchor_x = swipe3_last_x = cx;
    swipe3_anchor_y = swipe3_last_y = cy;
    swipe3_scale = swipe3_scale_compute() * swipe3_gain;
    // 垂直(Mission Control/App Exposé): MMF 用 1/主屏高, 不用空间数公式
    swipe3_scale_v = (1.0 / CGDisplayBounds(CGMainDisplayID()).size.height) * swipe3_gain;
    swipe3_active = 1;
}

static void swipe3_move(int cx, int cy) {
    if (!swipe3_axis) {
        // 轴锁定: 累计位移过阈值后按主方向定轴(水平=切空间, 垂直=调度中心)
        if (abs(cx - swipe3_anchor_x) >= 10 || abs(cy - swipe3_anchor_y) >= 10) {
            swipe3_axis = (abs(cx - swipe3_anchor_x) >= abs(cy - swipe3_anchor_y)) ? 1 : 2;
            if (debug) fprintf(stderr, "[tpkbd] SWIPE3 axis=%d\n", swipe3_axis);
        }
        swipe3_last_x = cx; swipe3_last_y = cy;
        return;
    }
    int dcnt;
    double d;
    int motion_type = swipe3_axis;
    if (swipe3_axis == 1) {
        dcnt = cx - swipe3_last_x;
        double dxp = dcnt * move_gain;                // 指针等效像素
        d = -dxp * swipe3_scale;                      // MMF: delta = -deltaX × scaleH
    } else {
        dcnt = cy - swipe3_last_y;
        double dyp = dcnt * move_gain;
        d = dyp * swipe3_scale_v;                     // MMF: delta = +deltaY × (1/屏高)
    }
    swipe3_last_x = cx; swipe3_last_y = cy;
    if (d == 0) return;
    int phase;
    if (!swipe3_sent) { swipe3_origin = d; phase = PH_BEGAN; swipe3_sent = 1; }
    else             { swipe3_origin += d; phase = PH_CHANGED; }
    // MMF: invertedFromDevice=natural, 存入前把累计偏移翻回设备方向
    double stored = natural ? -swipe3_origin : swipe3_origin;
    post_dock_swipe_event(phase, motion_type, stored, 0, 0);
    swipe3_last_d = d;
    if (debug) fprintf(stderr, "[tpkbd] SWIPE3 axis=%d d=%.5f origin=%.3f stored=%.3f\n",
                       swipe3_axis, d, swipe3_origin, stored);
}

static void swipe3_end(int cancelled) {
    if (!swipe3_active) return;
    swipe3_active = 0;
    if (swipe3_sent && swipe3_axis) {
        // 末帧方向与总位移同号 → Ended, 反号(回拉取消) → Cancelled (MMF 规则)
        int end_phase = cancelled ? PH_CANCELLED : PH_ENDED;
        if (swipe3_origin != 0 && swipe3_last_d != 0 &&
            (swipe3_last_d > 0) != (swipe3_origin > 0)) end_phase = PH_CANCELLED;
        double exit_speed = swipe3_last_d * 100.0;
        double stored = natural ? -swipe3_origin : swipe3_origin;
        if (debug) fprintf(stderr, "[tpkbd] SWIPE3 end axis=%d phase=%d exit=%.1f\n",
                           swipe3_axis, end_phase, exit_speed);
        post_dock_swipe_event(end_phase, swipe3_axis, stored, exit_speed, 1);
    }
    swipe3_sent = 0;
    swipe3_axis = 0;
}

static CGPoint cursor_pos(void) {
    CGEventRef e = CGEventCreate(NULL);
    CGPoint p = CGEventGetLocation(e);
    if (e) CFRelease(e);
    return p;
}

static void three_drag_release(void) {
    if (three_drag.phase == DRAG3_ACTIVE) post_click(three_drag_cursor, 0, 1);
    three_drag = (ThreeFingerDrag){0};
}

static void release_pointer_buttons(void) {
    three_drag_release();
    if (btn_prev) {
        if (btn_type) post_right_click(cursor_pos(), 0, 1);
        else post_click(cursor_pos(), 0, click_count);
    }
    if (rel_btn_prev) post_click(cursor_pos(), 0, 1);
    btn_prev = rel_btn_prev = release_streak = drag_armed = 0;
}

static void clamp_to_screen(int *x, int *y) {
    if (*x < screen_bounds.origin.x) *x = (int)screen_bounds.origin.x;
    if (*y < screen_bounds.origin.y) *y = (int)screen_bounds.origin.y;
    if (*x > screen_bounds.origin.x + screen_bounds.size.width  - 1)
        *x = (int)(screen_bounds.origin.x + screen_bounds.size.width  - 1);
    if (*y > screen_bounds.origin.y + screen_bounds.size.height - 1)
        *y = (int)(screen_bounds.origin.y + screen_bounds.size.height - 1);
}

static int handle_three_drag(const DragContact *contacts, int count,
                              uint16_t scan, int swallow, int click) {
    Drag3Output out = drag3_update(&three_drag, contacts, count, scan, mach_ns(),
                                  drag3_on, swallow || btn_prev, click,
                                  move_gain, move_acceleration);
    if (!out.consume) return 0;
    if (scroll_active) {
        scrl_ema_x = scrl_ema_y = 0;
        end_scroll_gesture();
        scroll_active = 0;
    }
    two_finger.mode = TWO_NONE;
    have_last = 0;
    tap_moved = tap_pointer_moved = 1;
    touch_now = swallow ? 0 : count > 0;
    // CGEventPost is asynchronous. Integrate from our last posted position,
    // otherwise 140Hz reports can read an older system cursor and lose travel.
    CGPoint cur = three_drag_cursor;
    if (out.begin) {
        // Dragging always selects from the current position, never as the
        // second/third click in a preceding click sequence.
        last_down_ms = 0;
        cur = cursor_pos();
        post_click(cur, 1, 1);
    }
    if (out.dx || out.dy) {
        int x = (int)cur.x + out.dx, y = (int)cur.y + out.dy;
        int intended_x = x, intended_y = y;
        clamp_to_screen(&x, &y);
        avoid_gap(&x, &y, (int)cur.x, (int)cur.y);
        if (x != intended_x) three_drag.motion.remainder_x = 0;
        if (y != intended_y) three_drag.motion.remainder_y = 0;
        if (x != (int)cur.x || y != (int)cur.y) post_mouse_drag(CGPointMake(x, y));
        cur = CGPointMake(x, y);
    }
    three_drag_cursor = cur;
    if (out.end) post_click(cur, 0, 1);
    if (!count) {
        tap_active = tap_nfid = 0;
        type_grandf = 0;
    }
    return 1;
}

// 相对位移: 在当前光标位置上叠加 (dx,dy)
static void handle_report(uint32_t cb_rid, uint8_t *r, CFIndex len) {
    if (probe_mode) {
        printf("cbrid=%u r0=0x%02x len=%d:", cb_rid, r[0], (int)len);
        for (int i = 0; i < len && i < 32; i++) printf(" %02x", r[i]);
        printf("\n");
        fflush(stdout);
        return;
    }

    if (debug) {
        fprintf(stderr, "[tpkbd] RX cbrid=%u r[0]=0x%02x len=%d:", cb_rid, r[0], (int)len);
        int lim = (cb_rid == REPORT_ID_TOUCH) ? (int)len : 9;  // 0x03 全量转储
        for (int i = 1; i < len && i < lim; i++) fprintf(stderr, " %02x", r[i]);
        fprintf(stderr, "\n");
    }

    // ---- 触摸板相对鼠标模式(兜底, cb_rid=0x02) ----
    //   digitizer 模式生效后 disable_rel=1, 此分支不再进入。
    //   注意: 小红点发 0x10, 由 macOS 原生驱动, 本程序不处理(避免双倍速)。
    if (cb_rid == (uint32_t)rel_rid) {
        if (disable_rel) return;   // digitizer 模式已生效, 忽略相对报文
        if (len < 4) return;
        int buttons = r[1] & 0x07;
        int dx = (int)(int8_t)r[2];
        int dy = (int)(int8_t)r[3];
        if (debug) {
            fprintf(stderr, "[tpkbd] rel len=%d raw=%02x %02x %02x %02x btn=%d dx=%d dy=%d\n",
                    (int)len, r[0], r[1], r[2], r[3], buttons, dx, dy);
        }
        if (rel_y_invert) dy = -dy;
        int left = buttons & 1;

        // 1) 先处理按键状态变化, 保证点按即时响应(down/up 都在当前光标位置)
        if (left != rel_btn_prev) {
            post_click(cursor_pos(), left, 1);
            rel_btn_prev = left;
        }

        // 2) 再处理移动: 按住左键时发拖拽事件(框选/拖动), 否则发普通移动
        if (dx || dy) {
            CGPoint p = cursor_pos();
            int nx = (int)p.x + (int)(dx * rel_gain);
            int ny = (int)p.y + (int)(dy * rel_gain);
            clamp_to_screen(&nx, &ny);
            avoid_gap(&nx, &ny, (int)p.x, (int)p.y);
            CGPoint np = CGPointMake(nx, ny);
            if (rel_btn_prev) post_mouse_drag(np);
            else              post_mouse_move(np);
        }
        return;
    }

    if (cb_rid != REPORT_ID_TOUCH || len < 30) return;

    // 收到首个 digitizer 报文说明 Input Mode 切换成功, 自动关掉相对模式
    // 避免 0x02 与 0x03 同时驱动造成双倍速。
    if (!disable_rel) {
        disable_rel = 1;
        fprintf(stderr, "[tpkbd] ✅ 收到首个 digitizer 报文(cb_rid=0x03), Input Mode 生效, 关闭相对模式\n");
    }

    int tips[NFINGER]     = {0};
    int fx[NFINGER]       = {0};
    int fy[NFINGER]       = {0};
    int contact_id[NFINGER] = {0};
    int ntips = 0;

    // 缓冲区带 report-ID 前缀: r[0]=0x03, 数据从 r[1] 起。
    // 每指 5 字节: [flags][X lo][X hi][Y lo][Y hi]
    //   flags: bit0=confidence(1=真手指) bit1=tip switch(1=接触) bit2..4=contact id
    for (int i = 0; i < NFINGER; i++) {
        uint8_t *b = r + 1 + 5 * i;
        int confidence = b[0] & 1;          // bit0: 1=真手指, 0=手掌/误触
        int tip = (b[0] >> 1) & 1;          // bit1: TipSwitch, 1=接触
        if (!confidence) tip = 0;           // 过滤手掌误触
        contact_id[i] = (b[0] >> 2) & 7;
        fx[i] = b[1] | (b[2] << 8);
        fy[i] = b[3] | (b[4] << 8);
        // 接触沿检测, 维护按下顺序
        if (tip && !tip_state[i]) down_seq[i] = ++seq_counter;
        if (!tip) down_seq[i] = 0;
        tip_state[i] = tip;
        tips[i] = tip;
        if (tip) ntips++;
    }

    if (scroll_trace_start) {
        int mask = 0, sx = 0, sy = 0;
        for (int i = 0; i < NFINGER; i++) if (tips[i]) {
            mask |= 1 << i; sx += fx[i]; sy += fy[i];
        }
        scroll_trace_record('R', ntips, mask, ntips ? sx * 2 / ntips : 0,
                            ntips ? sy * 2 / ntips : 0, r[26] | (r[27] << 8), r[29] & 1);
    }

    int click = r[29] & 1;   // 物理按压(板面下压); r[28] 是接触计数,别读错

    // ---- 打字锁定评估: 敲键后 typelock_ms 内吞掉"新"触摸 ----
    // 已免锁(type_grandf)的在板手势照常处理; 物理按键见文件尾只挡新按下。
    int swallow = 0;
    if (typelock_ms > 0 && last_key_ms &&
        now_ms() - last_key_ms < (uint64_t)typelock_ms) {
        swallow = !type_grandf;
        if (!type_lock_on) {
            type_lock_on = 1;
            if (debug) fprintf(stderr, "[tpkbd] TYPELOCK engage (+%llums grandf=%d)\n",
                               (unsigned long long)(now_ms() - last_key_ms), type_grandf);
        }
    } else if (type_lock_on) {
        type_lock_on = 0;
        have_last = 0;   // 解锁首帧重新锚定, 防止用陈旧坐标跳变
        if (ntips > 0) {
            // 解锁瞬间板上还有残留接触(如手掌): 标记为"已位移的旧接触",
            // 它抬起时不会被误判成轻点点按。
            tap_active = 1; tap_nfid = ntips; tap_moved = 1; tap_down_ms = 0;
        }
        if (debug) fprintf(stderr, "[tpkbd] TYPELOCK release\n");
    }

    // 轻点判定跟踪: 触摸起点/时长/峰值指数/主指位移 (锁定期不跟踪)
    if (ntips > 0 && !swallow) {
        int prim = -1, pseq = 0x7fffffff;
        for (int i = 0; i < NFINGER; i++)
            if (tips[i] && down_seq[i] > 0 && down_seq[i] < pseq) { pseq = down_seq[i]; prim = i; }
        if (prim >= 0) {
            if (!tap_active) {
                tap_active = 1;
                tap_nfid = 0;
                tap_moved = 0;
                tap_pointer_moved = 0;
                tap_down_ms = now_ms();
                tap_anchor_x = fx[prim]; tap_anchor_y = fy[prim];
            }
            if (ntips > tap_nfid) tap_nfid = ntips;
            if (outside_move_slop(fx[prim] - tap_anchor_x, fy[prim] - tap_anchor_y))
                tap_moved = 1;
        }
    }

    // 惯性滚动中: 任何新接触或物理按压都立即截停(同 macOS 触控板行为)
    if (mom_active && (ntips > 0 || click)) mom_stop();

    if (pinch_active && (ntips != 2 || click || btn_prev || swallow || !pinch_on)) {
        pinch_finish(ntips >= 3 || click || btn_prev || swallow || !pinch_on);
        pinch_tail = ntips == 1;
    }

    int swipe_fingers = drag3_on ? 4 : 3;
    if (swipe3_active && (ntips < swipe_fingers || !swipe3_on ||
                          swallow || click || btn_prev)) {
        swipe3_end(!swipe3_on || swallow || click || btn_prev);
        system_swipe_tail = ntips > 0 || click;
    }
    // A four-finger lift passes through three/two/one contacts. Those tail
    // reports belong to the swipe, not to a new drag, zoom or scroll.
    if (system_swipe_tail) {
        have_last = 0;
        tap_moved = tap_pointer_moved = 1;
        touch_now = swallow ? 0 : ntips > 0;
        if (!ntips && !click) {
            system_swipe_tail = 0;
            tap_active = tap_nfid = type_grandf = 0;
        }
        return;
    }
    DragContact drag_contacts[NFINGER];
    int drag_count = 0;
    for (int i = 0; i < NFINGER; i++) if (tips[i])
        drag_contacts[drag_count++] = (DragContact){contact_id[i], fx[i], fy[i]};
    if (handle_three_drag(drag_contacts, drag_count, r[26] | (r[27] << 8), swallow, click))
        return;
    if (drag3_on && ntips >= 4 && !swipe3_on && !btn_prev) {
        // Disabling desktop swipes does not turn four fingers into scrolling,
        // nor may their staggered lift start a three-finger drag.
        if (scroll_active) {
            scrl_ema_x = scrl_ema_y = 0;
            end_scroll_gesture();
            scroll_active = 0;
        }
        system_swipe_tail = 1;
        have_last = 0;
        tap_moved = tap_pointer_moved = 1;
        touch_now = swallow ? 0 : 1;
        return;
    }
    if (ntips != 1) pinch_tail = 0;
    if (pinch_tail && !click && !btn_prev && !swallow) {
        have_last = 0; touch_now = 1; tap_moved = 1;
        return;
    }
    if (ntips < 2 && !scroll_active) two_finger.mode = TWO_NONE;
    if (ntips >= 3 || click || btn_prev || swallow) {
        two_finger.mode = TWO_NONE;
        if (click || btn_prev || swallow) scrl_ema_x = scrl_ema_y = 0;
    }

    // 双指先后抬起会经历 2→1→0。旧逻辑在 2→1 启动惯性, 下一帧仍有
    // 一指便立即截停。给剩余手指 60ms 抬起窗口, 等全部离板再决定惯性。
    // 窗口内不把残留手指误当指针移动; 超时则回到单指移动, 不启动惯性。
    if (scroll_active && ntips == 1 && !swallow && !btn_prev && !click) {
        if (!scroll_lift_ms) scroll_lift_ms = now_ms();
        if (now_ms() - scroll_lift_ms < 60) {
            have_last = 0;
            touch_now = 1;
            return;
        }
        scrl_ema_x = scrl_ema_y = 0;
    } else {
        if (scroll_lift_ms && (now_ms() - scroll_lift_ms >= 60 || click || swallow))
            scrl_ema_x = scrl_ema_y = 0;
        scroll_lift_ms = 0;
    }

    if (debug) {
        fprintf(stderr, "[tpkbd] DG ntips=%d click=%d f0=(%d,%d)tip%d f1=(%d,%d)tip%d\n",
                ntips, click, fx[0], fy[0], tips[0], fx[1], fy[1], tips[1]);
    }

    CGPoint cur = cursor_pos();

    if (ntips >= swipe_fingers && swipe3_on && !swallow && !btn_prev && !click) {
        // ---- 四指(兼容模式为三指): 切空间/调度中心 (dock swipe) ----
        // 物理按住期间不进此分支: 按住时一切触摸都是拖移(同原生触控板)。
        // 质心 = 最早按下的四指(或兼容模式下的三指)
        int pick[NFINGER], npick = 0, c3x = 0, c3y = 0;
        for (int rank = 0; rank < swipe_fingers; rank++) {
            int best = -1, best_seq = 0x7fffffff;
            for (int i = 0; i < NFINGER; i++) {
                if (!tips[i] || down_seq[i] <= 0) continue;
                int dup = 0;
                for (int j = 0; j < npick; j++) if (pick[j] == i) dup = 1;
                if (dup) continue;
                if (down_seq[i] < best_seq) { best_seq = down_seq[i]; best = i; }
            }
            if (best < 0) break;
            pick[npick++] = best;
        }
        for (int j = 0; j < npick; j++) { c3x += fx[pick[j]]; c3y += fy[pick[j]]; }
        if (npick > 0) {
            c3x /= npick; c3y /= npick;
            if (!swipe3_active) {
                // 滚动中途加第三指: 先收尾, 并清 EMA 防误触发惯性
                if (scroll_active) { scrl_ema_x = scrl_ema_y = 0; end_scroll_gesture(); }
                scroll_active = 0;
                swipe3_begin(c3x, c3y);
            } else {
                swipe3_move(c3x, c3y);
            }
        }
        have_last = 0;
        touch_now = 1;
    } else if (ntips == 0 && !swallow) {
        // 双指抬起瞬间: 发 Ended 收尾; 甩动够快则接惯性滚动物理曲线
        // (先记住本轮是否真滚过 — end_scroll_gesture 会复位 gesture_state)
        int tap_scrolled = (gesture_state >= 2 || (touch_resampling &&
                            (touch_resampler.x != 0 || touch_resampler.y != 0)));
        if (scroll_active) end_scroll_gesture();
        else { scrl_ema_x = scrl_ema_y = 0; }
        have_last = 0;
        scroll_active = 0;
        two_finger.mode = TWO_NONE;

        // ---- 轻点点按判定 (抬指时回溯评估) ----
        // 条件: 时长<阈值 + 两轴位移均在 move_slop 内 + 未移动光标/滚动。
        // 双指滚动哪怕只滚了 1px(gesture_state 到过 2), 两指轻点即作废,
        // 否则轻蹭一下就会弹右键菜单。
        if (tap_active && tap_on && btn_prev == 0 &&
            (tap_nfid == 1 || (tap_nfid == 2 && tap_right_on)) && !tap_moved && !tap_pointer_moved &&
            !tap_scrolled &&
            now_ms() - tap_down_ms < (uint64_t)tap_ms_thresh) {
            if (tap_nfid == 1) {
                int c = click_count_for(cur);
                post_click(cur, 1, c);
                post_click(cur, 0, c);
                if (debug) fprintf(stderr, "[tpkbd] TAP left count=%d\n", c);
            } else {
                post_right_click(cur, 1, 1);
                post_right_click(cur, 0, 1);
                if (debug) fprintf(stderr, "[tpkbd] TAP right\n");
            }
        }
        tap_active = 0; tap_nfid = 0; tap_moved = 0;
        type_grandf = 0;   // 在板手势结束, 后续新触摸按锁定规则处理
    } else if (ntips >= 2 && !swallow && !btn_prev && !click) {
        // ---- 双指分流: 平移滚动、改变间距捏合; 按 ContactID 跟踪 ----
        // (物理按住期间让位给拖移, 不滚动)
        int s1 = -1, s2 = -1;
        for (int k = 0; k < 2; k++) {
            int best = -1, best_seq = 0x7fffffff;
            for (int i = 0; i < NFINGER; i++) {
                if (tips[i] && down_seq[i] > 0 &&
                    (k == 0 || i != s1) && down_seq[i] < best_seq) {
                    if (k == 1 && i == s1) continue;
                    best_seq = down_seq[i];
                    best = i;
                }
            }
            if (k == 0) s1 = best; else s2 = best;
        }
        if (s1 >= 0 && s2 >= 0) {
            double cx = (fx[s1] + fx[s2]) * 0.5;
            double cy = (fy[s1] + fy[s2]) * 0.5;
            double distance = hypot((double)fx[s2] - fx[s1], (double)fy[s2] - fy[s1]);
            uint64_t received_ns = mach_ns();
            uint16_t scan = r[26] | (r[27] << 8);
            if (!two_finger_same_contacts(&two_finger, contact_id[s1], contact_id[s2])) {
                // 换指/固件换 ContactID 时重锚, 不能把新接触位置当成巨大缩放。
                pinch_finish(1);
                if (scroll_active) {
                    scrl_ema_x = scrl_ema_y = 0;
                    end_scroll_gesture();
                }
                two_finger_begin(&two_finger, contact_id[s1], contact_id[s2],
                                 cx, cy, distance, received_ns);
                scroll_active = 1;
                gesture_send_maybegin();
                touch_sample_ns = received_ns;
                touch_scan_previous = scan;
                // 直通兼容模式保留原来的滚动起步时序。
                if (!pinch_on || ntips != 2) {
                    two_finger.mode = TWO_SCROLL;
                    touch_scroll_begin(received_ns, scan);
                }
            } else {
                int was_pending = two_finger.mode == TWO_PENDING;
                int mode = two_finger_classify(&two_finger, cx, cy, distance, pinch_on && ntips == 2);
                uint64_t sample_ns = touch_scroll_time(received_ns, scan);
                if (mode == TWO_PINCH) {
                    if (!pinch_active) {
                        // 未确定时只发过滚动 MayBegin: Cancelled 收回, 不生成滚动或惯性。
                        scrl_ema_x = scrl_ema_y = 0;
                        end_scroll_gesture();
                        scroll_active = 0;
                        pinch_begin(cur, two_finger.distance, two_finger.began_ns,
                                    sample_ns, scan);
                    }
                    pinch_move(distance, sample_ns);
                    tap_moved = 1;
                } else {
                    double dx = (cx - last_cx) * scroll_gain;
                    double dy = (cy - last_cy) * scroll_gain;
                    scroll_velocity_update(natural ? dx : -dx, natural ? dy : -dy,
                                           (double)sample_ns / 1e6);
                    if (mode == TWO_SCROLL) {
                        if (was_pending) {
                            touch_scroll_begin(sample_ns, scan);
                            if (touch_resampling)
                                scroll_resampler_begin(&touch_resampler, two_finger.began_ns);
                            dx = (cx - two_finger.cx) * scroll_gain;
                            dy = (cy - two_finger.cy) * scroll_gain;
                        }
                        touch_scroll_push(dx, dy, sample_ns);
                    }
                }
            }
            last_cx = cx; last_cy = cy;
            have_last = 0;
        }
    } else if (!swallow) {
        // ---- 指针移动: 差分 × 速度增益, 保留小数余量 ----
        // 未按住 = 单指移动; 物理按住期间 = 拖移(滚动/三指分支已让位)。
        // 指针始终跟随最新落下的手指 — 原生触控板的"换指拖移": 按住后
        // 另一指接管指针(选中长文本就靠它), 换跟指当帧只锚定不跳动。
        if (scroll_active) end_scroll_gesture();   // 双指变单指: 手势收尾
        scroll_active = 0;
        two_finger.mode = TWO_NONE;
        int s = -1, sseq = 0;
        for (int i = 0; i < NFINGER; i++)
            if (tips[i] && down_seq[i] > sseq) { sseq = down_seq[i]; s = i; }
        if (s >= 0) {
            int suppress = 0;
            uint64_t pointer_now = mach_ns();
            uint16_t pointer_scan = r[26] | (r[27] << 8);
            if (!have_last || move_slot != s || pointer_contact_id != contact_id[s]) {
                // 新按下/换跟踪指: 仅锚定, 不跳动
                if (debug) fprintf(stderr, "[tpkbd] MV anchor slot=%d pad=(%d,%d) cur=(%.0f,%.0f)\n",
                                   s, fx[s], fy[s], cur.x, cur.y);
                mv_slop_done = 0;
                mv_frames = 0;
                snap_done = 0;
                pointer_motion_anchor(&pointer_motion, fx[s], fy[s], pointer_scan, pointer_now);
            } else {
                // 速度取相邻原始报文, 不能用死区累计距离除以一帧时间。
                pointer_motion_observe(&pointer_motion, fx[s], fy[s], pointer_scan, pointer_now);
                int disp = abs(fx[s] - last_px) + abs(fy[s] - last_py);
                mv_frames++;
                if (!btn_prev && !click && !mv_slop_done && !tap_pointer_moved &&
                    move_slop > 0 && !snap_done && mv_frames <= 10 &&
                    outside_move_slop(fx[s] - last_px, fy[s] - last_py)) {
                    // 落板跳变伪影: 落板后接触面稳定, 质心会单帧大跳(实测
                    // 10~18 counts, 方向朝掌侧=屏上向下)。它同时越过落指死区
                    // (外泄成漂移)和轻点位移阈值(废掉轻点)。窗口内首次大跳
                    // 判为伪影: 吞帧重锚, 移动/轻点锚点同步跟过去; 只吞一次,
                    // 真正的快速甩动(连续大帧)不受影响。
                    snap_done = 1;
                    tap_anchor_x = fx[s]; tap_anchor_y = fy[s];
                    tap_moved = 0;
                    last_px = fx[s]; last_py = fy[s];
                    pointer_motion_anchor(&pointer_motion, fx[s], fy[s], pointer_scan, pointer_now);
                    suppress = 1;
                    if (debug) fprintf(stderr, "[tpkbd] SNAP %d counts @frame %d, re-anchored\n",
                                       disp, mv_frames);
                } else if (btn_prev && drag_armed) {
                    // 按下拖移抑制(drag slop, 同原生触控板): 板面下压沉陷的
                    // 微移不是拖移 — 截图/选窗类工具靠"点击瞬间零位移"工作,
                    // 微移会被当成拉小框。越过阈值后恢复普通拖移。
                    if (abs(fx[s] - drag_ax) + abs(fy[s] - drag_ay) < drag_slop) {
                        suppress = 1;
                        last_px = fx[s]; last_py = fy[s];   // 静默重锚
                        pointer_motion_anchor(&pointer_motion, fx[s], fy[s], pointer_scan, pointer_now);
                        if (debug) fprintf(stderr, "[tpkbd] DRAG slop d=(%d,%d) swallowed\n",
                                           fx[s] - drag_ax, fy[s] - drag_ay);
                    } else {
                        drag_armed = 0;
                    }
                } else if (!btn_prev && move_slop > 0 && !mv_slop_done) {
                    // 落指死区(move slop): 手指落板瞬间接触质心自然下沉数
                    // counts, 不滤掉的话"轻点点按"会先漂一下再点击(落点偏下)。
                    // 死区内锚点保持不动, 越阈那帧的差分自带累计位移一次补发,
                    // 正常滑行无感(与 drag slop 的静默重锚语义不同)。
                    // 与上方轻点识别使用同一个闭区间: 两轴都 <= move_slop。
                    // 旧版 Manhattan 距离且用 <, 使 (7,7) 或 (0,14) 既移动又轻点。
                    if (!outside_move_slop(fx[s] - last_px, fy[s] - last_py)) {
                        suppress = 1;
                    } else {
                        mv_slop_done = 1;
                    }
                }
                if (!suppress) {
                    int pdx, pdy;
                    pointer_motion_apply(&pointer_motion, fx[s] - last_px, fy[s] - last_py,
                                         move_gain, move_acceleration, &pdx, &pdy);
                    int nx = last_sx + pdx;
                    int ny = last_sy + pdy;
                    int intended_x = nx, intended_y = ny;
                    if (debug) fprintf(stderr, "[tpkbd] MV slot=%d pad=(%d,%d) last=(%d,%d) d=(%d,%d) -> (%d,%d)\n",
                                       s, fx[s], fy[s], last_px, last_py,
                                       fx[s]-last_px, fy[s]-last_py, nx, ny);
                    clamp_to_screen(&nx, &ny);
                    avoid_gap(&nx, &ny, last_sx, last_sy);
                    // 屏幕边缘被挡下的分数不能累积成返程时的额外位移。
                    if (nx != intended_x) pointer_motion.remainder_x = 0;
                    if (ny != intended_y) pointer_motion.remainder_y = 0;
                    if (nx != (int)cur.x || ny != (int)cur.y) {
                        tap_pointer_moved = 1;
                        tap_moved = 1;
                    }
                    // 按住期间必须发 Dragged 而非 Moved: 按下时板面下沉, 触点
                    // 天然有微小位移, 若在 Down/Up 之间插入 Moved, App 的点击
                    // 跟踪会取消该次点击(表现为"点按偶尔不灵")。
                    if (nx != last_sx || ny != last_sy) {
                        if (btn_prev) {
                            if (btn_type) post_right_drag(CGPointMake(nx, ny));
                            else          post_mouse_drag(CGPointMake(nx, ny));
                        } else {
                            post_mouse_move(CGPointMake(nx, ny));
                        }
                    }
                    cur.x = nx; cur.y = ny;
                }
            }
            move_slot = s;
            pointer_contact_id = contact_id[s];
            if (!suppress) { last_px = fx[s]; last_py = fy[s]; }  // 死区抑制期间保持锚点
            last_sx = (int)cur.x; last_sy = (int)cur.y;
            have_last = 1;
        }
    }

    // ---- 物理按压: 单指=左键, 双指及以上=右键 ----
    // Desktop gestures cannot start a button press, but an existing physical
    // button must still reach the release path even with extra fingers down.
    if (ntips >= swipe_fingers && swipe3_on && !btn_prev) return;
    // 按下立即响应; 松开经 5 帧(≈50ms)消抖, 吞掉机械回弹造成的 1-0-1 抖动
    // (实测出现过 40ms 回弹间隙, 会被系统误判为双击)。
    // 连击计数: 500ms 内且位移 <12px 的再次按下 → count 递增(双击=2, 三击=3),
    // Down/Up 都带同一 count, 双击选词/三击选段才能正确工作。
    // 键型由按下瞬间的指数决定并锁定到松开, 中途加减指不换键。
    if (click) {
        release_streak = 0;
        if (!btn_prev && !swallow) {   // 锁定期吞新按压; 松开路径照常 → 键不卡死
            btn_type = (ntips >= 2) ? 1 : 0;
            if (btn_type) {
                if (debug) fprintf(stderr, "[tpkbd] BTN right down at (%.0f,%.0f)\n",
                                   cur.x, cur.y);
                post_right_click(cur, 1, 1);
            } else {
                click_count = click_count_for(cur);
                if (debug) fprintf(stderr, "[tpkbd] BTN left down count=%d at (%.0f,%.0f)\n",
                                   click_count, cur.x, cur.y);
                post_click(cur, 1, click_count);
            }
            // 拖移抑制锚点: 记按下瞬间跟踪指位置(沉陷微移在其邻域内被吞)
            {
                int fs = -1, fseq = 0;
                for (int i = 0; i < NFINGER; i++)
                    if (tips[i] && down_seq[i] > fseq) { fseq = down_seq[i]; fs = i; }
                if (fs >= 0) {
                    drag_armed = 1; drag_ax = fx[fs]; drag_ay = fy[fs];
                    pointer_motion_anchor(&pointer_motion, fx[fs], fy[fs],
                                          r[26] | (r[27] << 8), mach_ns());
                }
            }
            btn_prev = 1;
        }
    } else if (btn_prev) {
        if (++release_streak >= 5) {
            if (btn_type) {
                if (debug) fprintf(stderr, "[tpkbd] BTN right up\n");
                post_right_click(cur, 0, 1);
            } else {
                if (debug) fprintf(stderr, "[tpkbd] BTN left up count=%d\n", click_count);
                post_click(cur, 0, click_count);
            }
            btn_prev = 0;
            release_streak = 0;
            drag_armed = 0;   // 本轮按压结束, 抑制期随之结束
            if (drag3_on && ntips >= 3) three_drag.phase = DRAG3_TAIL;
        }
    }

    // 打字锁定的"免锁"判定依据: 只有真正在跟踪的接触才算(被吞掉的手掌不算)
    touch_now = swallow ? 0 : (ntips > 0);
}

// ---- HID 回调 ----
static void input_report_cb(void *context, IOReturn result, void *sender,
                            IOHIDReportType type, uint32_t reportID,
                            uint8_t *report, CFIndex reportLength) {
    (void)context; (void)result; (void)sender; (void)type;
    handle_report(reportID, report, reportLength);
}

// 下发 Input Mode 切换: SET_FEATURE(Feature, reportID=4, [val]) 让设备从
// 相对鼠标模式切到多点触控 digitizer 模式, 之后才会发 Report ID 0x03。
//
// 实证(2026-10-09): IOKit 对本设备的 feature 报文缓冲布局为
//   buffer[0]=reportID, buffer[1..]=payload, 长度=payload+1。
//   仅传 payload(不带 reportID 前缀)时 SET 返回成功但模式值不变(无效);
//   带 reportID 前缀时 GET 回读确认模式值被正确改写(0->3)。
static int get_input_mode(IOHIDDeviceRef device, const char *tag) {
    uint8_t buf[4] = {0};
    CFIndex len = sizeof(buf);
    IOReturn gr = IOHIDDeviceGetReport(device, kIOHIDReportTypeFeature,
                                       INPUT_MODE_RID, buf, &len);
    fprintf(stderr, "[tpkbd] GET InputMode[%s] ret=0x%x len=%d buf=%02x %02x\n",
            tag, gr, (int)len, buf[0], buf[1]);
    return buf[1];   // buf[0]=reportID, buf[1]=实际模式值
}

static void send_input_mode(IOHIDDeviceRef device) {
    if (!set_input_mode) return;
    uint8_t val = (uint8_t)input_mode_val;

    int before = get_input_mode(device, "before");
    if (before == val) {
        fprintf(stderr, "[tpkbd] Input Mode 已是 %d, 无需切换\n", val);
        return;
    }
    // 布局 B: buffer[0]=reportID, buffer[1]=value
    uint8_t buf[2] = { (uint8_t)INPUT_MODE_RID, val };
    IOReturn sr = IOHIDDeviceSetReport(device, kIOHIDReportTypeFeature,
                                       INPUT_MODE_RID, buf, 2);
    usleep(60000);
    int after = get_input_mode(device, "after");
    fprintf(stderr, "[tpkbd] SET InputMode=%d ret=0x%x, 回读 %d->%d %s\n",
            val, sr, before, after,
            (sr == kIOReturnSuccess && after == val) ? "✅切换成功" : "❌切换失败");
}

// ---- 键位重排: ThinkPad 键盘 Win(LeftGUI)↔Alt(LeftAlt) 互换, 对齐 Mac 排列 ----
// 思路: PC 键盘物理排列 Fn/Ctrl/Win/Alt, Mac 是 Fn/Ctrl/Option/Command —
// 第3键(Win)→Option、第4键(Alt,紧邻空格)→Command, 位置一一对齐。
// hidutil UserKeyMapping 按 VID/PID 匹配, 只挂本键盘的 HID 服务, 不影响
// MacBook 内置键盘。映射易失(重启/重插即清), 由 device_attached 每次重刷。
static int swap_winalt = 1;   // TPKB_SWAP_WINALT, 0=关闭

static void apply_key_mapping(void) {
    if (!swap_winalt) return;
    extern char **environ;
    pid_t pid;
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    char *argv[] = {
        (char *)"/usr/bin/hidutil", (char *)"property",
        (char *)"--matching", (char *)"{\"VendorID\":6127,\"ProductID\":24757}",
        (char *)"--set",
        (char *)"{\"UserKeyMapping\":["
                 "{\"HIDKeyboardModifierMappingSrc\":0x7000000e3,\"HIDKeyboardModifierMappingDst\":0x7000000e2},"
                 "{\"HIDKeyboardModifierMappingSrc\":0x7000000e2,\"HIDKeyboardModifierMappingDst\":0x7000000e3}]}",
        NULL
    };
    int r = posix_spawn(&pid, "/usr/bin/hidutil", NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (r == 0) {
        int st = 0;
        waitpid(pid, &st, 0);
        fprintf(stderr, "[tpkbd] 键位重排 Win↔Alt: hidutil %s\n",
                (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? "已应用" : "返回非零(查 --matching 参数)");
    } else {
        fprintf(stderr, "[tpkbd] 键位重排: hidutil 启动失败 (errno=%d)\n", r);
    }
}

static void device_attached(void *context, IOReturn result,
                            void *sender, IOHIDDeviceRef device) {
    (void)context; (void)result; (void)sender;
    CFNumberRef v = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey));
    CFNumberRef p = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey));
    CFNumberRef u = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDPrimaryUsageKey));
    CFNumberRef up = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDPrimaryUsagePageKey));
    int vi = 0, pi = 0, ui = 0, upi = 0;
    if (v) CFNumberGetValue(v, kCFNumberIntType, &vi);
    if (p) CFNumberGetValue(p, kCFNumberIntType, &pi);
    if (u) CFNumberGetValue(u, kCFNumberIntType, &ui);
    if (up) CFNumberGetValue(up, kCFNumberIntType, &upi);
    if (vi != VID || pi != PID) return;
    // 属性可能缺失(EventService 对象)，缺失时仍尝试，靠回调数据验证
    if (upi != 0 && !(upi == 1 && ui == 2)) return;   // 只要鼠标接口

    // macOS 13+ 对同一 USB 接口会创建两个服务对象(IOHIDDevice 与
    // IOHIDEventService)，原始报文回调只会在其中一个上触发，
    // 所以两个都尝试打开+挂回调，谁送来数据算谁。
    IOReturn ret = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (ret != kIOReturnSuccess)
        ret = IOHIDDeviceOpen(device, kIOHIDOptionsTypeSeizeDevice);
    fprintf(stderr, "[tpkbd] 匹配鼠标接口对象 %p, open=0x%x\n", device, ret);
    if (ret != kIOReturnSuccess && ret != kIOReturnExclusiveAccess)
        return;

    IOHIDDeviceScheduleWithRunLoop(device, CFRunLoopGetMain(), kCFRunLoopDefaultMode);

    uint8_t *buf = malloc(REPORT_BUF_SIZE);
    IOHIDDeviceRegisterInputReportCallback(device, buf, REPORT_BUF_SIZE,
                                           input_report_cb, NULL);
    fprintf(stderr, "[tpkbd] 已挂回调: %04x:%04x (%p)\n", vi, pi, device);

    // 挂好回调后再切 Input Mode, 避免漏掉切换后第一批 Report 0x03
    send_input_mode(device);

    // 键位重排随键盘重插重刷(hidutil 映射挂在设备服务上, 重插即失)
    apply_key_mapping();
}

static void device_detached(void *context, IOReturn result,
                            void *sender, IOHIDDeviceRef device) {
    (void)context; (void)result; (void)sender; (void)device;
    release_pointer_buttons();
    swipe3_end(1);
    system_swipe_tail = 0;
    pinch_finish(1);
    if (scroll_active) {
        scrl_ema_x = scrl_ema_y = 0;
        end_scroll_gesture();
        scroll_active = 0;
    }
    two_finger.mode = TWO_NONE; pinch_tail = 0; have_last = 0;
    fprintf(stderr, "[tpkbd] 设备断开\n");
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--probe") == 0) probe_mode = 1;
    if (argc > 1 && strcmp(argv[1], "--capture") == 0) return run_capture();

    const char *env;
    if ((env = getenv("TPKB_MOVE_GAIN"))    != NULL) {
        double value = strtod(env, NULL);
        if (isfinite(value) && value > 0 && value <= 20) move_gain = value;
    }
    if ((env = getenv("TPKB_MOVE_ACCEL"))   != NULL) move_acceleration = atoi(env) != 0;
    if ((env = getenv("TPKB_SCROLL_GAIN"))  != NULL) scroll_gain = atof(env);
    if ((env = getenv("TPKB_SCROLL_NATURAL")) != NULL) natural    = atoi(env);
    if ((env = getenv("TPKB_MOMENTUM"))      != NULL) momentum_on = atoi(env);
    if ((env = getenv("TPKB_DRAG_A"))        != NULL) mom_drag_a  = atof(env);
    if ((env = getenv("TPKB_PINCH")) != NULL) pinch_on = atoi(env) != 0;
    if ((env = getenv("TPKB_PINCH_GAIN")) != NULL) {
        double value = strtod(env, NULL);
        if (isfinite(value) && value >= 0.1 && value <= 4) pinch_gain = value;
    }
    if ((env = getenv("TPKB_SWIPE3"))        != NULL) swipe3_on   = atoi(env);
    if ((env = getenv("TPKB_DRAG3"))         != NULL) drag3_on    = atoi(env) != 0;
    if ((env = getenv("TPKB_SWIPE3_GAIN"))   != NULL) swipe3_gain = atof(env);
    if ((env = getenv("TPKB_TAPCLICK"))      != NULL) tap_on      = atoi(env);
    if ((env = getenv("TPKB_TAP_RIGHT"))     != NULL) tap_right_on = atoi(env);
    if ((env = getenv("TPKB_DRAG_SLOP"))     != NULL) drag_slop   = atoi(env);
    if ((env = getenv("TPKB_MOVE_SLOP"))     != NULL) move_slop   = atoi(env);
    if ((env = getenv("TPKB_TP_SCROLL"))     != NULL) tp_scroll_on = atoi(env);
    if ((env = getenv("TPKB_TP_GAIN"))       != NULL) tp_gain      = atof(env);
    if ((env = getenv("TPKB_TP_CLICK"))      != NULL) {
        if      (!strcmp(env, "terminal")) tp_click_action = 1;
        else if (!strcmp(env, "native"))   tp_click_action = 2;
        else                               tp_click_action = 0;
    }
    if ((env = getenv("TPKB_TAP_MS"))        != NULL) tap_ms_thresh = atoi(env);
    if ((env = getenv("TPKB_TYPELOCK_MS"))   != NULL) typelock_ms = atoi(env);
    if ((env = getenv("TPKB_SWAP_WINALT"))   != NULL) swap_winalt  = atoi(env);
    if ((env = getenv("TPKB_DRAG_B"))        != NULL) mom_drag_b  = atof(env);
    if ((env = getenv("TPKB_REL_GAIN"))     != NULL) rel_gain    = atof(env);
    if ((env = getenv("TPKB_REL_Y_INVERT")) != NULL) rel_y_invert = atoi(env);
    if ((env = getenv("TPKB_DEBUG"))        != NULL) debug        = atoi(env);
    if ((env = getenv("TPKB_REL_RID"))      != NULL) rel_rid      = (int)strtol(env, NULL, 0);
    if ((env = getenv("TPKB_DISABLE_REL"))  != NULL) disable_rel  = atoi(env);
    if ((env = getenv("TPKB_SET_INPUT_MODE")) != NULL) set_input_mode = atoi(env);
    if ((env = getenv("TPKB_INPUT_MODE"))   != NULL) input_mode_val = (int)strtol(env, NULL, 0);
    if ((env = getenv("TPKB_TAP")) != NULL) {
        if (strcmp(env, "hid") == 0)     g_tap = kCGHIDEventTap;
        else if (strcmp(env, "annotated") == 0) g_tap = kCGAnnotatedSessionEventTap;
        else                             g_tap = kCGSessionEventTap;
        fprintf(stderr, "[tpkbd] 事件注入点 TPKB_TAP=%s\n", env);
    }

    signal(SIGUSR1, scroll_trace_signal);
    signal(SIGUSR2, scroll_trace_signal);
    signal(SIGTERM, driver_stop_signal);
    signal(SIGINT, driver_stop_signal);
    CFRunLoopTimerRef trace_timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent(),
                                                        0.25, 0, 0, scroll_trace_control, NULL);
    if (trace_timer) {
        CFRunLoopAddTimer(CFRunLoopGetMain(), trace_timer, kCFRunLoopDefaultMode);
        CFRelease(trace_timer);
    }
    if ((env = getenv("TPKB_SCROLL_SMOOTH_MS")) != NULL) {
        double value = strtod(env, NULL);
        if (isfinite(value) && value >= 0 && value <= 40) scroll_smooth_ms = value;
    }
    fprintf(stderr, "[tpkbd] 指针: 速度加速=%s, 增益=%.2f (慢速%.2f / 快速%.2f), 小数位移累积\n",
            move_acceleration ? "开" : "关", move_gain,
            pointer_motion_gain(0, move_gain, move_acceleration),
            pointer_motion_gain(1500, move_gain, move_acceleration));
    fprintf(stderr, "[tpkbd] 双指捏合=%s gain=%.2f, 与滚动分流, 按压优先拖移\n",
            pinch_on ? "开" : "关", pinch_gain);
    fprintf(stderr, "[tpkbd] 三指拖移=%s, 桌面手势=%s (%d指)\n",
            drag3_on ? "开" : "关", swipe3_on ? "开" : "关", drag3_on ? 4 : 3);
    scroll_frames_init();
    hid_api_init();
    update_display_bounds();
    CGDisplayRegisterReconfigurationCallback(display_reconfig_cb, NULL);
    fprintf(stderr, "[tpkbd] 启动 (probe=%d, 虚拟桌面 %.0fx%.0f+%d屏, gain=%.2f/%.2f)\n",
            probe_mode, screen_bounds.size.width, screen_bounds.size.height,
            (int)g_display_count, move_gain, scroll_gain);

    // 检查/请求「辅助功能」授权(CGEventPost 移动光标必需)。
    // 该权限失效时 CGEventPost 静默失败(不报错、光标不动), 与输入监控不同,
    // 故这里主动查询并弹出系统授权对话框。
    {
        const void *keys[] = { kAXTrustedCheckOptionPrompt };
        const void *vals[] = { kCFBooleanTrue };
        CFDictionaryRef opts = CFDictionaryCreate(
            kCFAllocatorDefault, keys, vals, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        Boolean trusted = AXIsProcessTrustedWithOptions(opts);
        CFRelease(opts);
        fprintf(stderr, "[tpkbd] AXIsProcessTrusted(辅助功能)=%d %s\n",
                (int)trusted, trusted ? "已授权" : "未授权(光标不会移动!)");
    }

    // 启动自检: 把光标跳到固定坐标, 直观验证事件注入是否生效
    if (getenv("TPKB_SELFTEST") && atoi(getenv("TPKB_SELFTEST"))) {
        CGPoint before = cursor_pos();
        CGPoint target = CGPointMake(400, 400);
        post_mouse_move(target);
        usleep(120000);
        CGPoint after = cursor_pos();
        fprintf(stderr, "[tpkbd] 自检: 光标 (%.0f,%.0f) -> 目标(400,400) -> 实测(%.0f,%.0f) %s\n",
                before.x, before.y, after.x, after.y,
                (fabs(after.x-400)<3 && fabs(after.y-400)<3) ? "✅注入生效" : "❌注入无效");
    }

    // 只按 VID/PID 匹配，Usage 过滤放在 device_attached 里做
    // (EventService 对象可能不带 PrimaryUsage 属性，字典匹配会漏掉它)
    int ints[2] = { VID, PID };
    CFNumberRef nums[2] = { num_create(ints[0]), num_create(ints[1]) };
    const void *keys[] = { CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey) };
    CFDictionaryRef matching = CFDictionaryCreate(
        kCFAllocatorDefault, keys, (const void **)nums, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(manager, matching);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    IOHIDManagerRegisterDeviceMatchingCallback(manager, device_attached, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(manager, device_detached, NULL);

    // 打字锁定: 挂键盘监听 tap(只听不改), 凭已有「输入监控」授权收 keyDown
    key_tap_port = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                    kCGEventTapOptionListenOnly,
                                    CGEventMaskBit(kCGEventKeyDown),
                                    key_tap_cb, NULL);
    if (key_tap_port) {
        CFRunLoopSourceRef ksrc = CFMachPortCreateRunLoopSource(NULL, key_tap_port, 0);
        CFRunLoopAddSource(CFRunLoopGetMain(), ksrc, kCFRunLoopDefaultMode);
        CFRelease(ksrc);
        fprintf(stderr, "[tpkbd] 打字锁定: 键盘监听就绪 (敲键后 %dms 内吞新触摸, 0=关)\n",
                typelock_ms);
    } else {
        fprintf(stderr, "[tpkbd] 打字锁定: 键盘 tap 创建失败, 功能不生效\n");
    }

    // 小红点中键: 按住+拨动=滚动, 单击=Terminal (过滤型 tap, 需辅助功能授权)
    if (tp_scroll_on) {
        CGEventMask mmask = CGEventMaskBit(kCGEventOtherMouseDown) |
                            CGEventMaskBit(kCGEventOtherMouseUp)   |
                            CGEventMaskBit(kCGEventMouseMoved)     |
                            CGEventMaskBit(kCGEventLeftMouseDragged) |
                            CGEventMaskBit(kCGEventRightMouseDragged) |
                            CGEventMaskBit(kCGEventOtherMouseDragged);
        mouse_tap_port = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                          kCGEventTapOptionDefault,
                                          mmask, mouse_tap_cb, NULL);
        if (mouse_tap_port) {
            CFRunLoopSourceRef msrc = CFMachPortCreateRunLoopSource(NULL, mouse_tap_port, 0);
            CFRunLoopAddSource(CFRunLoopGetMain(), msrc, kCFRunLoopDefaultMode);
            CFRelease(msrc);
            CGEventTapEnable(mouse_tap_port, true);
            fprintf(stderr, "[tpkbd] 小红点中键: 滚动模式就绪 (按住+拨动=滚动, 单击=%s)\n",
                    tp_click_action == 1 ? "Terminal" :
                    tp_click_action == 2 ? "原生中键" : "无动作");
        } else {
            fprintf(stderr, "[tpkbd] 小红点中键: 鼠标 tap 创建失败, 功能不生效\n");
        }
    }

    CFRunLoopRun();
    release_pointer_buttons();
    pinch_finish(1);
    swipe3_end(1);
    swipe3_resend_cancel();
    return 0;
}
