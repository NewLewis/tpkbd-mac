# 逆向与开发纪要 (Devlog)

> 本文件是项目开发过程的历史快照与里程碑记录，保留原始的排查细节与教训。
> 面向用户的文档见 [README](../README.md)。

## 第一阶段：设备鉴定与描述符解码（2026-10-09）

### 要解决的问题

要给一块 **ThinkPad 拆机改装的外接键盘**（用户主力键盘，小红点重度使用者）救活触摸板：接 MacBook 后打字正常、**小红点原生可用**，但**触摸板完全无响应**。macOS 没有外接触摸板的通用驱动。

### 设备鉴定

| 项 | 值 |
|---|---|
| 设备名 | ThinkPad X1 Tablet Thin Keyboard Gen 3 |
| 代工 | Chicony |
| USB ID | VID `0x17ef`（联想）PID `0x60b5` |

设备枚举出 **3 个 USB HID 接口**：

1. **键盘接口**（UsagePage 1 / Usage 6）——普通打字键 + Fn 多媒体键，macOS 原生可用
2. **鼠标接口**（UsagePage 1 / Usage 2）——750 字节复合描述符，macOS 只认其中小红点部分
3. **厂商接口**（UsagePage 1 / Usage 0x80）——1 字节杂项报文（LED/热键），与触摸板无关

**鼠标接口描述符解码结论**（本项目第一个关键发现）：

- Report ID `0x02`：小红点，2 键 + 8 位相对 XY（macOS 已原生驱动）
- Report ID `0x10`：第二套相对鼠标——触摸板的"相对模式"
- Report ID `0x03`：**标准 HID 多点触控 digitizer**：5 指独立报文、16 位绝对坐标 X∈[0,1163]、Y∈[0,718]、接触计数、物理按压位——**触摸板本体**
- Report ID `0x04` 等 + 一组厂商 Output 报文：配置与模式切换通道
- 结论：**触摸板协议是公开的 HID 标准，不需要逆向私有协议**

### 诊断命令存档

```bash
# 枚举 HID 接口（system_profiler SPUSBDataType 对该设备无输出, 用 hidutil）
hidutil list | grep -i -E "0x17ef|thinkpad"

# 提取报告描述符（ioreg 必须用 IOHIDDevice 基类; 属性名是 "ProductID" 不是 "idProduct"）
ioreg -r -c IOHIDDevice -l -w 0 > /tmp/hid2.txt
# 取含 '"ProductID" = 24757' (0x60b5) 的块中的 "ReportDescriptor" 十六进制, 逐字节按 HID 规范解码
```

### 权限两连坑（解法沿用至今）

- `IOHIDDeviceOpen` 对鼠标类接口返回 `0xe00002e2`——必须授予 **输入监控**
- `CGEventPost` 缺 **辅助功能** 时**静默失败**（光标纹丝不动，极易误判成代码 bug）

## 第二阶段：Input Mode 切换突破（2026-10-10）

触摸板无响应的根因确认：出厂固件默认跑相对鼠标模式，多指 digitizer 模式要主机**主动下发标准 HID「Input Mode」Feature**（reportID 4，值 3）切换。两个细节坑：

- Feature buffer **首字节必须是 reportID**（`{0x04, 0x03}`），只传 payload 返回 kIOReturnSuccess 但模式不变
- 模式由设备记住，驱动重启无需重切

### digitizer Report 0x03 布局（1480 帧程序化全量解码钉死）

```
回调缓冲 len=30: r[0]=0x03(reportID 前缀) + 29 数据字节
finger k 数据起于 r[1+5k] (k=0..4), 每指 5 字节:
  [0] bit0=Confidence(1=真手指,0=手掌) bit1=TipSwitch bit2..4=ContactID
  [1..2] X 小端 (0..1163)   [3..4] Y 小端 (0..718)
尾部: r[26..27]=scan time  r[28]=接触计数  r[29] bit0=物理按压
```

**重大教训（烧掉两个会话）**：此前两轮会话的"无前缀"布局结论是错的——真根因是调试打印把 r[0] 打在行首、正文从 r[1] 起转储，肉眼解码整体偏移 1 字节。**规矩：二进制布局只信脚本对全量帧的自洽校验（tip 数==计数、坐标在量程、动作段分布吻合），肉眼读日志一律不算数。**

另一教训：多设备复合 HID 接口，绝不能靠一次混合手势抓包断定谁发哪个 rid——必须一次只动一个设备、清空日志、单独采集做对照（曾把 0x02/0x10 认反）。

## 第三阶段：手感对齐原生触控板（2026-10-12）

### 滚动"卡卡的"根因

纯像素滚动事件被 App 当**鼠标滚轮格子**，各 App 自己再做一层加速/平滑——快慢都顿挫，调增益治标不治本。原生丝滑靠**手势相位事件流**（type 22 连续滚动 + MayBegin/Began/Changed/Ended 相位 + 每事件配 type 29 手势 + 动量相位流），整包移植自 **Mac Mouse Fix** 的 `GestureScrollSimulator`。非显然语义：

- Ended 后部分 App（Xcode）按最后非零 Changed **自带续滚**——不接自家惯性时必须补零增量 momentumEnd 阻断
- 只碰未滚收 Cancelled 不收 Ended；动量期不发 type 29；行增量=像素/10

### 三指系统手势（私有 IOHIDEvent）

切空间/调度中心 = 构造 type 23 DockSwipe IOHIDEvent（motion 1横/2竖、flavor=DockPrimary、progress、velocity 子事件）塞进 type 30 CGEvent。`_SLEventSetIOHIDEvent` 未导出，用 MMF 生产做法内存 poke（`+0x18` 解引用 `+0xd0` 存指针）。`IOHIDEventCreate` 为 **5 参**签名（ctypes 读回验证）。缩放公式与"末帧同号→Ended/反号→Cancelled"规则均取自 MMF；结束事件需 200/500ms 重发规避系统手势卡死 bug。

### 多屏

光标钳制必须用**全部活动屏联合矩形** + `CGDisplayRegisterReconfigurationCallback` 热插拔刷新——只钳主屏会把光标困死在主屏（用户外接了右侧扩展屏）。

### 事件构造侧的零散铁律

- 按住期间移动必须发 `kCGEventLeftMouseDragged`，插 Moved 会被 App 取消该次点击（"点按偶尔不灵"根因）
- launchd agent 里 `CGEventPost` 到 `kCGHIDEventTap` 会被静默丢弃，用 `kCGSessionEventTap`
- TCC 授权绑定签名身份 → 自签长期证书 `tpkbd-sign`，重编译授权不失效
- 只重编译必须 `launchctl kickstart -k`，launchd 不自动换新二进制

## 第四阶段：补全功能（2026-10-12）

- **多显示⽰器**：双指右键（轻点+物理按压）与单指轻点左键（含双击/三击连击计数）
- **打字锁定**：libinput DWT 语义——敲键后 500ms 吞新触摸、在板手势放行（key 回调快照 touch_now）、物理键只挡新按下不挡松开（防卡键）
- **键位重排**：`hidutil UserKeyMapping` 按 VID/PID 匹配，ThinkPad 键盘 Win(LeftGUI)↔Alt(LeftAlt) 互换对齐 Mac 底排，MacBook 内置键盘不受影响；映射易失（重启清空）→ device_attached 里重刷

## 尚未实现（候选）

双指捏合缩放（motion=3 通道现成）、三指拖移窗口（与三指滑动手势冲突，需模式切换方案）、四指捏合启动台/显示桌面、三指轻点查词。

## 相关背景

本机共存设备：罗技 Lift 垂直鼠标（滚轮由 Mos 接管）与 MacBook 内置触摸板（轻点+三指拖移），三者与本驱动无冲突设计。
