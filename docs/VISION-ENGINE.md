# CINEMAX Vision Engine: Smart-TV-style picture processing for Windows

This document comes before the implementation. It records:
- what this laptop's display hardware can do (measured on 2026-11-20),
- which Windows mechanisms can change the picture system-wide,
- which controls are real hardware and which are processing,
- what cannot be done, and why.

## 1. Measured hardware (this laptop)

| Item | Value | How it was measured |
|---|---|---|
| GPU | Intel Iris Xe, driver 32.0.101.7088 | WMI `Win32_VideoController` |
| Displays | 1 internal panel (EDID maker `CMN`, product 1522), eDP, 34 × 19 cm | `QueryDisplayConfig`, WMI `WmiMonitorID` |
| Mode | 1920 × 1080 @ 120.02 Hz, 8 bits per colour | `QueryDisplayConfig`, DXGI `GetDesc1` |
| HDR | **Not supported**; Windows reports `highDynamicRangeSupported = 0` | `DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2` |
| Wide colour / ACM | Supported, currently off | same |
| Luminance | ~270 nits peak, 0.5 nits black (≈ 540:1) | DXGI `MaxLuminance` / `MinLuminance` |
| Gamut | R(0.590, 0.350) G(0.330, 0.555) B(0.153, 0.119): colour triangle ≈ 67 % of sRGB's area | DXGI primaries |
| Monitor controls (DDC/CI) | None (normal for built-in panels) | `GetMonitorCapabilities` |
| Backlight | 0–100 % via WMI `WmiMonitorBrightness` | WMI |
| Windows gamma ramp | Works within Windows' safety range: blue × 0.62 accepted, × 0.40 rejected | `SetDeviceGammaRamp` test, restored |
| Windows colour matrix | Works (saturation × 1.2 accepted) | `MagSetFullscreenColorEffect` test, restored |
| **Intel display pipe** (IGCL 1.1) | **3D LUT 17³**, 1D degamma LUT (2049), 3×3 CSC + offsets, 1D gamma LUT (2049 × 3), output 3×3 matrix | `ctlPixelTransformationGetConfig` |
| **Intel hardware sharpness** | Non-adaptive filter, strength 1–5 | `ctlGetSharpnessCaps`; set + restore test OK |
| Windows Night Light | Off | CloudStore registry state |

## 2. Windows mechanisms that can change the picture

| Mechanism | Applies to | Can do | Cannot do | Cost / caveats |
|---|---|---|---|---|
| **Intel IGCL display pipe** (3D LUT + sharpness) | The whole screen after Windows composites it: apps, games (including exclusive fullscreen), protected video, subtitles, cursor | Any per-pixel colour/tone transform (17³ 3D LUT); hardware sharpening | Anything on non-Intel GPUs; 3D LUT while Windows HDR is on | **Zero** GPU/CPU per frame, no added latency. The driver may reset on mode changes or sleep, so CINEMAX re-applies. |
| Windows gamma ramp (`SetDeviceGammaRamp`) | One display, at scan-out | Per-channel curves: brightness, contrast, gamma, black level, temperature, RGB, blue light | Saturation, vibrance, sharpness | Windows rejects ramps far from identity; shares hardware with Night Light and calibration; ignored in HDR |
| Magnification full-screen colour matrix | All displays at once (composited content) | Linear 5×5 matrix: saturation, colour balance | Vibrance (non-linear), gamma, sharpness | Not applied to exclusive-fullscreen games; only one app can use it (conflicts with Windows Colour filters) |
| Monitor hardware via DDC/CI | External monitors that support it | Brightness, contrast, RGB gain, sometimes sharpness | Internal panels | ~50 ms per command; varies by monitor |
| Backlight via WMI | Built-in panel | Light output only | Colour / contrast | Same control as Windows' brightness slider |
| Windows HDR / Advanced Color (`DisplayConfig`) | HDR-capable displays | Status; turning HDR on or off **only when the user clicks** | Real HDR on an SDR panel | CINEMAX never forces HDR |
| Desktop Duplication + Direct3D shader + overlay window | (rejected) | Anything | — | Protected video turns black, adds ≥ 1 frame of latency, costs GPU, and an overlay can interfere with input and capture. **Not used.** |

### Decision

- **On Intel GPUs (this laptop): the hardware display pipe.**
  - Every colour and tone control is folded into one 17 × 17 × 17 3D LUT, loaded into the display engine.
  - Sharpness uses Intel's hardware filter.
  - Backlight uses the panel's real backlight.
  - Nothing is drawn over the screen, and nothing is captured.
- **Other GPUs (fallback): Windows gamma ramp plus the Windows colour matrix.**
  - Covers every control except vibrance (approximated by saturation) and sharpness.
  - The UI labels this mode.

## 3. Controls and what implements them on this laptop

| Control | Implementation | Hardware or processing |
|---|---|---|
| Backlight | WMI panel backlight | **Hardware** |
| Brightness (picture) | Mid-tone lift/lower in perceptual lightness (OKLab L); black and white points kept | Processing → 3D LUT |
| Contrast | S-curve around mid-grey; shadow and highlight ends tapered so nothing clips or crushes | Processing → 3D LUT |
| Black level | Near-black lift/lower that fades out by mid-tones | Processing → 3D LUT |
| Gamma | Target display gamma 1.8–2.6 (2.2 = unchanged) | Processing → 3D LUT |
| Saturation | OKLab chroma scaling | Processing → 3D LUT |
| Vibrance | Boosts muted colours more than saturated ones; skin hues protected | Processing → 3D LUT |
| Colour temperature | Planckian white point (K) relative to D65, in linear light, normalised so white never clips | Processing → 3D LUT |
| RGB colour balance / white balance | 2-point: R/G/B gain (highlights) and offset (shadows) | Processing → 3D LUT |
| Blue light reduction | Extra warm shift in linear light, luminance-compensated | Processing → 3D LUT |
| Sharpness | Intel display-engine sharpening filter, Off + 5 levels | **Hardware** |
| HDR status | Windows Advanced Color info per display | Read-only, plus an explicit user toggle where supported |

Out-of-gamut results are brought back by reducing chroma at constant lightness and hue, never by clipping channels. That avoids hue shifts and blotchy saturated areas.

## 4. Limits

1. **The panel's physical limits don't change.**
   - About two-thirds of the sRGB colour area, about 540:1 contrast, 270 nits, SDR only.
   - VIVID and HDR EXPERIENCE change how the picture *looks*, not what the panel can show.
2. **No real HDR on this laptop.** On this panel HDR EXPERIENCE is an *HDR-style tone curve* (shadow detail lift, highlight roll-off, mid-tone contrast), and it's labelled that way. On an HDR monitor, CINEMAX shows HDR status, can switch Windows HDR on when you ask, and leaves HDR content to Windows' own colour management. The 3D LUT is not available in HDR mode.
3. **Sharpness has 5 hardware steps**, not a continuous slider.
4. **Screenshots and screen recordings don't show the effect.** The display pipe works after Windows composes the screen, which is also why it can't be blocked or bypassed by apps.
5. **Before/after on the real screen** works by bypassing the hardware processing while you hold *Compare*. A side-by-side split of the live desktop would need screen capture plus an overlay (rejected above).
6. **Windows Night Light** uses a different hardware stage. Both can be active, and their effects add up. CINEMAX detects Night Light and says so in the panel.
7. **The driver can reset the pipe** after display mode changes, sleep or driver updates. CINEMAX re-applies on display/power changes and checks every few seconds.
8. **Crash safety.** The hardware stays processed if a process dies. `CinemaxGuard` therefore restores the Intel pipe defaults, the original sharpness and the original gamma ramp when CINEMAX exits unexpectedly. The *Restore original display* button does the same at any time.

## 5. Auto picture mode (no screen capture)

Uses only process and window information:

- **Foreground app**
  - `GetForegroundWindow` → process image name and window title.
  - UWP apps: the real app is found behind `ApplicationFrameHost`.
- **Games**
  - Windows' own `SHQueryUserNotificationState` reports `QUNS_RUNNING_D3D_FULL_SCREEN` / `QUNS_BUSY`.
  - Also detected from known launcher install folders (Steam, Epic, Riot, Xbox, GOG, EA, Ubisoft).
- **Video**
  - Video players (VLC, MPC, PotPlayer, mpv, Media Player, Netflix, Prime Video…).
  - Browsers, when the title shows a video site (the same rules as audio Auto) *and* the browser is actually playing sound. User-configurable.
- **Work**
  - Editors and IDEs (VS Code, Antigravity, Visual Studio, JetBrains IDEs).
  - Office apps, PDF readers, terminals.
- **Per-app rules** chosen by the user always win. Manual mode turns Auto off.

## 6. Scenes: picture and sound together

| Scene | Picture | Sound (when the sound mode is Auto) |
|---|---|---|
| Movie / video | CINEMA | Movie preset (CINEMA) |
| Game | GAMING | GAMING |
| Work / coding | WORK | Balanced: the music preset if music is playing, otherwise MUSIC |
| Music | default picture | Music preset (CONCERT HALL) |
| Anything else | default picture (user setting) | Existing sound Auto |

Manual picture and sound controls always override Auto.

## 7. Implementation map

| Piece | File |
|---|---|
| Colour maths, presets, 3D LUT, analysis | `engine/src/cinemax/vision/Picture.*` |
| Vision Engine (profiles, Auto, apply/restore, JSON) | `engine/src/cinemax/vision/VisionEngine.*` |
| Display detection, HDR state, backlight, gamma ramp, Night Light | `win/src/Displays.cpp` |
| Intel display pipe (3D LUT, sharpness) | `win/src/IntelPipe.cpp` + `third_party/igcl/igcl_api.h` |
| Crash-safe restore state (used by CinemaxGuard) | `win/src/VisionState.cpp` |
| Foreground app + classification | `win/src/ForegroundApp.cpp` |
| Re-apply on display/power/unlock events | `app/src/Main.cpp` (SessionWatcher) |
| Picture screen | `ui/src/screens/Picture.tsx` |

Tests: `tests/DspTests.cpp → testPicture()` checks:
- neutral settings are an exact identity;
- the white point (6500 K is unchanged);
- every preset keeps a monotonic grey ramp, black stays black, and at least 20 of 25 near-black and near-white steps survive;
- extreme user settings stay monotonic;
- vibrance protects skin tones;
- the 17³ LUT has the right size and range.

Measured on this laptop:
- the driver accepted a full 17³ LUT;
- restore-to-default works;
- IGCL output IDs match Windows target IDs (0x800050).

## 8. Picture modes (tuned 2026-11-21)

The modes are tuned to look as different as TV picture modes on this SDR panel, without
crushing blacks or clipping whites. `testPicture()` enforces it:
- every mode keeps at least 20 of 25 near-black and near-white steps;
- black stays black;
- every mode differs visibly from the original picture (mean OKLab ΔE ≥ 3 on natural colours);
- vibrance at maximum adds at most 12 % colour to skin tones.

| Mode | White point | Tone | Colour | Sharpness | Backlight |
|---|---|---|---|---|---|
| CINEMA | 5400 K (warm) | gamma 2.45, contrast +24, black detail +9 | sat +10, vibrance 42 | off | −8 |
| HDR EXPERIENCE | 6900 K | contrast +40, shadow lift +15, brightness +8 | sat +26, vibrance 80 | 2 | +20 |
| VIVID | 9300 K (cool) | contrast +34, brightness +8 | sat +50, vibrance 100 | 3 | +25 |
| GAMING | 7200 K | gamma 1.9, shadow lift +20 | sat +22, vibrance 75 | 3 | +12 |
| WORK | 6200 K + 25 % blue-light cut | contrast −4, brightness −5 | sat −8 | 1 | −5 |
| NIGHT | 3800 K + 55 % blue-light cut | brightness −18, contrast −8 | sat −10 | off | −30 |

**Backlight** (*Picture modes set the backlight*, on by default):
- Each mode moves the real panel backlight relative to *your* level, like a TV.
- Your level is remembered, and so are your own changes from the keyboard keys or the Windows slider: a mode switch never snaps those back.
- Your level is restored on exit, on *Restore original display*, while holding *compare*, and by `CinemaxGuard` after a crash.

**Colour limits:** saturated colours are already near the edge of the SDR signal range, so the
colour boost is eased off there. Mid-saturation colours (sky, foliage, clothing) carry most
of the change.
