package app.cinemax.mobile

import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.AudioPlaybackConfiguration
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.core.content.ContextCompat
import app.cinemax.mobile.audio.AudioEngine
import app.cinemax.mobile.audio.EarTest
import app.cinemax.mobile.capture.CaptureEngine
import app.cinemax.mobile.capture.NativeEngine
import app.cinemax.mobile.capture.SessionMuter
import app.cinemax.mobile.head.HeadStatus
import app.cinemax.mobile.audio.ListeningMode
import app.cinemax.mobile.audio.PresetId
import app.cinemax.mobile.audio.SoundParams
import app.cinemax.mobile.audio.presetParams
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import org.json.JSONObject

enum class OutputKind(val label: String, val headphones: Boolean) {
    SPEAKER("Phone speaker", false),
    WIRED("Wired headphones", true),
    BLUETOOTH("Bluetooth", true),
    USB("USB audio", true),
    HDMI("TV / HDMI", false),
    OTHER("Audio output", false),
}

/** One measured "real ears" recording (or the classic head model, key "model"). */
data class EarProfile(val key: String, val name: String, val info: String)

/** The output is a TV or soundbar: speaker tuning, surround sized to the room, lip-sync. Per device. */
data class TvSetup(
    val isTv: Boolean = false,
    val speakers: Int = 1,          // 1 TV speakers, 2 soundbar, 3 AV receiver / home theatre
    val sizeInch: Float = 55f,
    val distanceM: Float = 2.5f,
    val lipSyncMs: Float = 0f,
) {
    fun toArray() = floatArrayOf(if (isTv) 1f else 0f, speakers.toFloat(), sizeInch, distanceM, lipSyncMs)
    fun toJson(): JSONObject = JSONObject().put("isTv", isTv).put("speakers", speakers)
        .put("sizeInch", sizeInch.toDouble()).put("distanceM", distanceM.toDouble()).put("lipSyncMs", lipSyncMs.toDouble())

    companion object {
        fun fromJson(o: JSONObject) = TvSetup(
            isTv = o.optBoolean("isTv", false),
            speakers = o.optInt("speakers", 1).coerceIn(1, 3),
            sizeInch = o.optDouble("sizeInch", 55.0).toFloat().coerceIn(24f, 120f),
            distanceM = o.optDouble("distanceM", 2.5).toFloat().coerceIn(0.8f, 8f),
            lipSyncMs = o.optDouble("lipSyncMs", 0.0).toFloat().coerceIn(0f, 300f),
        )
    }
}

/** The device name looks like a TV ("[TV] Samsung 7 Series", "BRAVIA", "LG webOS TV", "Mi TV"...).
 *  Over Bluetooth Android only says "speaker" or "headphones", so the name is all there is. */
fun looksLikeTv(name: String): Boolean {
    val n = " " + name.lowercase().map { if (it.isLetterOrDigit() || it == '[' || it == ']') it else ' ' }.joinToString("") + " "
    val tokens = listOf("[tv]", " tv ", " television ", " bravia ", " webos ", " smart tv", " android tv", " google tv",
        " fire tv", " roku ", " tcl ", " hisense ", " mi tv", " vu tv", " qled ", " oled tv", " crystal uhd ")
    return tokens.any { n.contains(it) }
}

/** The ear test currently playing: which set, and when it started (elapsedRealtime). */
data class EarTestState(val key: String, val startedMs: Long)

enum class Detected { UNKNOWN, MUSIC, MOVIE }

/** FULL = the laptop's CINEMAX engine (captures and re-plays other apps' audio);
 *  BUILTIN = Android's own effects only. */
enum class EngineMode(val key: String) { FULL("full"), BUILTIN("builtin") }

data class FullEngineState(
    val status: Status = Status.OFF,
    val message: String? = null,
    val stats: CaptureEngine.Stats? = null,
) {
    enum class Status { OFF, NEEDS_CONSENT, STARTING, RUNNING, FAILED }
}

data class AppState(
    val enabled: Boolean = false,
    val params: SoundParams = presetParams(PresetId.FLAT),
    val mode: ListeningMode = ListeningMode.AUTO,
    val musicPreset: PresetId = PresetId.CONCERT_HALL,
    val moviePreset: PresetId = PresetId.CINEMA,
    val detected: Detected = Detected.UNKNOWN,
    val outputKind: OutputKind = OutputKind.SPEAKER,
    val outputName: String = "Phone speaker",
    val engine: AudioEngine.Status = AudioEngine.Status(),
    val startOnBoot: Boolean = true,
    /** Close CINEMAX by itself after this long with nothing playing (paused films get longer). */
    val autoClose: Boolean = true,
    val autoCloseMinutes: Int = 2,
    /** Virtual surround really running on the current output (Android's own check). */
    val surroundActive: Boolean = false,
    /** "Hold to compare" pressed: effects bypassed so the original sound plays. */
    val comparing: Boolean = false,
    val engineMode: EngineMode = EngineMode.FULL,
    val full: FullEngineState = FullEngineState(),
    val smartTone: Boolean = true,
    /** Videos through the CINEMAX engine (about 0.25 s audio delay) or, when false,
     *  through Android's effects with no delay (perfect lip-sync). */
    val videosUseFullEngine: Boolean = true,
    /** True while a video plays and Android's effects are used instead (see above). */
    val videoPassThrough: Boolean = false,
    /** The phone maker's own effects that re-process CINEMAX's output (e.g. Dolby Atmos). */
    val makerEffects: List<String> = emptyList(),
    /** One-time USB setup done (DUMP permission: lets CINEMAX mute the originals). */
    val muteSetupDone: Boolean = false,
    /** Real-ear recordings found in the app (plus the head model, last). */
    val earProfiles: List<EarProfile> = emptyList(),
    /** "auto" (dummy head), "model" or a recording's key. */
    val earProfile: String = "auto",
    val earTest: EarTestState? = null,
    /** Front-camera head tracking (headphones, CINEMAX engine, real ears). */
    val headTracking: Boolean = false,
    val headTrackingFlip: Boolean = false,
    val head: HeadStatus = HeadStatus(),
    /** This output's TV setup (Bluetooth TV, soundbar, HDMI). */
    val tv: TvSetup = TvSetup(),
) {

    /** Headphone 3D sound: a headphone-type output that isn't a TV (some TVs say "headset" over Bluetooth). */
    val headphones get() = outputKind.headphones && !tv.isTv
    /** The real-ear set in use ("model" = head model). */
    val activeEarKey: String get() {
        val measured = earProfiles.filter { it.key != "model" }
        if (earProfile == "model") return "model"
        measured.firstOrNull { it.key == earProfile }?.let { return it.key }
        return measured.firstOrNull { it.key == "ku100" }?.key ?: measured.firstOrNull { it.key == "kemar" }?.key
            ?: measured.firstOrNull()?.key ?: "model"
    }
    /** The output can be a TV (not the phone speaker or wired headphones). */
    val tvPossible get() = outputKind == OutputKind.BLUETOOTH || outputKind == OutputKind.HDMI ||
        outputKind == OutputKind.USB || outputKind == OutputKind.OTHER

    /** The laptop engine is wanted but not running (needs the screen-capture OK). */
    val fullWanted get() = enabled && engineMode == EngineMode.FULL
    val fullRunning get() = full.status == FullEngineState.Status.RUNNING
}

/**
 * In-process controller shared by the UI and the foreground service.
 * All mutations happen on the main thread.
 */
object Cinemax {
    private lateinit var app: Context
    private val prefs by lazy { app.getSharedPreferences("cinemax", Context.MODE_PRIVATE) }
    private val main = Handler(Looper.getMainLooper())

    private val _state = MutableStateFlow(AppState())
    val state: StateFlow<AppState> = _state.asStateFlow()

    /** Live spectrum (0..1 per band) from the phone's real output; empty when unavailable. */
    val spectrum = MutableStateFlow(FloatArray(0))

    private var initialised = false
    private var deviceKey = "speaker"

    fun init(context: Context) {
        if (initialised) return
        initialised = true
        app = context.applicationContext

        _state.value = AppState(
            enabled = prefs.getBoolean("enabled", true),
            mode = ListeningMode.fromKey(prefs.getString("mode", null)) ?: ListeningMode.AUTO,
            musicPreset = PresetId.fromKey(prefs.getString("musicPreset", null)) ?: PresetId.CONCERT_HALL,
            moviePreset = PresetId.fromKey(prefs.getString("moviePreset", null)) ?: PresetId.CINEMA,
            startOnBoot = prefs.getBoolean("startOnBoot", true),
            autoClose = prefs.getBoolean("autoClose", true),
            autoCloseMinutes = prefs.getInt("autoCloseMinutes", 2),
            engineMode = if (prefs.getString("engineMode", if (dumpGranted()) "full" else "builtin") == "builtin") EngineMode.BUILTIN else EngineMode.FULL,
            smartTone = prefs.getBoolean("smartTone", true),
            videosUseFullEngine = prefs.getBoolean("videosFull", true),
            muteSetupDone = app.checkSelfPermission(SessionMuter.DUMP) ==
                android.content.pm.PackageManager.PERMISSION_GRANTED,
            earProfiles = loadEars(),
            earProfile = prefs.getString("earProfile", "auto") ?: "auto",
            headTracking = prefs.getBoolean("headTracking", false),
            headTrackingFlip = prefs.getBoolean("headTrackingFlip", false),
        )

        val am = app.getSystemService(AudioManager::class.java)
        am.registerAudioDeviceCallback(object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(added: Array<out AudioDeviceInfo>) = refreshOutput()
            override fun onAudioDevicesRemoved(removed: Array<out AudioDeviceInfo>) = refreshOutput()
        }, main)
        am.registerAudioPlaybackCallback(object : AudioManager.AudioPlaybackCallback() {
            override fun onPlaybackConfigChanged(configs: MutableList<AudioPlaybackConfiguration>) = onPlayback(configs)
        }, main)

        refreshOutput()
        onPlayback(am.activePlaybackConfigurations)
    }

    // ------------------------------------------------------------------ engine on/off
    fun setEnabled(on: Boolean) {
        prefs.edit().putBoolean("enabled", on).apply()
        _state.update { it.copy(enabled = on) }
        val intent = Intent(app, CinemaxService::class.java)
        if (on) ContextCompat.startForegroundService(app, intent) else app.stopService(intent)
    }

    private fun dumpGranted() = app.checkSelfPermission(SessionMuter.DUMP) == android.content.pm.PackageManager.PERMISSION_GRANTED

    fun reportEngine(status: AudioEngine.Status) = _state.update { it.copy(engine = status) }

    fun reportSurround(active: Boolean) = _state.update { it.copy(surroundActive = active) }

    fun setCompare(on: Boolean) = _state.update { it.copy(comparing = on && it.enabled) }

    fun setEngineMode(mode: EngineMode) {
        prefs.edit().putString("engineMode", mode.key).apply()
        _state.update { it.copy(engineMode = mode) }
        if (mode == EngineMode.BUILTIN && _state.value.enabled)
            app.startService(Intent(app, CinemaxService::class.java).setAction(CinemaxService.ACTION_STOP_FULL))
    }

    fun setVideosUseFullEngine(on: Boolean) {
        prefs.edit().putBoolean("videosFull", on).apply()
        _state.update { it.copy(videosUseFullEngine = on) }
    }

    fun reportVideoPassThrough(on: Boolean) = _state.update { it.copy(videoPassThrough = on) }

    fun reportMakerEffects(list: List<String>) = _state.update { it.copy(makerEffects = list) }

    fun setSmartTone(on: Boolean) {
        prefs.edit().putBoolean("smartTone", on).apply()
        _state.update { it.copy(smartTone = on) }
    }

    fun reportFull(full: FullEngineState) = _state.update { it.copy(full = full) }

    fun refreshSetup() = _state.update {
        it.copy(muteSetupDone = app.checkSelfPermission(SessionMuter.DUMP) ==
            android.content.pm.PackageManager.PERMISSION_GRANTED)
    }

    /** The user approved the screen-capture prompt: hand the token to the service. */
    fun startFullEngine(resultCode: Int, data: Intent) {
        if (!_state.value.enabled) return
        reportFull(FullEngineState(FullEngineState.Status.STARTING))
        ContextCompat.startForegroundService(app, Intent(app, CinemaxService::class.java)
            .setAction(CinemaxService.ACTION_START_FULL)
            .putExtra(CinemaxService.EXTRA_RESULT_CODE, resultCode)
            .putExtra(CinemaxService.EXTRA_DATA, data))
    }

    /** Listeners (the service) that want to know when other apps start or stop playing. */
    var onPlaybackChanged: (() -> Unit)? = null

    // ------------------------------------------------------------------ real ears, ear test, head tracking
    /** Installs the measured sets (assets/hrtf) in the native engine; before any pipeline exists. */
    private fun loadEars(): List<EarProfile> {
        val list = mutableListOf<EarProfile>()
        if (NativeEngine.loaded) runCatching {
            val names = app.assets.list("hrtf").orEmpty().filter { it.endsWith(".chrtf") }.sorted()
            val files = names.map { n -> app.assets.open("hrtf/$n").use { it.readBytes() } }.toTypedArray()
            val info = NativeEngine.nInstallHrtf(files)
            for (i in 0 until info.size / 3) list += EarProfile(info[3 * i], info[3 * i + 1], info[3 * i + 2])
        }.onFailure { android.util.Log.w("Cinemax", "real ears not loaded", it) }
        list += EarProfile("model", "Head model (classic CINEMAX)", "CINEMAX's own head model - no recordings.")
        return list
    }

    fun setEarProfile(key: String) {
        prefs.edit().putString("earProfile", key).apply()
        _state.update { it.copy(earProfile = key) }
    }

    private val earTest by lazy { EarTest(app) }

    /** Plays the test walk (front, right, behind, left, above) with one set. */
    fun playEarTest(key: String) {
        earTest.stop()
        val started = EarTestState(key, android.os.SystemClock.elapsedRealtime())
        _state.update { it.copy(earTest = started) }
        earTest.play(key) { _state.update { s -> if (s.earTest == started) s.copy(earTest = null) else s } }
    }

    fun stopEarTest() {
        earTest.stop()
        _state.update { it.copy(earTest = null) }
    }

    fun setHeadTracking(on: Boolean) {
        prefs.edit().putBoolean("headTracking", on).apply()
        _state.update { it.copy(headTracking = on) }
        // The service may now use the camera: it must say so while CINEMAX is on screen (Android rule).
        if (_state.value.enabled) app.startService(Intent(app, CinemaxService::class.java).setAction(CinemaxService.ACTION_REFRESH))
    }

    fun setHeadTrackingFlip(on: Boolean) {
        prefs.edit().putBoolean("headTrackingFlip", on).apply()
        _state.update { it.copy(headTrackingFlip = on) }
    }

    fun reportHead(s: HeadStatus) = main.post { _state.update { it.copy(head = s) } }

    // ------------------------------------------------------------------ TV output
    fun setTvSetup(transform: (TvSetup) -> TvSetup) {
        val next = transform(_state.value.tv)
        prefs.edit().putString("tv:$deviceKey", next.toJson().toString()).apply()
        _state.update { it.copy(tv = next) }
    }

    private fun tvFor(key: String, kind: OutputKind, name: String): TvSetup =
        prefs.getString("tv:$key", null)?.let { runCatching { TvSetup.fromJson(JSONObject(it)) }.getOrNull() }
            ?: TvSetup(isTv = kind == OutputKind.HDMI || (kind != OutputKind.SPEAKER && kind != OutputKind.WIRED && looksLikeTv(name)))

    // ------------------------------------------------------------------ presets & params
    fun applyPreset(id: PresetId) {
        val s = _state.value
        val mode = when (id) {
            s.musicPreset -> ListeningMode.MUSIC
            s.moviePreset -> ListeningMode.MOVIES
            else -> ListeningMode.MANUAL
        }
        setModeInternal(mode)
        setParamsInternal(loadCustomIf(id))
    }

    /** Any hand edit becomes CUSTOM for this device and stops automatic switching. */
    fun editParams(transform: (SoundParams) -> SoundParams) {
        val cur = _state.value.params
        val base = if (cur.preset != PresetId.CUSTOM) cur.preset else cur.base
        val next = transform(cur).copy(preset = PresetId.CUSTOM, base = base)
        prefs.edit().putString("custom:$deviceKey", next.toJson().toString()).apply()
        if (_state.value.mode != ListeningMode.MANUAL) setModeInternal(ListeningMode.MANUAL)
        setParamsInternal(next)
    }

    fun setMode(mode: ListeningMode) {
        setModeInternal(mode)
        if (mode != ListeningMode.MANUAL) setParamsInternal(presetParams(presetForMode()))
    }

    fun setModePresets(music: PresetId? = null, movie: PresetId? = null) {
        _state.update { it.copy(musicPreset = music ?: it.musicPreset, moviePreset = movie ?: it.moviePreset) }
        prefs.edit().putString("musicPreset", _state.value.musicPreset.key)
            .putString("moviePreset", _state.value.moviePreset.key).apply()
        if (_state.value.mode != ListeningMode.MANUAL) setParamsInternal(presetParams(presetForMode()))
    }

    fun setAutoClose(on: Boolean) {
        prefs.edit().putBoolean("autoClose", on).apply()
        _state.update { it.copy(autoClose = on) }
    }

    fun setAutoCloseMinutes(minutes: Int) {
        prefs.edit().putInt("autoCloseMinutes", minutes).apply()
        _state.update { it.copy(autoCloseMinutes = minutes) }
    }

    /** Last time the user touched the CINEMAX screen (elapsedRealtime): restarts the auto-close countdown. */
    @Volatile var lastInteraction = 0L

    /** The user's own on/off choice (auto-close does not change it: opening CINEMAX starts it again). */
    fun userWantsOn() = prefs.getBoolean("enabled", true)

    /** Fired when CINEMAX closes itself; the screen (if still in recents) finishes too. */
    val closeRequests = kotlinx.coroutines.flow.MutableSharedFlow<Unit>(extraBufferCapacity = 1)

    /** Nothing has played for a while: stop everything (engine, capture, notification) and close. */
    fun autoClose() {
        _state.update { it.copy(enabled = false, comparing = false) }
        app.stopService(Intent(app, CinemaxService::class.java))
        closeRequests.tryEmit(Unit)
    }

    fun setStartOnBoot(on: Boolean) {
        prefs.edit().putBoolean("startOnBoot", on).apply()
        _state.update { it.copy(startOnBoot = on) }
    }

    fun resetDevice() {
        prefs.edit().remove("profile:$deviceKey").remove("custom:$deviceKey").apply()
        setMode(ListeningMode.AUTO)
    }

    private fun loadCustomIf(id: PresetId): SoundParams =
        if (id == PresetId.CUSTOM) prefs.getString("custom:$deviceKey", null)?.let {
            runCatching { SoundParams.fromJson(JSONObject(it)) }.getOrNull()
        } ?: _state.value.params.copy(preset = PresetId.CUSTOM)
        else presetParams(id)

    private fun presetForMode(): PresetId {
        val s = _state.value
        return when (s.mode) {
            ListeningMode.MUSIC -> s.musicPreset
            ListeningMode.MOVIES -> s.moviePreset
            ListeningMode.AUTO -> when (s.detected) {
                Detected.MOVIE -> s.moviePreset
                Detected.MUSIC -> s.musicPreset
                Detected.UNKNOWN -> if (s.params.preset == s.moviePreset) s.moviePreset else s.musicPreset
            }
            ListeningMode.MANUAL -> s.params.preset
        }
    }

    private fun setModeInternal(mode: ListeningMode) {
        prefs.edit().putString("mode", mode.key).apply()
        _state.update { it.copy(mode = mode) }
    }

    private fun setParamsInternal(p: SoundParams) {
        _state.update { it.copy(params = p) }
        prefs.edit().putString("profile:$deviceKey", p.toJson().toString()).apply()
    }

    // ------------------------------------------------------------------ output device
    private fun refreshOutput() {
        val am = app.getSystemService(AudioManager::class.java)
        val media = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build()
        val routed: AudioDeviceInfo? = if (Build.VERSION.SDK_INT >= 33)
            am.getAudioDevicesForAttributes(media).firstOrNull()
        else null
        val dev = routed ?: am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).let { all ->
            all.firstOrNull { isBt(it.type) } ?: all.firstOrNull { isWired(it.type) } ?: all.firstOrNull { isUsb(it.type) }
            ?: all.firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }
        }

        val kind = when {
            dev == null -> OutputKind.SPEAKER
            isBt(dev.type) -> OutputKind.BLUETOOTH
            isWired(dev.type) -> OutputKind.WIRED
            isUsb(dev.type) -> OutputKind.USB
            dev.type == AudioDeviceInfo.TYPE_HDMI || dev.type == AudioDeviceInfo.TYPE_HDMI_ARC ||
                dev.type == AudioDeviceInfo.TYPE_HDMI_EARC -> OutputKind.HDMI
            dev.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> OutputKind.SPEAKER
            else -> OutputKind.OTHER
        }
        val name = when (kind) {
            OutputKind.SPEAKER -> "Phone speaker"
            OutputKind.WIRED -> "Wired headphones"
            else -> dev?.productName?.toString()?.takeIf { it.isNotBlank() && it != Build.MODEL } ?: kind.label
        }
        val key = when (kind) {
            OutputKind.SPEAKER -> "speaker"
            OutputKind.WIRED -> "wired"
            else -> "${kind.name.lowercase()}:$name"
        }

        val changed = key != deviceKey || _state.value.outputName != name
        deviceKey = key
        _state.update { it.copy(outputKind = kind, outputName = name, tv = tvFor(key, kind, name)) }
        if (!changed && _state.value.params.preset != PresetId.FLAT) return

        // Load this device's profile, or follow the active mode (default CINEMA-style sound).
        val s = _state.value
        val stored = prefs.getString("profile:$key", null)?.let { runCatching { SoundParams.fromJson(JSONObject(it)) }.getOrNull() }
        val p = when {
            s.mode != ListeningMode.MANUAL -> presetParams(presetForMode())
            stored != null -> if (stored.preset == PresetId.CUSTOM) stored else presetParams(stored.preset)
            else -> presetParams(presetForMode())
        }
        _state.update { it.copy(params = p) }
    }

    private fun isBt(t: Int) = t == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP || t == AudioDeviceInfo.TYPE_BLE_HEADSET ||
        t == AudioDeviceInfo.TYPE_BLE_SPEAKER || t == AudioDeviceInfo.TYPE_BLUETOOTH_SCO
    private fun isWired(t: Int) = t == AudioDeviceInfo.TYPE_WIRED_HEADPHONES || t == AudioDeviceInfo.TYPE_WIRED_HEADSET
    private fun isUsb(t: Int) = t == AudioDeviceInfo.TYPE_USB_HEADSET || t == AudioDeviceInfo.TYPE_USB_DEVICE

    // ------------------------------------------------------------------ Auto: music vs movie
    /** Apps declare what they play (AudioAttributes content type): YouTube / Netflix / video
     *  players report MOVIE, Spotify / YouTube Music report MUSIC. */
    private fun onPlayback(configs: List<AudioPlaybackConfiguration>) {
        onPlaybackChanged?.invoke()
        var movie = false
        var music = false
        for (c in configs) {
            val a = c.audioAttributes
            if (a.usage != AudioAttributes.USAGE_MEDIA && a.usage != AudioAttributes.USAGE_GAME && a.usage != AudioAttributes.USAGE_UNKNOWN) continue
            when (a.contentType) {
                AudioAttributes.CONTENT_TYPE_MOVIE -> movie = true
                AudioAttributes.CONTENT_TYPE_MUSIC -> music = true
            }
        }
        val next = when {
            movie -> Detected.MOVIE
            music -> Detected.MUSIC
            else -> return   // nothing recognisable playing: keep the last decision
        }
        if (next == _state.value.detected) return
        _state.update { it.copy(detected = next) }
        if (_state.value.mode == ListeningMode.AUTO) setParamsInternal(presetParams(presetForMode()))
    }
}
