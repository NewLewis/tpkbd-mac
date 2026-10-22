# tpkbd-mac

**让 ThinkPad X1 Tablet 薄键盘上的触摸板在 macOS 上满血复活的开源用户态驱动。** [English](README.md)

[直达安装](#安装) · [调参](#调参) · [工作原理](#工作原理) · [踩坑记录](#疑难排查)

## 为什么需要它

ThinkPad X1 Tablet Thin Keyboard Gen 3（Chicony 代工，USB VID `17ef` PID `60b5`）是客制化键盘圈常见的拆机 donor 键盘。接上 Mac 后，三个输入设备里有两个开箱即用：**键盘**和**小红点**。唯独**触摸板完全没反应**——出厂固件把触摸板跑在 PC 式相对鼠标模式，macOS 没有为它编写的驱动。

后来发现这块触摸板其实完整实现了**标准 HID 多点触控 digitizer 协议**，只差一条指令把它切过去。于是不需要内核扩展、不需要 DriverKit、不需要逆向私有协议——全部方案就是一个单文件 C 用户态守护进程（约 1300 行），以 LaunchAgent 常驻。

## 功能

| 手势 | 功能 | 说明 |
|---|---|---|
| 单指移动 | 光标 | 按手速平滑加速、保留小数位移；基础速度 `TPKB_MOVE_GAIN` 可调；可跨到外接扩展屏 |
| 单指按压 | 左键 | 松开 5 帧消抖；双击选词/三击选段正常 |
| 按住不放 + 移动（任意指数） | 拖移 / 选中文本 | 按住期间触摸全部变拖移，滚动/手势让位；指针跟随最新落下的手指，可换指接力；按压沉陷微移被吞（drag slop，点击干净） |
| 单指轻点 | 左键 | <250ms 且位移 <1.3mm（取 libinput 阈值）才算点；落板跳变伪影已滤除；支持双击/三击连击 |
| 双指移动 | 丝滑滚动 | 手势相位事件流——App 把它当真触控板而非鼠标滚轮 |
| 双指甩动 | 惯性滚动 | 移植 Mac Mouse Fix 的触控板物理曲线 |
| 双指捏合 / 张开 | 缩小 / 放大 | 原生缩放事件，需应用支持；同一次接触锁定滚动或缩放，抬指后可切换 |
| 双指轻点 / 按压 | 右键 | 轻点默认开启，分别跟踪两指；与滚动、捏合、拖移和物理按压互斥 |
| 三指移动 | 拖移 / 选中文字 | 无需按压；从当前光标位置开始，少于三指时松开；残留指不会误滚动或移动光标 |
| 四指左右滑 | 切换桌面空间 / 全屏应用 | 跟手、回拉取消、按空间数自适应缩放 |
| 四指上滑 / 下滑 | 调度中心 / App Exposé | 全部抬指后再开始拖移 |
| 打字中 | 触摸板锁定 500ms | libinput 式 disable-while-typing；在板手势放行、按住的键不卡死 |
| Win ↔ Alt 键 | 互换为 Option / Command | 对齐 Mac 底排排列，经 `hidutil` **只对本键盘**生效 |
| 按住中键 + 拨小红点 | 丝滑滚动 | 经典 ThinkPad 滚动（带惯性）；期间光标会小幅漂移（已知限制，见疑难排查）；中键单击默认无动作，`TPKB_TP_CLICK` 可配 |

小红点故意完全不碰——macOS 原生已驱动它。

## 环境要求

- macOS 26 及以上。仅在 **macOS 27.0.1 / Apple Silicon** 上实测过。（调度中心/切空间用的是 macOS 27 的 IOHIDEvent 通道；旧系统需要 Mac Mouse Fix 里的 pre-27 变体。）
- Xcode 命令行工具（`xcode-select --install`）。
- 键盘以 USB 连接。

## 安装

```bash
git clone <本仓库> && cd tpkbd-mac

# 1. 编译（先创建长期自签证书, 见下）
./build.sh

# 2. 安装 LaunchAgent（记得改 plist 里的二进制路径！）
cp com.pjr.tpkbd-touchpad.plist ~/Library/LaunchAgents/
#    → 打开它, 把 /path/to/tpkbd-mac/tpkbd_touchpad 换成你的真实绝对路径
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.pjr.tpkbd-touchpad.plist
```

**创建签名证书（一次性，强烈建议）。** TCC 隐私授权绑定在签名身份上。如果每次重编译都换身份（ad-hoc 签名就是这样），授权会静默失效，出现各种灵异故障。创建一个长期自签证书：

1. 打开 **钥匙串访问 → 证书助理 → 创建证书**
2. 名称 `tpkbd-sign`，身份类型选"代码签名"，勾选"覆盖默认值"，有效期拉长（如 10 年）
3. `./build.sh` 会自动找到它

**授权（一次性）。** 代理首次运行后，在 系统设置 → 隐私与安全性 里允许 `tpkbd_touchpad`：

- **输入监控** → 勾选
- **辅助功能** → 勾选

然后 `launchctl kickstart -k gui/$(id -u)/com.pjr.tpkbd-touchpad` 重启代理并重插键盘。`tail /tmp/tpkbd.log` 应能看到 `已挂回调` 与 `Input Mode` 字样。

## 调参

全部走 plist 环境变量——改完 `launchctl bootout` + `bootstrap` 即生效，无需重编译。

| 变量 | 默认 | 含义 |
|---|---|---|
| `TPKB_MOVE_GAIN` | 1.8 | 光标基础倍率；速度曲线从慢移 0.5× 平滑提高到快移 2×，默认实际约 0.9～3.6 |
| `TPKB_MOVE_ACCEL` | 1 | 按手速加速；0 使用固定倍率。两种模式都保留小数位移 |
| `TPKB_SCROLL_GAIN` | 2.2 | 滚动速度倍率 |
| `TPKB_SCROLL_SMOOTH_MS` | 12 | 按屏幕刷新重采样的缓冲时长(ms)，0 关闭；增加的输入延迟上限约为缓冲加一个刷新周期 |
| `TPKB_PINCH` | 1 | 双指捏合缩放开关，0 恢复仅双指滚动 |
| `TPKB_PINCH_GAIN` | 1.0 | 缩放灵敏度（0.1～4），与自然滚动方向和光标速度独立 |
| `TPKB_SCROLL_NATURAL` | 1 | 1=自然滚动 |
| `TPKB_TAPCLICK` | 1 | 轻点点按 |
| `TPKB_TAP_RIGHT` | 1 | 双指轻点=右键，设为 0 关闭；也受轻点总开关控制，双指物理按压独立保留 |
| `TPKB_DRAG_SLOP` | 12 | 按下瞬间的拖移抑制阈值（传感器 counts）：按压沉陷的微移被吞掉、点击保持干净（截图选窗等依赖）；0 关闭 |
| `TPKB_MOVE_SLOP` | 14 | 落指死区（传感器 counts）≈ libinput 的 1.3mm 轻点位移阈值：轻点与移动共用每轴闭区间，避免微移既移动又点击；越阈后一次性补发累计位移 |
| `TPKB_TAP_MS` | 250 | 轻点最长时长(ms) |
| `TPKB_DRAG3` | 1 | 三指拖移与选字；设为 0 恢复原来的三指桌面手势 |
| `TPKB_SWIPE3` | 1 | 桌面手势开关，沿用旧配置名；开启三指拖移后使用四指 |
| `TPKB_SWIPE3_GAIN` | 1.0 | 桌面手势滑动速度（三指/四指共用） |
| `TPKB_TYPELOCK_MS` | 500 | 打字锁定窗口, 0=关 |
| `TPKB_SWAP_WINALT` | 1 | Win↔Alt 互换 |
| `TPKB_TP_SCROLL` | 1 | 小红点中键滚动开关 |
| `TPKB_TP_GAIN` | 2.5 | 小红点位移→滚动像素增益 |
| `TPKB_TP_CLICK` | 无 | 中键单击行为：无动作（默认）/ `terminal` / `native`（原生中键） |
| `TPKB_MOMENTUM` / `TPKB_DRAG_A` / `TPKB_DRAG_B` | 1 / 30 / 0.7 | 惯性滚动开关与物理曲线 (a·v^b) |
| `TPKB_DEBUG` | 0 | 详细日志到 `/tmp/tpkbd.log` |

## 工作原理

1. **模式切换**：设备接入后，守护进程向鼠标接口发标准 HID Input Mode Feature 报文（reportID 4、值 3），固件即改发多点触控 digitizer 报文（`0x03`）代替相对鼠标报文。该设置跨重插保持。坑：Feature buffer 必须是 `[reportID, value]`，只发 payload 返回成功但什么都不改。
2. **报文解析**：30 字节 = 5 指位 × 5 字节（confidence/tip/contact-id 标志位 + 16 位 X∈[0,1163]、Y∈[0,718]）+ scan time + 接触计数 + 物理按压位。细节与解码方法论在源码注释和 [docs/devlog.md](docs/devlog.md)。
3. **事件合成**（最有意思的部分）：直接发像素滚动事件，各 App 会当鼠标滚轮自己再做一层加速——这就是第三方触摸板工具"卡卡的"根源。原生触控板手感来自**手势相位事件流**：MayBegin→Began→Changed→Ended 的连续滚动事件，每个配一个 type-29 手势事件（Safari 回弹依赖它），抬指后再接动量相位流。惯性物理（`v' = −a·v^b`，a=30、b=0.7）、相位语义、整套方案**移植自 [Mac Mouse Fix](https://github.com/noah-nuebling/mac-mouse-fix)**。
4. **拖移与系统级手势**：三指拖移发送左键按下、拖动、松开事件，复用指针速度曲线并保留三指质心的小数位移；轻触静止不会点击，接触换槽/换 ID 时重锚。四指滑动构造私有 `IOHIDEvent`（type 23 DockSwipe），带 motion/flavor/progress 字段、velocity 子事件、type-30 CGEvent 载体——与 MMF 在 macOS 27 的做法一致。切空间缩放经私有 CGS API 按空间数自适应；结束事件 200/500ms 后重发一次，规避系统偶发的手势卡死 bug。手势确定后锁定到抬指，避免三指拖移与四指滑动互相抢占。
5. **其余**：光标钳制用全部屏幕的联合矩形（只钳主屏会把光标困死在主屏）；打字锁定是 ListenOnly keyDown tap 实现的 libinput DWT 语义；Win↔Alt 互换经 `hidutil` 按 VID/PID 匹配、只影响这一把键盘。

完整逆向过程（包括烧掉两个调试会话的报文布局大坑）见 [docs/devlog.md](docs/devlog.md)。

## 疑难排查

- **光标完全不动** → 缺辅助功能权限。`CGEventPost` 缺它时**静默失败**，症状和代码 bug 一模一样。
- **`IOHIDDeviceOpen` 返回 `0xe00002e2`** → 缺输入监控权限。
- **重编译后行为没变** → launchd 还在跑旧二进制：`launchctl kickstart -k gui/$(id -u)/com.pjr.tpkbd-touchpad`。
- **重编译后授权失效** → 在用 ad-hoc 签名；按安装一节创建 `tpkbd-sign` 证书。
- **移动二进制后授权失效** → TCC 授权同时绑定签名身份和二进制的**绝对路径**。建议把守护进程一次性部署到稳定路径（如 `TPKB_INSTALL_DIR="$HOME/Library/Application Support/tpkbd-mac" ./build.sh`）、LaunchAgent 指向那里，之后随便整理源码目录都不会再断授权。
- **小红点中键滚动时光标小幅漂移** → 已知限制：WindowServer 在事件到达会话层 tap 之前就更新了光标，吞掉的事件仍会让光标微动。App 侧不会看到拖拽（滚动本身是干净的）；彻底修复需要抢占 HID 设备（未实现）。
- 日志：`tail -f /tmp/tpkbd.log`（`TPKB_DEBUG=1` 可看全量报文转储）。

## 已知限制

- 三指拖移在不足三指时松开鼠标，没有拖移锁定或全抬指续拖；下一轮手势需先全部抬指。
- 无压力感应（硬件无压感）。
- 打字锁定在 Secure Input 会话（密码框）内不生效——系统对 event tap 隐藏按键。
- 只在一把键盘固件 + 一个 macOS 版本上实测。报文 ID 与布局是该设备的实证常量，不同固件可能不同。

## 致谢

- **[Mac Mouse Fix](https://github.com/noah-nuebling/mac-mouse-fix)**（Noah Nuebling）——"原生 Mac 触控板到底发什么事件"的唯一真相源。本项目的手势相位滚动设计、惯性参数、DockSwipe 构造均衍生自其源码（依 [MMF License](https://github.com/noah-nuebling/mac-mouse-fix/blob/master/License) 使用）。觉得这套手感好，请去付费支持 MMF。
- **[libinput](https://gitlab.freedesktop.org/libinput/libinput)**——轻点位移阈值（1.3mm）与落指死区采用了它的 `DEFAULT_TAP_MOVE_THRESHOLD`（基于本板落板沉降伪影的实测标定）。
- Linux `hid-lenovo` 驱动的相关讨论，佐证了这个键盘家族有标准 digitizer 模式。

## 许可证

[MIT](LICENSE)，其中含 MMF 衍生部分的署名声明。

## 开发验证

运行 `./tests/run.sh` 可验证滚动精度、惯性速度和手势相位，包含两指先后抬起的回放测试。测试只在内存中检查事件，不发送鼠标或滚动输入；需要 Xcode 命令行工具。实际滚动手感仍需在目标设备上验证。
