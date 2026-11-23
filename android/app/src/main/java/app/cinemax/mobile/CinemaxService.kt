package app.cinemax.mobile

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.media.AudioManager
import android.util.Log
import androidx.core.content.ContextCompat
import app.cinemax.mobile.audio.AudioEngine
import app.cinemax.mobile.capture.CaptureEngine
import app.cinemax.mobile.head.HeadStatus
import app.cinemax.mobile.head.HeadTracker
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * Keeps CINEMAX processing alive in the background. Two engines:
 *
 *  - FULL: the laptop's CINEMAX DSP engine. Needs the user's screen-capture OK each
 *    time it starts (Android rule) - Android then lets CINEMAX capture other apps'
 *    audio, which is processed and re-played while the originals are muted.
 *  - BUILTIN: Android's own effects on the global output (no capture). Used when the
 *    user chose it, and as the fallback whenever FULL is not running.
 *
 * Only one runs at a time, so nothing is ever processed twice.
 */
class CinemaxService : Service() {
    private val builtin = AudioEngine()
    private var builtinRunning = false
    private var capture: CaptureEngine? = null
    private var projection: MediaProjection? = null
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val main = Handler(Looper.getMainLooper())
    private var tracker: HeadTracker? = null
    private var lastPlayingMs = 0L
    /** Media volume in dB (for the volume-aware tone), polled every second. */
    @Volatile private var volumeDb = 0f

    override fun onCreate() {
        super.onCreate()
        Cinemax.init(this)
        createChannel()
        goForeground(withProjection = false)
        volumeDb = currentVolumeDb()
        startBuiltin()

        // Parameters / output device / compare -> whichever engine is running.
        scope.launch {
            Cinemax.state
                .map { paramsOf(it) }
                .distinctUntilChanged()
                .collect { applyParams(it); updateNotification() }
        }
        // Built-in engine: keep headphone surround alive. Full engine: refresh stats.
        scope.launch {
            while (isActive) {
                delay(1000)
                // Volume keys: re-apply when the media volume moved (volume-aware tone).
                val v = currentVolumeDb()
                if (kotlin.math.abs(v - volumeDb) >= 0.5f) {
                    volumeDb = v
                    if (Cinemax.state.value.params.volumeAware) applyParams(currentParams())
                }
                if (builtinRunning) {
                    builtin.keepSurround()
                    Cinemax.reportSurround(builtin.surroundActive)
                }
                capture?.stats()?.let { st ->
                    Cinemax.reportFull(Cinemax.state.value.full.copy(stats = st))
                    if (st.playing) lastPlayingMs = SystemClock.elapsedRealtime()
                }
                updateTracker()
            }
        }
        scope.launch {
            Cinemax.state.map { it.headTracking to it.headTrackingFlip }.distinctUntilChanged().collect {
                tracker?.flip = it.second
                updateTracker()
            }
        }
        Cinemax.onPlaybackChanged = { capture?.poke() }
        // Auto-close: nothing playing and the CINEMAX screen not touched for a while -> close.
        scope.launch {
            val am = getSystemService(AudioManager::class.java)
            var idleSince = 0L
            var lastWasMovie = false
            while (isActive) {
                delay(IDLE_CHECK_MS)
                val s = Cinemax.state.value
                val playing = am.isMusicActive   // also true while CINEMAX mutes an app's original sound
                if (playing) lastWasMovie = s.detected == Detected.MOVIE
                if (!s.autoClose || playing || s.comparing) {
                    idleSince = 0L
                    continue
                }
                val now = SystemClock.elapsedRealtime()
                if (idleSince == 0L) idleSince = now
                // Touching the CINEMAX screen (e.g. changing settings) restarts the countdown.
                val idleMs = minOf(now - idleSince, now - Cinemax.lastInteraction)
                // A paused film is a break, not the end of listening.
                val limitMin = if (lastWasMovie) maxOf(s.autoCloseMinutes, MOVIE_GRACE_MIN) else s.autoCloseMinutes
                if (idleMs >= limitMin * 60_000L) {
                    Log.i(TAG, "Nothing played for $limitMin min${if (lastWasMovie) " (after a film)" else ""} - closing CINEMAX")
                    Cinemax.autoClose()
                    break
                }
            }
        }
        // Samsung Dolby Atmos / Adapt Sound / Equaliser re-process CINEMAX's output: tell the user.
        scope.launch(Dispatchers.IO) {
            while (isActive) {
                Cinemax.reportMakerEffects(app.cinemax.mobile.audio.PhoneEffects.detect(this@CinemaxService))
                delay(15_000)
            }
        }
        // Videos: CINEMAX engine, or Android's effects (no delay) if the user prefers lip-sync.
        scope.launch {
            Cinemax.state.map { it.detected == Detected.MOVIE && !it.videosUseFullEngine }
                .distinctUntilChanged()
                .collect { routeEngines() }
        }

        val s = Cinemax.state.value
        if (s.fullWanted && !s.fullRunning) Cinemax.reportFull(FullEngineState(FullEngineState.Status.NEEDS_CONSENT))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_START_FULL -> {
                val code = intent.getIntExtra(EXTRA_RESULT_CODE, Activity.RESULT_CANCELED)
                @Suppress("DEPRECATION")
                val data: Intent? = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(EXTRA_DATA, Intent::class.java)
                                    else intent.getParcelableExtra(EXTRA_DATA)
                if (data != null) startFull(code, data)
            }
            ACTION_STOP_FULL -> stopFull(Cinemax.state.value.fullWanted, null)
            ACTION_REFRESH -> { goForeground(withProjection = projection != null); updateTracker() }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        Cinemax.onPlaybackChanged = null
        scope.cancel()
        tracker?.stop()
        tracker = null
        stopFullEngineOnly()
        builtin.stop()
        builtinRunning = false
        Cinemax.setCompare(false)
        Cinemax.reportSurround(false)
        Cinemax.reportEngine(builtin.status)
        Cinemax.reportFull(FullEngineState())
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    // ------------------------------------------------------------------ engines
    private data class Params(
        val params: app.cinemax.mobile.audio.SoundParams, val headphones: Boolean,
        val comparing: Boolean, val smartTone: Boolean, val earKey: String, val tv: TvSetup,
        val speaker: Boolean, val earTest: Boolean,
    )

    private fun paramsOf(s: AppState) = Params(s.params, s.headphones, s.comparing, s.smartTone, s.activeEarKey,
        s.tv, s.outputKind == OutputKind.SPEAKER, s.earTest != null)

    private fun currentVolumeDb(): Float = runCatching {
        val am = getSystemService(AudioManager::class.java)
        app.cinemax.mobile.audio.mediaVolumeDb(am.getStreamVolume(AudioManager.STREAM_MUSIC), am.getStreamMaxVolume(AudioManager.STREAM_MUSIC))
    }.getOrDefault(0f)

    private fun applyParams(p: Params) {
        capture?.let { c ->
            c.setParams(p.params, p.headphones, p.smartTone, p.comparing, p.earKey, p.tv.toArray(), p.speaker, volumeDb)
            c.silenced = p.earTest      // the ear test plays on its own output
        }
        if (builtinRunning) {
            builtin.apply(p.params, p.headphones, volumeDb)
            builtin.setBypass(p.comparing || p.earTest)   // Android's effects would colour the test
            Cinemax.reportSurround(builtin.surroundActive)
        }
    }

    private fun currentParams() = paramsOf(Cinemax.state.value)

    // ------------------------------------------------------------------ head tracking
    /** The camera runs only while it can help: tracking on, CINEMAX engine processing sound on
     *  headphones with surround and real ears, and something played in the last 15 seconds. */
    private fun updateTracker() {
        val s = Cinemax.state.value
        val camOk = checkSelfPermission(android.Manifest.permission.CAMERA) == android.content.pm.PackageManager.PERMISSION_GRANTED
        val recent = SystemClock.elapsedRealtime() - lastPlayingMs < 15_000
        val reason = when {
            !s.headTracking -> null
            !camOk -> "Allow the camera to use head tracking."
            capture == null || s.videoPassThrough -> "Starts when the CINEMAX engine processes the sound."
            !s.headphones -> "Starts when the sound goes to headphones or earbuds."
            s.params.surround < 0.01f -> "Starts when Surround is on."
            s.activeEarKey == "model" -> "Needs real ears (the head model doesn't follow your head)."
            !recent -> "Waiting for something to play."
            else -> ""
        }
        if (reason == "") {
            if (tracker == null) {
                tracker = HeadTracker(this) { Cinemax.reportHead(it) }.also { it.flip = s.headTrackingFlip; it.start() }
                Log.i(TAG, "head tracking: camera on")
            }
        } else {
            tracker?.let { it.stop(); tracker = null; Log.i(TAG, "head tracking: camera off ($reason)") }
            Cinemax.reportHead(if (reason == null) HeadStatus() else HeadStatus("off", reason))
        }
    }

    private fun startBuiltin() {
        if (builtinRunning) return
        Cinemax.reportEngine(builtin.start())
        builtinRunning = true
        applyParams(currentParams())
    }

    private fun stopBuiltin() {
        if (!builtinRunning) return
        builtin.stop()
        builtinRunning = false
        Cinemax.reportSurround(false)
    }

    private fun startFull(resultCode: Int, data: Intent) {
        stopFullEngineOnly()
        // Android 14+: the service must be a media-projection foreground service
        // before it may use the screen-capture token.
        goForeground(withProjection = true)
        val mp = try {
            getSystemService(MediaProjectionManager::class.java).getMediaProjection(resultCode, data)
        } catch (t: Throwable) {
            Log.w(TAG, "getMediaProjection failed", t); null
        }
        if (mp == null) { stopFull(true, "Screen-capture permission was not granted"); return }
        mp.registerCallback(object : MediaProjection.Callback() {
            override fun onStop() {
                main.post { if (projection === mp) stopFull(Cinemax.state.value.fullWanted, "Android stopped the capture") }
            }
        }, main)
        projection = mp

        // Built-in effects off first, so the replayed audio isn't processed twice.
        stopBuiltin()
        val engine = CaptureEngine(this, mp) { why ->
            main.post { if (capture != null) stopFull(false, why) }
        }
        val err = engine.start()
        if (err != null) {
            Log.w(TAG, "full engine failed: $err")
            stopFull(false, err)
            return
        }
        capture = engine
        applyParams(currentParams())
        routeEngines()
        Cinemax.reportFull(FullEngineState(FullEngineState.Status.RUNNING, stats = engine.stats()))
        updateNotification()
    }

    /** Full engine running: decide whether it processes, or Android's effects do (videos with lip-sync preference). */
    private fun routeEngines() {
        val c = capture ?: return
        val s = Cinemax.state.value
        val pass = s.detected == Detected.MOVIE && !s.videosUseFullEngine
        c.setPassThrough(pass)                 // un-mute originals / pause CINEMAX output first...
        if (pass) startBuiltin() else stopBuiltin()   // ...so nothing is ever processed twice
        Cinemax.reportVideoPassThrough(pass)
        updateNotification()
    }

    /** Stops the full engine; falls back to Android's built-in effects. */
    private fun stopFull(needsConsent: Boolean, message: String?) {
        stopFullEngineOnly()
        goForeground(withProjection = false)
        startBuiltin()
        val status = when {
            message != null && !needsConsent -> FullEngineState.Status.FAILED
            needsConsent -> FullEngineState.Status.NEEDS_CONSENT
            else -> FullEngineState.Status.OFF
        }
        Cinemax.reportFull(FullEngineState(status, message))
        updateNotification()
    }

    private fun stopFullEngineOnly() {
        capture?.stop()     // releases the mutes: originals audible again
        capture = null
        Cinemax.reportVideoPassThrough(false)
        projection?.let { p -> projection = null; runCatching { p.stop() } }
    }

    // ------------------------------------------------------------------ notification
    private fun goForeground(withProjection: Boolean) {
        val n = buildNotification()
        if (Build.VERSION.SDK_INT >= 34) {
            var type = ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
            if (withProjection) type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            // Head tracking uses the camera from the background: Android allows that only for a
            // camera-type service that said so while CINEMAX was on screen.
            val wantCamera = Cinemax.state.value.headTracking &&
                checkSelfPermission(android.Manifest.permission.CAMERA) == android.content.pm.PackageManager.PERMISSION_GRANTED
            if (wantCamera) {
                try {
                    startForeground(NOTIFICATION_ID, n, type or ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
                    return
                } catch (t: Throwable) {
                    Log.w(TAG, "camera service type refused (CINEMAX not on screen?)", t)
                }
            }
            startForeground(NOTIFICATION_ID, n, type)
        } else if (Build.VERSION.SDK_INT >= 29 && withProjection) {
            startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        } else {
            startForeground(NOTIFICATION_ID, n)
        }
    }

    private fun createChannel() {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "CINEMAX sound", NotificationManager.IMPORTANCE_LOW).apply {
            description = "Shown while CINEMAX processes your phone's sound"
            setShowBadge(false)
        })
    }

    private fun buildNotification(): Notification {
        val s = Cinemax.state.value
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val engine = when {
            capture != null && s.videoPassThrough -> "Video: Android effects (no delay)"
            capture != null -> "CINEMAX engine"
            s.fullWanted -> "Basic effects · tap to start the CINEMAX engine"
            else -> "Android effects"
        }
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_cinemax)
            .setContentTitle("CINEMAX · ${s.params.preset.title}")
            .setContentText("$engine · ${s.outputName}")
            .setContentIntent(open)
            .setOngoing(true)
            .setColor(0xFFFF6A1A.toInt())
            .build()
    }

    private fun updateNotification() =
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, buildNotification())

    companion object {
        private const val TAG = "CinemaxService"
        private const val CHANNEL = "cinemax"
        private const val NOTIFICATION_ID = 7
        private const val IDLE_CHECK_MS = 10_000L
        private const val MOVIE_GRACE_MIN = 15
        const val ACTION_START_FULL = "app.cinemax.mobile.START_FULL"
        const val ACTION_STOP_FULL = "app.cinemax.mobile.STOP_FULL"
        const val ACTION_REFRESH = "app.cinemax.mobile.REFRESH"
        const val EXTRA_RESULT_CODE = "resultCode"
        const val EXTRA_DATA = "data"
    }
}

/** Turns CINEMAX back on after a restart if it was on and "Start on boot" is enabled.
 *  (Android effects until the app is opened: the full engine needs the capture OK.) */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        Cinemax.init(context)
        val s = Cinemax.state.value
        if (s.enabled && s.startOnBoot)
            ContextCompat.startForegroundService(context, Intent(context, CinemaxService::class.java))
    }
}
