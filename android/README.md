# CINEMAX Mobile (Android)

The Android version of CINEMAX. It keeps the same look, presets and
Music / Movies / Auto switch as the Windows app, and has two sound engines.

## 1. CINEMAX engine (default): the same DSP as the laptop

The phone runs the laptop's processing code. `engine/src/cinemax/dsp` and
`core/Params.cpp` are compiled unchanged with the Android NDK into `libcinemax.so`.
That gives the phone the same presets and sound as the laptop: 3D binaural surround
with height, the concert-hall reverb, the bass enhancer with protection, dialogue
enhancement, Smart tone, loudness levelling and the look-ahead limiter.

```
other apps' audio ──(Android internal audio capture)──► capture thread ─► lock-free FIFO
                                                                             │
           AudioTrack ◄── CINEMAX DspChain ◄── drift-compensating resampler ◄┘  (render thread)
other apps' own output ──► muted (DynamicsProcessing at −120 dB on their audio session)
```

* **Capture.** `AudioPlaybackCapture` takes media and game audio from other apps.
  Android only allows it after the user OKs its *"start recording or casting"* prompt,
  which it shows every time capture starts (an Android rule). CINEMAX uses only the
  sound: it never creates a screen image, and nothing is recorded or stored.
* **Clock drift.** The capture clock and the speaker or Bluetooth clock run
  independently. The same PI controller as the Windows WASAPI pipeline nudges the
  resampler (±ppm), so the buffer never runs dry and the delay doesn't creep up.
* **Muting the originals** (`SessionMuter.kt`). CINEMAX finds other apps' audio
  sessions through `dumpsys audio`. That needs the `DUMP` permission, granted once
  over USB:
  ```
  adb shell pm grant app.cinemax.mobile android.permission.DUMP
  ```
  CINEMAX then attaches a muting effect to each session. Android's capture takes the
  audio before effects, so CINEMAX still receives it.
* **Safety:**
  * CINEMAX only mutes apps that allow capture. It checks both the player's attribute
    flags and the app's manifest flag (`ALLOW_AUDIO_PLAYBACK_CAPTURE`). Apps that block
    capture keep playing normally.
  * If a muted app plays but the capture stays silent for 4 s, CINEMAX un-mutes it
    and leaves it alone.
  * If capture or output fails, CINEMAX un-mutes everything first, then switches to
    the basic Android effects.
  * The muting effects belong to the CINEMAX process, so if CINEMAX is killed, Android
    removes them and every app is heard normally.
* **Not processed:** apps that block capture. RootlessJamesDSP lists Spotify and
  Chrome; Netflix and Prime Video are expected to block it too. Also apps that play
  through exclusive low-latency (MMAP) streams. These all play unprocessed.
* **Delay:** about **0.3 s**. Most of it is Android's own capture route (its internal
  submix pipe and record thread). CINEMAX's own buffers are about 50–60 ms. That's
  fine for music, but voices in videos can lag behind lips. *Settings → CINEMAX engine
  for videos* can send videos through Android's effects instead (no delay), while music
  keeps the CINEMAX engine.
* **Idle:** when nothing has played for 3 s, CINEMAX pauses its output so the phone
  can sleep.

### Verified on a Galaxy SM-E156B (Android 16), phone speaker, preset Concert Hall

The test played noise bursts as an ordinary media app and recorded them with the
phone's own microphone. Timestamps came from Android's AudioTrack and AudioRecord.

| Check | Result |
|---|---|
| Without CINEMAX | sound reaches the mic ~28–34 ms after its presentation time |
| CINEMAX engine, screen on | original muted; only the processed copy is heard, at +270 to +335 ms |
| CINEMAX engine, screen off / locked | same: original muted, processed copy at +326–330 ms, no new dropouts |
| Muting failure modes | test app's audio session un-muted automatically when it ended |
| DSP load | 15–25 % of one core's real-time budget |

### Dolby-style and Apple-style features (same code as the laptop)

* **DOLBY-STYLE** (a switch on Home, also a preset tile and a Movies-mode choice) and **Smart EQ** (Sound Effects). Smart EQ listens to the music or
  film and slowly evens its tone toward Balanced / Warm / Detailed. In the CINEMAX engine
  DOLBY-STYLE is the laptop's: the **Atmos-style 7.1.4 up-mix** (11 virtual speakers, voices
  on a real centre, 4 overhead, film surround decoded), speech-aware dialogue and reflections
  from each speaker's direction. **Volume-aware bass** works in both engines: it follows the
  media volume and adds back the bass and top that a low volume takes away (Android's effects
  fold it into their EQ). Android's effects ignore Smart EQ and the up-mix.
* **3D ears** (Sound Effects). Headphone surround uses measured real-ear recordings (SADIE II,
  `assets/hrtf`) instead of the head model. The dummy head is used by default. **Find my ears**
  plays a test walk (front, right, behind, left, above) with each set on its own output, and
  works with either engine. CINEMAX's own sound is silenced during the test.
* **Head tracking** (front camera, optional, off by default):
  * The laptop's face analysis (`engine/src/cinemax/head/FaceAnalyser.*`, BlazeFace through
    ONNX Runtime for Android) runs on ~640×480 YUV frames, at most 15 per second. The pose goes
    straight to the audio engine.
  * Which way up the picture is comes from the sensor and screen rotation. It is checked by
    reading the frame all four ways, because an upright face scores ~0.9 and an upside-down
    one ~0.65.
  * The camera runs only while it can help: CINEMAX engine on, headphones, surround on, real
    ears, and something played in the last 15 s.
  * Android only lets a background service use the camera if it declared the camera while
    CINEMAX was on screen. CINEMAX does that when you switch tracking on, when you open the app
    and when the engine starts.
  * Frames are analysed in memory and dropped.
* **TV tab** (always in the bottom bar; the settings appear for Bluetooth, HDMI and USB outputs):
  * Bluetooth TVs are recognised by name ("[TV] Samsung…", BRAVIA, webOS…), or you can mark
    the output yourself.
  * A TV is never treated as headphones.
  * It gets TV-speaker / soundbar / receiver tuning, surround sized to screen and seat, and
    lip-sync, all from the same `TvTuning` code as the laptop.
  * The TV's picture settings are laptop-only.

The phone app has no picture (display) module: Android lets no app recolour the screen (the colour matrix is refused even to the USB shell on the Galaxy F15), and the TV tab is about sound only.

## 2. Android effects (fallback, or chosen in Settings)

Android's built-in effects on the global output (audio session 0). There's no
capture prompt and no added delay, but the sound is simpler. CINEMAX also uses these
while the full engine isn't running, for example after a restart, until you open
the app and OK the prompt.

| Stage | Android effect |
|---|---|
| 10-band EQ, bass, dialogue, compressor, limiter | DynamicsProcessing (insert) |
| Virtual surround (headphones & earbuds) | Virtualizer, forced to headphone mode (most earbuds report the generic Bluetooth type, which Android's virtualizer otherwise ignores) |
| Room / hall ambience | **Insert** Environmental Reverb, selected by UUID. The public class always picks the auxiliary reverb, which is silent on the global output. Wet level = ROOM_LEVEL + REVERB_LEVEL − 2000 mB. |

**Hold to hear the original** (Home screen) bypasses either engine while pressed.

**Auto** reads the content type that playing apps declare to Android: video apps
report MOVIE, music apps report MUSIC.

## Build and install (USB debugging)

```powershell
# 1. Native engine (needs the Android NDK, e.g. r28b, in %USERPROFILE%\CINEMAX-dev\ndk)
powershell -ExecutionPolicy Bypass -File android\build-native.ps1
# 2. App
$env:JAVA_HOME = "C:\Program Files\Eclipse Adoptium\jdk-17.0.21.9-hotspot"
cd android
.\gradlew.bat --offline --project-cache-dir "$env:USERPROFILE\CINEMAX-dev\android-gradle-cache" assembleDebug
adb install -r "$env:USERPROFILE\CINEMAX-dev\android-build\app\outputs\apk\debug\app-debug.apk"
adb shell pm grant app.cinemax.mobile android.permission.DUMP
```

Build output goes to `%USERPROFILE%\CINEMAX-dev\android-build`, outside OneDrive.
A ready-built APK is in `dist\CINEMAX-Mobile-1.0.0.apk`. It's debug-signed, which is
fine for installing on your own phone.

## Tips

* For the true CINEMAX sound, turn off Samsung's **Dolby Atmos**, **Adapt sound**,
  **Equaliser** and **Loudness normalisation** (Settings → Sounds and vibration → Sound
  quality and effects). They process the sound again *after* CINEMAX. The Home screen
  warns when it detects them, and its button opens that page.
* To compare with the laptop, use the same preset on both: the laptop remembers a
  preset per device (e.g. your TWS earbuds), while the phone's Auto mode picks Concert
  Hall for music.
* Set Settings → Battery to **Unrestricted** for CINEMAX, so processing keeps running in
  the background.

Developed by Nani Chowdary
