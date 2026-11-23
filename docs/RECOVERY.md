# Audio routing recovery

While CINEMAX is **on**, Windows plays everything into a virtual device ("CABLE Input"),
and CINEMAX forwards the processed sound to your speakers or headphones. If you ever
hear nothing, use one of these. They are listed from easiest to most manual.

## 1. In CINEMAX
* **Settings → Restore default audio routing**
* Or right-click the tray icon → **Restore Windows audio routing**

Either one turns the engine off and sets your real speakers/headphones back as the
Windows default output.

## 2. Start menu shortcut
**Start → CINEMAX → CINEMAX – Restore Audio** runs `CinemaxGuard.exe --restore`.
If CINEMAX is still running (for example, hung), it asks for confirmation, closes it
and restores your output.

## 3. Command line
```bat
"%ProgramFiles%\CINEMAX\CinemaxGuard.exe" --restore
```
Add `--quiet` to skip the dialogs.

## 4. Windows Settings (always works)
1. Press **Win + Ctrl + V**, or click the speaker icon in the taskbar → the `>` next to the volume slider.
2. Pick your speakers or headphones instead of **CABLE Input**.

You can also go to **Settings → System → Sound → Output** and choose your device.

## What happens automatically
* **CINEMAX closes or crashes:** `CinemaxGuard.exe` notices and restores your output within about half a second.
* **You log off, restart or shut down:** CINEMAX restores the output before Windows ends the session.
* **Power loss or blue screen:** at the next logon, a one-time entry runs the guard. It restores the output unless CINEMAX starts and takes over.
* CINEMAX only restores while Windows still points at its virtual device. If you already chose another device yourself, it is left alone.

## Picture (CINEMAX Vision)
* **In CINEMAX:** *Picture → Restore original display*, or tray icon → *Restore original display*.
* **CINEMAX closes or crashes:** `CinemaxGuard.exe` undoes every display change: Intel colour
  table back to default, original sharpness, and the original gamma ramp.
* **Restart:** the display pipe starts clean (CINEMAX never makes its colour table persistent).
* `%LOCALAPPDATA%\CINEMAX\vision-state.ini` records what was changed, so the guard can undo it.
  Picture profiles live in `vision.json`.

## Files
* `%LOCALAPPDATA%\CINEMAX\routing-state.ini` records the state used for recovery. It's safe to delete while CINEMAX is closed.
* `%LOCALAPPDATA%\CINEMAX\logs\cinemax.log` and `guard.log` contain diagnostics.
