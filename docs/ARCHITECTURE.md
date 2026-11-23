# CINEMAX architecture

## 1. The problem: system-wide processing on Windows

Windows lets apps play to an output device through the audio engine (`audiodg.exe`).
It has no supported, user-mode way to put a processor between *every* app and the
speakers. These are the options:

| Approach | How it works | Verdict |
|---|---|---|
| **WASAPI loopback only** | Capture what the speakers play | **Rejected.** Loopback is a copy. The original keeps playing, so you'd hear both. It can't replace the output. |
| **System APO** (Equalizer APO's approach) | A COM DLL registered as an SFX/MFX effect on each endpoint, loaded inside `audiodg.exe` | No added latency, but it needs admin and writes to protected `MMDevices\...\FxProperties` keys. A bug in the DLL crashes `audiodg` and takes all audio on that endpoint down with it. Many modern drivers ignore third-party APOs, including the Intel Smart Sound / Bluetooth offload path on this laptop. Driver updates also reset it, and visualisation needs IPC out of `audiodg`. Not chosen for v1. |
| **Custom virtual audio driver** (FxSound / Boom 3D) | A kernel-mode virtual endpoint that the app reads from | Needs kernel driver development, EV code signing and Microsoft attestation signing. Not feasible here. |
| **Virtual cable + user-mode engine** ✅ | Windows' default output points at a signed virtual device (VB-CABLE). CINEMAX loopback-captures it, processes the audio and renders to the real device. | **Chosen.** All processing runs in user mode, so there's no kernel code to write. It works with every output, Bluetooth included, and it's crash-safe with guaranteed recovery. Costs ≈ 25–60 ms of extra latency and needs clock-drift correction. |

## 2. Signal path

```
 Apps (YouTube, Netflix, Spotify, games, browsers …)
        │  Windows default output = "CABLE Input" (console + multimedia roles only)
        ▼
 VB-CABLE virtual render endpoint ──► WASAPI loopback capture (shared, polled, 200 ms buffer)
                                              │ (any rate, 1–8 channels, real 5.1/7.1 supported)
                                              ▼
                       Resampler (Kaiser-windowed sinc, 16/32 taps, 256 phases)
                       + PI drift controller on FIFO level (±2000 ppm)
                                              ▼
 DSP chain:  volume compensation → dialogue → spatial (HRTF / speaker virtualiser + room)
             → 10-band EQ + tone → bass (shelf, sub, harmonics, LF limiter, HPF)
             → loudness normaliser (BS.1770) → compressor → bypass crossfade → look-ahead limiter
                                              ▼
 WASAPI shared-mode event-driven render ──► real device (laptop speakers, BT earbuds, USB, …)
```

* **Communications role untouched.** Teams, Discord and phone calls keep using the real device directly. That keeps call latency low and doesn't fight Bluetooth hands-free mode.
* **One real-time thread.** It runs on the render event with MMCSS "Pro Audio" priority. Each wake it drains the loopback packets into the resampler FIFO, pulls exactly the frames the device needs, processes them and writes them. Capture and render never need a lock between them.
* **Drift control.** The virtual cable and a Bluetooth headset run on independent clocks. A PI controller nudges the resampling ratio, using the smoothed FIFO level as its input. Stalls larger than about 80 ms are dropped to resynchronise; this is counted as an xrun. When the source goes silent (no packets), the pipeline goes back to priming instead of reporting errors.
* **No allocation, locks or system calls in the loop.** Parameters reach the audio thread through a wait-free triple buffer. Meters go back through atomics and a lock-free FIFO (`juce::AbstractFifo`). Denormals are flushed. The test-suite checks for zero heap allocations in `DspChain::process` and the resampler.

## 3. Volume handling

While CINEMAX is on, the Windows volume flyout and the volume keys control the *virtual* device. `VolumeMirror` handles this in three ways:

1. It copies the real device's volume onto the virtual device when routing starts, so loudness doesn't jump.
2. It mirrors later virtual-device volume changes onto the real device. That applies hardware or Bluetooth absolute volume, and works the other way too (earbud buttons → flyout).
3. It undoes the virtual device's software attenuation before the DSP. Loopback of a software-volume endpoint is post-volume, so without this step the volume would be applied twice. It also keeps the dynamics and the loudness normaliser from fighting the user's volume setting.

The real device always carries the current volume, so turning CINEMAX off never makes audio suddenly louder.

## 4. Safety and recovery (never leave Windows without audio)

| Layer | Mechanism |
|---|---|
| State file | `%LOCALAPPDATA%\CINEMAX\routing-state.ini` is written **before** the default device changes. It records `engaged`, pid, source and restore target. |
| Guard process | `CinemaxGuard.exe --watch <pid>` waits on the CINEMAX process handle. If CINEMAX dies while engaged, the guard hands the default back to the real device. |
| Session end | A hidden window handles `WM_ENDSESSION` and restores routing synchronously before logoff or shutdown. |
| Power loss / BSOD | While engaged, an HKCU `Run` entry runs `CinemaxGuard.exe --restore-if-stale` at the next logon. It's removed on clean disengage. |
| Next launch | CINEMAX itself repairs stale state at startup. |
| Manual | Start menu → **CINEMAX – Restore Audio**, the tray menu, or Settings → *Restore default audio routing*. |
| Respect the user | Restore only acts if Windows still points at the virtual device. A device the user picked in the meantime is never overridden. |
| Anti-fight | If another tool keeps changing the default device (more than 6 times in 10 s), CINEMAX turns itself off instead of fighting it. |

## 5. Device policy

* Endpoints are classified from `PKEY_Device_EnumeratorName` (`BTHENUM`, `BTHHFENUM`, `INTELAUDIO`, `HDAUDIO`, `USB`, `ROOT`), `PKEY_AudioEndpoint_FormFactor` and the names. The rules were checked against this machine's real devices.
* Bluetooth **hands-free (HFP)** endpoints are never auto-selected, because they are narrow-band call audio. CINEMAX never opens a Bluetooth microphone, so the A2DP quality profile stays active.
* **Output choice order:** explicit user choice → (auto-switch off: current device) → most recently used active devices → Windows default (if real) → built-in speakers → headphones → USB → anything usable.
* If the user picks a device in the Windows flyout while CINEMAX is on, it becomes the new CINEMAX output and routing is reasserted.
* With auto-switch on, newly connected headphones, Bluetooth or USB devices take over the output. When a device disconnects, CINEMAX falls back through the history. Devices that fail to open three times are skipped for 30 s.
* Each output device has its own profile (`%LOCALAPPDATA%\CINEMAX\profiles\<hash>.json`). A device with no saved profile starts on the user's **"sound for new devices"** preset (Settings; default **CINEMA**, or FLAT for neutral). Every preset keeps the limiter and bass protection active. The engine turns on when the app opens (Settings → *Start engine on launch*), so opening CINEMAX is enough to hear the effect.

## 6. Process and module structure

```
CINEMAX.exe      JUCE GUI app: message thread = engine controller + WebView2 UI + tray
  ├─ engine/     (JUCE-using C++; compiled into each consumer)
  │    core/     Engine (policy, routing, profiles), Params/presets, Settings JSON,
  │              DeviceMonitor (IMMNotificationClient), Metering (MeterBus + FFT Analyzer)
  │    audio/    WasapiPipeline (RT thread), VolumeMirror
  │    dsp/      Filters, Equalizer, BassEnhancer, DialogueEnhancer, Spatializer
  │              (+RoomAmbience), Dynamics (LoudnessMeter/Normalizer, Compressor,
  │              Limiter), Resampler, DspChain
  ├─ win/        JUCE-free Win32 layer: endpoints, IPolicyConfig, routing state, logging
  └─ ui/         React + TypeScript (Vite single-file bundle embedded in the exe)
CinemaxGuard.exe recovery watchdog (win/ only, ~200 KB)
cinemax-cli.exe  diagnostics: device list, offline WAV processing, live pipeline test
cinemax_tests    offline DSP test-suite
```

The UI talks to the engine only through the contract in `ui/src/bridge/types.ts`. Calls use JUCE native functions; state, notices and 30 Hz meters come back as events. When the window is closed to the tray, the WebView is destroyed, so it costs no browser processes. The engine keeps running.

## 7. DSP details

* **EQ.** Ten RBJ peaking bands (31.5 Hz–16 kHz, editable frequency, gain and Q), a 100 Hz shelf, a 1 kHz peak (Q 0.9), an 8 kHz shelf and a preamp. All parameters glide over about 30 ms. Bands above 0.45·fs (e.g. at 16 kHz hands-free) are bypassed.
* **Bass.** A shelf at a user-set frequency, a 45 Hz sub peak, and a psychoacoustic harmonic generator: Chebyshev T2/T3/T4 applied to the envelope-normalised low band, band-limited to its harmonics. It also has a dynamic LF limiter with perfect reconstruction when inactive, and a device-aware protective high-pass (80 Hz for built-in speakers, 45 Hz for Bluetooth speakers).
* **Dialogue.** A voice EQ on the mid or centre channel (-250 Hz, +1.2 kHz, +3 kHz) plus a background-level control on the side or surround channels. Exact reconstruction when neutral.
* **Spatial.** On headphones it renders binaurally with a spherical-head model. Each virtual loudspeaker gets a Woodworth ITD, a Brown–Duda head-shadow filter per ear and a rear pinna shelf. Stereo is up-mixed by correlation-based centre extraction and decorrelated surround ambience. On speakers it widens the image with crosstalk cancellation limited to 300 Hz–3 kHz, keeping the bass mono. A room model (early reflections plus a 4-line Hadamard FDN) adds theatre space. Real 5.1, 7.1 and 7.1.4 from the virtual device are rendered channel by channel.
* **Listening modes and Smart tone** (`dsp/ContentAnalyzer`, `core/Engine`).
  * **Music / Movies / Auto** is a global mode. Music and Movies apply the preset chosen in Settings (default CONCERT HALL / CINEMA). Auto follows the content analyser. Picking another preset or editing anything switches to *manual*. A newly connected device follows the active mode.
  * **App / site recognition (checked first, every second).**
    * Windows audio sessions on the virtual device are listed (`win/AudioSessions`), and the loudest *audible* one is matched by executable (VLC, MPC, PotPlayer, Kodi, Plex, Spotify, YouTube Music, …) or by window title, for browser tabs (Netflix, Prime Video, Disney+/Hotstar, JioCinema, Spotify, JioSaavn, …).
    * A match that stays stable for 2 s decides; there's no sound analysis wait.
    * Only the matched label is shown or logged; titles are never stored.
    * Switching to Auto keeps the current Music/Movies preset until something is recognised.
    * `cinemax-cli sessions` shows what is playing and the hint.
  * **YouTube / YouTube Music play both, so the title decides.** An ordered keyword list runs from specific phrases to general words, and the first match wins:
    * film phrases: "full movie", "dubbed", "web series", "HDRip/WEBRip/BluRay"
    * song phrases: "video song", "lyrical/lyrics", "jukebox", "official audio"
    * streaming services
    * general words, film: "trailer", "episode", "scene", "movie", "film", "1080p"
    * general words, music: "song", "album", "ft.", "music"
    * Video players playing an audio file (.mp3/.flac…) count as music.
  * **Content analyser (fallback, heuristic, input side, per 10 ms frame).**
    * Speech evidence (syllabic modulation, pauses, loudness spread) pushes towards film, and **rhythm regularity** (autocorrelation of 3-band onset flux over 6 s, lags 0.3–1.5 s) pushes towards music: `p = 0.48 + 0.8·speech − 0.65·beat`.
    * This fixed the "everything is music" bias on films with a music bed: a synthetic dialogue-over-score scene now scores 0.65, against ≈0.15 before.
    * Once a film is detected, 12 s of clear music (p < 0.30) is needed to switch back, so song sequences inside films don't flip the preset.
    * Recognised titles still switch in 2 s. It looks at four things:
    * voice-band envelope modulation around 5 Hz (syllabic rhythm)
    * pause ratio (frames >25 dB below the running peak)
    * loudness spread across 400 ms windows
    * active surround or LFE channels
  * These combine into a smoothed film probability. The engine switches only after ~5 s of real audio and 3 consecutive seconds above 0.62 or below 0.38, and holds during silence.
  * **Smart tone.** Measures the long-term bass (40–160 Hz) and treble (5–16 kHz) share of the input and compares it with the balance of mastered music (−5 / −15 dB).
    * Boosts correct half the difference; bass cuts use 1.5× (shares compress near 0 dB) and treble cuts 0.8×.
    * Limits: bass +5/−4 dB (boost capped near +1 dB on films), treble +3/−4 dB, rate-limited to 1 dB/s.
    * Applied by two gliding shelves (110 Hz / 6 kHz) after the EQ.
* **Venue reverb (theatre → concert hall).**
  * Signal path: early reflections whose spacing scales with the venue → pre-delay (8–38 ms) → 4 Schroeder diffusers → an 8-line FDN.
  * The FDN uses a fast Walsh–Hadamard mixing matrix, per-line air-absorption damping (darker in bigger halls), and two slowly modulated lines (0.31 / 0.47 Hz) to avoid metallic ringing.
  * *Venue* (0..1) sets RT60 from 0.45 s up to 2.1 s. It was calibrated against measured Schroeder T30, so measured values sit within 3 % of the displayed seconds.
  * All changes glide, so there are no clicks.
  * The **CONCERT HALL** preset (music in a live hall) uses venue 0.65 (≈1.5 s), 40 % room, a wide stage and 15 % height. Dynamics stay untouched, and the level matches the input.
  * It was tuned down after listening feedback ("too much echo"):
    * Early reflections are kept within ≈50 ms.
    * The pre-delay is at most 28 ms.
    * On speakers, room and reflections are scaled to 65 %, because the real room adds its own reverb.
    * Result: measured delayed energy fell by ≈8 dB on speakers and ≈5 dB on headphones in every time window.
  * Untouched factory presets in saved profiles reload from the current definition, so tuning changes reach existing users. CUSTOM profiles are kept as stored.
* **3D height (Atmos-*style*, not Dolby Atmos).** Every virtual speaker now has a true 3-D direction. ITD and head shadow come from the actual direction vector, so raised sources lose interaural level difference. Raised sources also get the pinna elevation cues: Blauert's "above" band at 8 kHz (up to +5 dB at 45°) and a dip at 3.6 kHz.
  * Height speakers sit at ±45°/±135° azimuth and +45° elevation, with a top-centre speaker at +60°.
  * Real top channels (TFL/TFR/TBL/TBR/TC) are rendered at their positions.
  * For stereo or 5.1/7.1 content there are no top channels, so a height layer is up-mixed instead. It takes left/right-leaning ambience plus some mid signal, high-passes it at 600 Hz (bass stays grounded), delays it 7–9 ms and decorrelates it, then places it on virtual ceiling speakers. Its level scales with *3D height* × *intensity*.
  * On speakers, the same elevation EQ is applied to a height bus mixed into the stereo image.
  * Preset amounts: IMAX-STYLE 60%, CINEMA 35%, THEATRE 30%, GAMING 20%, DEEP BASS 15%.
* **Dynamics.** An open-loop BS.1770 short-term loudness normaliser, gated at -50 LUFS, with rate-limited gain. A soft-knee compressor with its sidechain high-passed at 90 Hz. A 1.5 ms look-ahead limiter whose ceiling is mathematically guaranteed: a sliding minimum, then release, then a moving average, all over the look-ahead window.
* **Presets** live in `engine/src/cinemax/core/Params.cpp`. The UI never hard-codes DSP values.

## 8. Verification status

Last run: 2026-11-20, MSVC 14.52, JUCE 9.0.6, Windows 11, on a Dell laptop with Cirrus Logic speakers and "TWS" Bluetooth earbuds.

**Verified**
* `cinemax_tests`: 672 checks, 0 failures. Covered:
  * RBJ filter maths and transparency of the neutral preset (bit-for-bit within 1e-5, after the limiter latency)
  * EQ band accuracy (±0.15 dB) and the bypass path
  * Binaural level and stereo image, and BS.1770 calibration (-20 LUFS ±0.15)
  * Resampler error: -77 dB standard and about -100 dB HQ for 48→44.1 kHz; the drift ppm is exact
  * The limiter ceiling is never exceeded, at three ceilings and +6 dBFS overload
  * Every preset × mono/stereo/5.1/7.1 × 16/44.1/48/96 kHz × headphone/speaker mode stays finite, within the ceiling, and decays to silence
  * **Zero heap allocations** in `DspChain::process` and the resampler
* Live, on real hardware (`cinemax-cli run --mute-output`): Bluetooth earbuds were loopback-captured, run through the DSP and rendered back to the earbuds (silent). 48 kHz, 22 ms buffer, ~39 ms estimated added latency, **~1.1–1.5 % DSP CPU**, 0 xruns. The FIFO held at its 15 ms target with drift correction converging to ~0 ppm.
* Device classification on this PC: built-in speakers → *Speakers (built-in)*, TWS → *Bluetooth earbuds*. Hands-free endpoints are excluded from automatic selection.
* `CINEMAX.exe`: the WebView2 UI loads from the embedded bundle and all 15 native functions are registered. Every screen renders live engine state. Turning the engine on without a virtual device reports `no-virtual-device` and leaves the Windows default output untouched. The guard starts with the app, sees a clean exit and does nothing.

**Verified live with VB-CABLE installed (2026-11-20)**
* **Opening the app routes everything through CINEMAX.** It switches itself on at launch, makes CABLE Input the Windows default, and starts the CINEMA preset for a new device.
* **A test signal travels end to end.** Measured by loopback on both the cable and the real output: apps → CABLE Input → CINEMAX → TWS earbuds or wired headphones, with 0 xruns, 0.6–1 % CPU, and drift converging to about +2 ppm.
* **The live dashboard shows real engine data.** The spectrum, meters and loudness (−19.2 LUFS on CINEMA) respond at about 30 fps while the window is visible. The stream pauses by design when the window is covered.
* **Presets switch live** (IMAX-STYLE / NIGHT / CINEMA), and bypass toggles cleanly.
* **The output can move between devices on the fly.** It switched from headphones to laptop speakers and back while routing stayed engaged.
* **Devices are followed automatically.** The TWS earbuds disconnected and wired headphones were plugged in, and CINEMAX followed to the wired headphones.
* **Volume mirroring works.** The cable and real device stay matched (50 %/51 %, 41 %/41 %). VB-CABLE reports hardware volume, so no compensation is applied.
* **The engine hands Windows back when turned off.** OFF restores the real device as the Windows default, and ON takes it over again.
* **Crash recovery works.** After a force-kill (simulated crash), CinemaxGuard restored the Windows default to the device in use within 0.6 s and cleared the engaged flag.
* **A bug was found and fixed.** CINEMA came out about 12 dB louder than its loudness target because compressor make-up stacked on top of the normaliser. The compressor now runs before the normaliser, with make-up off while normalising. CINEMA now adds about 5 dB on a quiet −21 LUFS input. The test-suite gained 27 preset-loudness checks (699 checks, 0 failed).
* `CINEMAX-Setup-1.0.0.exe` builds with Inno Setup 6.
* **Listening modes / Auto / Smart tone (added 2026-11-20).** 967 checks, 0 failed.
  * On synthetic material, dialogue-like scores p(movie) 0.99, music-like 0.00, and 5.1 with active surrounds 0.85.
  * Smart tone gives a thin track +5.0 dB bass, a boomy one −3.9 dB bass, and a harsh one −4.0 dB treble.
  * **Live through Windows → CABLE → CINEMAX:** dialogue switched CONCERT HALL → CINEMA after ≈9 s, and the following music switched back to CONCERT HALL ≈11 s after it began. The UI showed the mode, detection, smart-tone gains and a notice.
  * **Not yet verified on real films and songs.** Music videos, podcasts and live concerts may be judged either way; Music / Movies fix the choice.
* **Concert hall / venue reverb (added 2026-11-20).** 957 checks, 0 failed.
  * Measured RT60 0.63 / 1.29 / 1.90 s against design 0.62 / 1.27 / 1.93 s.
  * CONCERT HALL is level-matched (−20.9 LUFS in → −21.0 out).
  * Live on the laptop speakers: 0 xruns from start to finish, CPU ≤ 5.9 % (the 8-line hall costs more than the old 4-line room).
  * How it sounds has not been confirmed by a listener.
* **3D height (added 2026-11-20).**
  * 868 checks, 0 failed. The height layer measurably raises the 8 kHz elevation band (+0.65 dB on full-band noise) and keeps level within 4 dB.
  * A signal in the real 7.1.4 top-front-left channel renders on the left. Every preset × 7.1.4 stays finite and within the ceiling, with zero allocations at 12 channels.
  * Live: IMAX-STYLE (60% height) ran on the wired headphones at ≤ 2.8% CPU. Two xruns occurred at stream start, while Windows moved apps onto the cable. There were none during 18 s of silence plus playback afterwards.
  * Height *perception* has not been confirmed by a listener, only measured.
  * The bass-heavy presets (IMAX-STYLE, DEEP BASS) stack an EQ low-band boost on the bass shelf and sub-bass. A 60 Hz test tone reached about −10 LUFS momentary before the normaliser caught up. That tuning may want trimming after real listening.

**Not yet verified**
* An installed copy (via Setup.exe), as opposed to running from `dist\CINEMAX`.
* Logoff/shutdown restore (`WM_ENDSESSION`), and the logon recovery entry after a power loss.
* 5.1/7.1 input from the cable, and USB / HDMI outputs.
