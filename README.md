# CINEMAX

**Real-time cinematic audio enhancement for Windows.** CINEMAX routes everything Windows
plays (YouTube, Netflix, Spotify, games, browsers and so on) through a native C++ DSP
engine, then on to your laptop speakers, Bluetooth earbuds, wired or USB headphones,
or external speakers.

* 10-band parametric EQ with bass / mid / treble tone controls
* Virtual surround: HRTF binaural rendering on headphones, crosstalk-cancelled
  widening on speakers, room ambience, and real 5.1 / 7.1 / 7.1.4 input support
* **3D height** (Atmos-style; not Dolby Atmos): virtual ceiling speakers with pinna
  elevation cues, an up-mixed overhead layer for stereo content, and real top
  channels placed overhead
* Bass enhancement: shelf, sub-bass, psychoacoustic harmonics for small drivers,
  LF limiter and speaker protection
* Dialogue clarity with background control
* Dynamics: BS.1770 loudness normalisation, compressor, and a look-ahead peak
  limiter with a guaranteed ceiling
* **Music / Movies / Auto** switch in the top bar. Auto detects whether music or a
  film/video is playing and switches presets by itself.
* **Smart tone**: automatic bass and treble per song or film (gentle, limited corrections)
* **Venue reverb**: from a small theatre to a large concert hall (0.45–2.1 s), with
  the **CONCERT HALL** preset for music that sounds played live
* **Real ears** on headphones (Apple-style spatial audio): virtual speakers rendered through
  measured HRTFs of real heads (6 sets from the University of York's SADIE II database:
  2 studio dummy heads and 4 people), with a one-minute **ear-fit test** that walks a sound
  round your head so you can pick the ears that sound most real to you
* **Head tracking with the webcam** (optional): CINEMAX follows your head so the sound stays
  on the screen when you turn, like AirPods. Runs only while it can be heard (headphones,
  surround on, real ears), nothing is recorded or saved, and the camera is shared with other apps
* **Smart EQ** (Dolby-style intelligent equalizer): measures the tonal balance of what is
  playing and slowly steers it to a target (Balanced / Warm / Detailed)
* **DOLBY-STYLE**, CINEMAX's own processing modelled on what Dolby's does (no Dolby code inside):
  * **Atmos-style 7.1.4 up-mix** (`dsp/Upmixer.*`): stereo is split frequency by frequency
    (short-time FFT, primary/ambience decomposition) into 11 virtual speakers. Voices mixed
    in the middle play from a real centre speaker, ambience wraps around and above you
    (4 ceiling speakers), and matrix-encoded film surround (anti-phase) goes behind you.
    Bass below ~120 Hz is never split. It adds one ~11 ms frame of delay.
  * **Speech-aware dialogue**: the voice lift acts on the extracted centre only while speech
    plays, and the ambience steps back under it.
  * **Directional room** on headphones: early reflections from the walls and ceiling,
    carrying the speakers that face them, put the sound outside your head without a long echo.
  * **Volume-aware tone** (`dsp/VolumeModeler.h`): follows the output device's volume and adds
    back the bass (up to +9 dB) and top (up to +3 dB) that quieter listening takes away.
* **TV section** (laptop on a TV over HDMI, or a soundbar / AV receiver):
  * TV sound: tuning for TV speakers / soundbar / AV receiver, virtual surround sized to the
    screen and your seat distance, lip-sync delay (0-300 ms), and a check of which Dolby
    formats the TV decodes, with a "Let the TV decode Dolby" switch (CINEMAX steps aside so
    apps can send the original Dolby track; it resumes on any other output)
  * TV picture (Intel graphics): HDMI black level Full / Limited (fixes washed-out or crushed
    blacks), telling the TV whether a film, game or desktop is on so it can switch its own
    picture mode, and the TV's low-latency game mode (ALLM) while gaming
* Presets: CINEMA, IMAX-STYLE, DOLBY-STYLE (all of the above), THEATRE, CONCERT HALL, MUSIC, DEEP BASS, GAMING, DIALOGUE, NIGHT MODE, CUSTOM
* A separate profile per output device, auto-switching when devices connect or
  disconnect, and safe Bluetooth reconnection handling
* Live spectrum, waveform, L/R meters, bass activity and peak-protection / clip
  indicators, all computed from the engine's real output
* Crash-proof routing: a watchdog process, session-end and next-logon recovery,
  plus a one-click restore
* **CINEMAX Vision: Smart-TV-style picture processing** for the whole screen.
  * Presets: CINEMA, HDR EXPERIENCE, VIVID, GAMING, WORK, NIGHT, CUSTOM.
  * Controls: brightness, contrast, black level, gamma, saturation, vibrance with skin-tone
    protection, colour temperature, blue-light reduction, 2-point white balance,
    hardware sharpness and the panel backlight.
  * On Intel graphics all of it runs in the GPU's **display pipe** (a 17³ 3D colour table
    plus a hardware sharpening filter). There's no overlay and no screen capture, it adds
    no GPU load, and it applies to every app and game. Other GPUs fall back to the Windows
    gamma ramp.
  * **Auto picture** follows the app in front and also switches the sound: films get CINEMA
    picture and sound, games GAMING, coding WORK.
  * Per-monitor profiles, *Hold to see the original* for a real before/after, and one-click
    *Restore original display*. See [docs/VISION-ENGINE.md](docs/VISION-ENGINE.md) for what
    is hardware, what is processing, and the limits.

> CINEMAX is not affiliated with IMAX or Dolby and holds no certification from them.
> "IMAX-STYLE" is a descriptive preset name.

How it works, and why a virtual audio device is required: see
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Requirements

| What | Why |
|---|---|
| Windows 10 2004+ / Windows 11, x64 | WASAPI loopback, WebView2 |
| **VB-CABLE** (free, https://vb-audio.com/Cable/) | The virtual device apps play into. Install as administrator and reboot if asked. Voicemeeter's virtual inputs also work. |
| Microsoft Edge WebView2 Runtime | UI (ships with Windows 11) |

## Using it

1. Install VB-CABLE, then start CINEMAX.
2. Flip the master switch **ON**. Windows' default output becomes *CABLE Input*.
   CINEMAX plays the processed sound on the device you were using.
3. Pick a preset, or fine-tune in *Equalizer* and *Sound Effects*. Settings are
   saved per output device.
4. Switching devices works either in *Audio Devices* or in the normal Windows
   sound flyout. CINEMAX adopts whatever you pick.
5. Flip the switch **OFF** (or quit) and your normal Windows routing comes back.

**Picture:** open *Picture*.
* **Choosing a picture:** pick a preset, or leave **Auto** on to follow the app in front.
* **Adjusting:** every slider changes the real display at once. Hold *Hold to see the
  original* to compare with the unprocessed picture.
* **Saving:** profiles are saved per monitor.
* **Undoing everything:** *Restore original display* (also in the tray menu) puts the screen
  back exactly as it was.

Closing the window keeps CINEMAX running in the tray (configurable). If you ever
hear nothing, see [docs/RECOVERY.md](docs/RECOVERY.md). Other problems:
[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## Building from source

Prerequisites:

* Visual Studio 2022/2026 **Build Tools** with *Desktop development with C++*
  (MSVC + Windows SDK + CMake/Ninja). JUCE 9 does not support MinGW.
* Node.js 20.19+ (for the UI bundle), Git
* Optional: Inno Setup 6 (`winget install JRSoftware.InnoSetup`) for the installer

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1             # UI + app + tests -> dist\CINEMAX
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Installer  # also dist\CINEMAX-Setup-1.0.0.exe
```

`build.ps1` imports the MSVC environment and builds the React UI into one
self-contained `index.html`. It then configures CMake (Ninja) and builds
`CINEMAX.exe`, `CinemaxGuard.exe`, `cinemax-cli.exe` and `cinemax_tests.exe`, runs the
DSP tests and stages everything in `dist\CINEMAX`. Heavy build artefacts (JUCE checkout,
CMake tree, `node_modules`) live in `%USERPROFILE%\CINEMAX-dev`, outside OneDrive.
JUCE 9.0.6 and the WebView2 SDK are fetched automatically if they're missing.

Manual CMake (from a *x64 Native Tools* prompt):

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCINEMAX_UI_HTML=<path\to\index.html>
cmake --build build
```

## Project layout

```
engine/    C++ audio engine (JUCE): dsp/, audio/ (WASAPI pipeline), core/ (controller, presets, profiles)
win/       JUCE-free Win32 layer: endpoint classification, default-device switching, recovery state
app/       CINEMAX.exe: JUCE window + WebView2 host, tray icon, UI bridge
guard/     CinemaxGuard.exe: recovery watchdog
tools/     cinemax-cli (diagnostics), make_icon.py
tests/     offline DSP test-suite
ui/        React + TypeScript UI (Vite single-file build); contract in ui/src/bridge/types.ts
installer/ Inno Setup script
docs/      architecture, recovery, troubleshooting
```

## Testing each stage independently

```powershell
cinemax_tests.exe                                    # DSP maths, limiter guarantee, presets, no-allocation check
cinemax-cli devices                                  # what CINEMAX detects (read-only)
cinemax-cli process in.wav out.wav --preset cinema   # processing only, offline
cinemax-cli run --source default --output default --mute-output --seconds 10
                                                     # capture + DSP + render timing, silent, no routing change
cinemax-cli run --source <cable id> --output <speaker id> --preset cinema
                                                     # audible live test without touching Windows routing
```

## Verification status

See the section at the end of [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#8-verification-status)
for what has been tested and how.

## Licence notes

See [LICENSE-NOTICE.md](LICENSE-NOTICE.md). JUCE is AGPLv3 or commercially licensed.

## CINEMAX Mobile (Android)

The phone version lives in [android/](android/README.md). It uses Android's built-in
effects on the phone's global output, so every app is processed. A ready-built APK
is at `dist\CINEMAX-Mobile-1.0.0.apk`.

---

Developed by Nani Chowdary
