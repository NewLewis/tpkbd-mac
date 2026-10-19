# tpkbd-mac

**A userspace macOS driver that turns the touchpad on a ThinkPad X1 Tablet Thin Keyboard into a fully functional, native-feeling Mac trackpad.** [中文说明](README.zh-CN.md)

[Skip to install](#install) · [Tuning](#tuning) · [How it works](#how-it-works) · [Troubleshooting](#troubleshooting)

## Why

The ThinkPad X1 Tablet Thin Keyboard Gen 3 (Chicony, USB VID `17ef` PID `60b5`) is a popular donor keyboard for DIY mechanical/compact keyboard builds. Plug it into a Mac and two of its three input devices work out of the box: the keyboard and the TrackPoint. The **touchpad is completely dead** — the firmware ships it in a PC-style relative-mouse mode that macOS simply has no driver for.

It turns out the touchpad also implements the **standard HID multitouch digitizer protocol**; it just needs to be switched into that mode. No kernel extension, no DriverKit, no reverse-engineered proprietary protocol required — everything below is a single-file C userspace daemon (~1300 lines) shipped as a LaunchAgent.

## Features

| Gesture | Action | Notes |
|---|---|---|
| 1-finger move | Pointer | Speed via `TPKB_MOVE_GAIN`; cursor can cross into external displays |
| 1-finger physical press | Left click | 5-frame debounce, proper double/triple-click counts |
| Button held + finger move | Drag / text selection | While pressed, all touch becomes drag — scrolling and gestures yield; pointer follows the newest finger (finger-walking); press-sink micro-movement is swallowed (drag slop) |
| 1-finger tap | Left click | <250 ms and <1.3 mm of travel (libinput's threshold); landing-snap artifacts are filtered; supports double/triple tap |
| 2-finger move | Smooth scrolling | Gesture-phase event stream — apps treat it like a real trackpad, not a mouse wheel |
| 2-finger flick | Momentum scrolling | Physics curve ported from Mac Mouse Fix's trackpad-like defaults |
| 2-finger press | Right click | 2-finger *tap*-right-click is off by default (easy to trigger accidentally) — enable with `TPKB_TAP_RIGHT=1` |
| 3-finger swipe left/right | Switch desktop space / full-screen app | Follows the finger, pull back to cancel |
| 3-finger swipe up / down | Mission Control / App Exposé | |
| Any typing | Touchpad locked for 500 ms | libinput-style disable-while-typing; touches already in progress are exempt; held buttons never stick |
| Win ↔ Alt keys | Remapped to Option / Command | Matches the Mac bottom-row layout, applied via `hidutil` **only to this keyboard** |
| Hold middle button + TrackPoint | Smooth scrolling | Classic ThinkPad scrolling with momentum; the cursor drifts slightly while active (known limitation, see Troubleshooting); a plain middle click does nothing by default — remap with `TPKB_TP_CLICK` |

The TrackPoint is deliberately left untouched — macOS already drives it natively.

## Requirements

- macOS 26 or newer. Tested on **macOS 27.0.1, Apple Silicon** only. (The 3-finger Mission Control / space-switching events use the macOS 27 IOHIDEvent path; older systems would need the pre-27 variant that Mac Mouse Fix implements.)
- Xcode Command Line Tools (`xcode-select --install`).
- The keyboard connected over USB.

## Install

```bash
git clone <this repo> && cd tpkbd-mac

# 1. Build (creates a stable self-signed code-signing identity first — see below)
./build.sh

# 2. Install the LaunchAgent (edit the binary path inside the plist!)
cp com.pjr.tpkbd-touchpad.plist ~/Library/LaunchAgents/
#    → open it and replace /path/to/tpkbd-mac/tpkbd_touchpad with your real path
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.pjr.tpkbd-touchpad.plist
```

**Creating the signing certificate (one-time, strongly recommended).** TCC privacy permissions (next step) are bound to the code-signing identity. If the binary is re-signed with a different identity after every rebuild — which is what ad-hoc signing does — the permissions silently stop applying and you get phantom failures. Create a long-lived self-signed cert instead:

1. Open **Keychain Access → Certificate Assistant → Create a Certificate**
2. Name: `tpkbd-sign`, Identity Type: Code Signing, check "Let me override defaults" and pick a long validity (e.g. 10 years)
3. `./build.sh` picks it up automatically

**Granting permissions (one-time).** After the agent first runs, allow the binary in System Settings → Privacy & Security:

- **Input Monitoring** → `tpkbd_touchpad`
- **Accessibility** → `tpkbd_touchpad`

Then restart the agent (`launchctl kickstart -k gui/$(id -u)/com.pjr.tpkbd-touchpad`) and replug the keyboard. Check `tail /tmp/tpkbd.log` — you should see `已挂回调` and `Input Mode` lines.

## Tuning

Everything is an environment variable in the plist — change and `launchctl bootout` + `bootstrap` again, no recompile needed.

| Variable | Default | Meaning |
|---|---|---|
| `TPKB_MOVE_GAIN` | 1.8 | Pointer speed multiplier |
| `TPKB_SCROLL_GAIN` | 2.2 | Scroll speed multiplier |
| `TPKB_SCROLL_NATURAL` | 1 | 1 = natural scrolling |
| `TPKB_TAPCLICK` | 1 | Tap-to-click |
| `TPKB_TAP_RIGHT` | 0 | 2-finger tap = right click (off by default; 2-finger press is always right click) |
| `TPKB_DRAG_SLOP` | 12 | Click-time drag slop (sensor counts): movement below this right after a press is swallowed so the press reads as a clean click — screenshot window-picking etc. depend on it; 0 disables |
| `TPKB_MOVE_SLOP` | 14 | Landing dead zone (sensor counts), ≈ libinput's 1.3 mm tap-move threshold: landing-settle shifts don't move the cursor and don't cancel taps; crossing it releases tracking with the full distance replayed |
| `TPKB_TAP_MS` | 250 | Max tap duration (ms) |
| `TPKB_SWIPE3` | 1 | 3-finger gestures |
| `TPKB_SWIPE3_GAIN` | 1.0 | 3-finger swipe speed |
| `TPKB_TYPELOCK_MS` | 500 | Typing lockout window, 0 = off |
| `TPKB_SWAP_WINALT` | 1 | Win↔Alt remap to Option/Command |
| `TPKB_TP_SCROLL` | 1 | TrackPoint middle-button scrolling on/off |
| `TPKB_TP_GAIN` | 2.5 | TrackPoint movement → scroll pixel gain |
| `TPKB_TP_CLICK` | *(none)* | Plain middle-click action: nothing (default), `terminal`, or `native` (real middle click) |
| `TPKB_MOMENTUM` / `TPKB_DRAG_A` / `TPKB_DRAG_B` | 1 / 30 / 0.7 | Momentum scrolling on/off and its drag-curve physics (a·v^b) |
| `TPKB_DEBUG` | 0 | Verbose logging to `/tmp/tpkbd.log` |

## How it works

1. **Mode switch.** On attach, the daemon sends the standard HID *Input Mode* feature report (report ID 4, value 3) to the mouse interface. The firmware then emits multitouch digitizer reports (ID `0x03`) instead of relative-mouse ones. The setting sticks across replugs. Gotcha: the feature buffer must be `[reportID, value]` — sending the payload alone returns success but changes nothing.
2. **Report parsing.** 30-byte reports: 5 finger slots × 5 bytes (confidence/tip/contact-id flags, 16-bit X∈[0,1163], Y∈[0,718]), scan time, contact count, and a physical button bit. Details and the decoding methodology are in the source comments and [docs/devlog.md](docs/devlog.md).
3. **Event synthesis.** The interesting part. Posting plain scroll events makes every app apply its own mouse-wheel acceleration — that's why third-party touchpad tools feel janky. Native trackpad feel comes from **gesture-phase event streams**: `MayBegin → Began → Changed → Ended` scroll events marked continuous, each followed by a type-29 gesture event (Safari swipe-back depends on it), then a momentum-phase stream after lift. The momentum physics (drag curve `v' = −a·v^b`, a=30, b=0.7), the phase semantics, and the entire approach are **ported from [Mac Mouse Fix](https://github.com/noah-nuebling/mac-mouse-fix)**.
4. **System gestures.** 3-finger swipes construct private `IOHIDEvent`s (type 23, DockSwipe) with motion/flavor/progress fields, a velocity child event, and a CGEvent type-30 carrier — same construction MMF uses on macOS 27. Space-switching scale adapts to the number of spaces via the private CGS API; end events get resent (200/500 ms) to dodge a system bug where the gesture can get stuck.
5. **Everything else:** cursor clamping uses the union rectangle of all displays (a single-display clamp traps the cursor); typing lock is a listen-only keyDown tap implementing libinput's disable-while-typing semantics; the Win↔Alt remap shells out to `hidutil` with a VID/PID match so only this keyboard is affected.

See [docs/devlog.md](docs/devlog.md) for the full reverse-engineering story, including the report-layout pitfall that cost two debugging sessions.

## Troubleshooting

- **Cursor doesn't move at all** → Accessibility permission missing. `CGEventPost` fails *silently* without it — this looks exactly like a code bug.
- **`IOHIDDeviceOpen` returns `0xe00002e2`** → Input Monitoring permission missing.
- **Rebuilt the binary, behavior unchanged** → launchd still runs the old image: `launchctl kickstart -k gui/$(id -u)/com.pjr.tpkbd-touchpad`.
- **Permissions lost after rebuild** → you're ad-hoc signing; create the `tpkbd-sign` cert (see Install).
- **Permissions lost after moving the binary** → TCC grants are bound to the binary's **absolute path** as well as its signing identity. Deploy the daemon to a stable path once (e.g. `TPKB_INSTALL_DIR="$HOME/Library/Application Support/tpkbd-mac" ./build.sh`) and point the LaunchAgent there, so reorganizing your source tree never breaks the grants again.
- **Cursor drifts while scrolling with the TrackPoint middle button** → known limitation: the WindowServer moves the cursor before session-level event taps see the event, so swallowed movement still shifts the cursor slightly. Swallowing still prevents apps from seeing the drag, so the scroll itself is clean; a full fix would require seizing the HID device (not implemented).
- Logs: `tail -f /tmp/tpkbd.log` (set `TPKB_DEBUG=1` for full report dumps).

## Limitations

- No pinch-to-zoom, no three-finger-drag, no force click (hardware has no pressure sensor). 
- Typing lock does not apply inside Secure Input sessions (password fields) — the system hides key events from event taps there.
- Tested on exactly one keyboard firmware and one macOS version. Report IDs and the report layout are verified constants for this device; a different firmware may differ.

## Acknowledgements

- **[Mac Mouse Fix](https://github.com/noah-nuebling/mac-mouse-fix)** by Noah Nuebling — the single most important reference for "what a native Mac trackpad actually emits". The gesture-phase scroll design, momentum parameters, and DockSwipe construction in this project are derived from its source code (used under the [MMF License](https://github.com/noah-nuebling/mac-mouse-fix/blob/master/License)). If you like how this feels, buy MMF.
- **[libinput](https://gitlab.freedesktop.org/libinput/libinput)** — the tap-move threshold (1.3 mm) and the landing dead zone adopt its `DEFAULT_TAP_MOVE_THRESHOLD` after measuring this pad's landing-settle artifacts.
- The Linux `hid-lenovo` driver discussions, for confirming this keyboard family exposes a standard digitizer mode.

## License

[MIT](LICENSE), with the MMF-derived attribution notice included therein.
