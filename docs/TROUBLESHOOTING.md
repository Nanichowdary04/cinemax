# Troubleshooting

Logs: `%LOCALAPPDATA%\CINEMAX\logs\cinemax.log` (app/engine) and `guard.log` (recovery).
Diagnostics tool: `cinemax-cli devices` lists every playback device the way CINEMAX
classifies it.

| Symptom | Cause / fix |
|---|---|
| **No sound at all** | See [RECOVERY.md](RECOVERY.md). Quick fix: tray icon → *Restore Windows audio routing*. |
| **"Virtual device needed"** | Install VB-CABLE from https://vb-audio.com/Cable/ (run `VBCABLE_Setup_x64.exe` as administrator → *Install Driver*, reboot if asked), then restart CINEMAX. Voicemeeter's virtual inputs also work; choose one under *Audio Devices → Virtual source*. |
| **Windows shows "CABLE Input" as the output** | Expected while CINEMAX is on: apps play into the cable and CINEMAX plays to your real device. To switch the real output, pick a device in the Windows sound flyout. CINEMAX adopts it as its new output automatically. |
| **Volume keys don't change loudness / it's suddenly very quiet** | Keep *Settings → Compensate Windows volume* on. CINEMAX mirrors the flyout volume onto your real device. If the real device itself is at a very low level, raise it once in Windows Sound settings while CINEMAX is off. |
| **Crackles or dropouts** | Increase *Settings → Buffer* to 40 or 80 ms (Bluetooth benefits most). Check the xrun counter on the Dashboard. Close heavy CPU loads. Avoid "Exclusive mode" apps on the same output. |
| **Audio a bit behind the picture** | CINEMAX adds about 25–60 ms. Use a 10–20 ms buffer on wired/built-in outputs. Most video players have an audio-delay setting. Bluetooth adds its own ~150–250 ms regardless of CINEMAX. |
| **Echo / doubled sound** | In *Sound settings → Recording → CABLE Output → Properties → Listen*, make sure **"Listen to this device" is off**. Otherwise Windows plays the cable a second time. |
| **Bluetooth earbuds sound like a phone call** | Something opened the earbuds' microphone, so Windows switched to hands-free (HFP). Choose the "Headphones (…)" entry, not "Headset (… Hands-Free)". CINEMAX never auto-selects hands-free. |
| **Earbuds reconnect but sound stays on the speakers** | Turn on *Settings → Automatic device switching*, or pick them under *Audio Devices*. Reconnecting devices that were used before are preferred automatically. |
| **"Another app keeps changing the default playback device"** | Tools like SoundSwitch or some vendor audio suites fight over the default device. Disable their auto-switching, or don't run them together with CINEMAX. |
| **The CINEMAX window is blank** | Install or repair the Microsoft Edge WebView2 Runtime (it ships with Windows 11). Audio keeps working from the tray. |
| **Distortion on laptop speakers with heavy bass** | Keep *Bass protection* on. It enables the LF limiter and the 80 Hz speaker high-pass for built-in speakers. *Harmonics* gives the impression of bass without overdriving small drivers. |
| **5.1/7.1 content** | Set *CABLE Input → Configure* to 5.1 or 7.1 in the classic Sound control panel. CINEMAX then receives the real channels and places each one with the HRTF engine. |
| **Want everything untouched for a moment** | Use *Bypass*: routing stays, processing crossfades out (the limiter still protects against overs). Use *Engine OFF* to remove CINEMAX from the signal path entirely. |

### Picture (CINEMAX Vision)

| Symptom | Cause / fix |
|---|---|
| **Screen colours look wrong / want the normal picture back** | *Picture → Restore original display*, or tray icon → *Restore original display*. If CINEMAX was killed, `CinemaxGuard` restores the display automatically, and so does a restart (the colour table is not kept across reboots). |
| **Screenshots / recordings don't show the effect** | Expected: the picture is processed in the display hardware *after* Windows composes the screen. |
| **The effect disappeared after sleep / unplugging a monitor** | The graphics driver resets its colour pipeline on some mode changes. CINEMAX re-applies on display, power and unlock events. *Picture → Apply and save profile* also re-applies. |
| **Too warm in the evening** | Windows Night Light and CINEMAX blue-light reduction add up. Lower one of them (the Picture page shows Night Light's state). |
| **"HDR EXPERIENCE" isn't real HDR** | Correct on SDR panels (like this laptop's): it's an HDR-style tone curve. On an HDR monitor, *Picture → HDR & hardware* can switch Windows HDR on when you ask. CINEMAX never forces it. |
| **Sharpness is greyed out** | It uses the Intel display pipe's hardware filter. Other GPUs don't offer it to apps. |
| **Picture changes when I switch apps** | That's *Auto picture*. Pick a preset (Manual), add an *App rule*, or turn Auto picture off. |

## Reporting a problem
Include `cinemax.log`, `guard.log` and the output of `cinemax-cli devices`.
