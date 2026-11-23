package app.cinemax.mobile.capture

import android.annotation.SuppressLint
import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioPlaybackCaptureConfiguration
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.projection.MediaProjection
import android.os.Process
import android.util.Log
import app.cinemax.mobile.audio.PresetId
import app.cinemax.mobile.audio.SoundParams

/**
 * The full CINEMAX engine on the phone - the same DSP code as the Windows app:
 *
 *   other apps' audio --(Android internal capture)--> CINEMAX DSP --> AudioTrack
 *
 * while [SessionMuter] silences those apps' original output. Three threads:
 * capture (AudioRecord -> native FIFO), render (native DSP -> AudioTrack, paced by
 * the output device) and the muter (finds and mutes other apps' sessions).
 */
class CaptureEngine(
    private val ctx: Context,
    private val projection: MediaProjection,
    /** Called (from a worker thread) when capture or output fails; the originals are
     *  already un-muted by then. */
    private val onFailure: (String) -> Unit,
) {
    data class Stats(
        val addedDelayMs: Int, val cpuPercent: Float, val xruns: Int,
        val smartBassDb: Float, val smartTrebleDb: Float, val movieProbability: Float, val confidence: Float,
        val playing: Boolean, val processedApps: List<String>, val unprocessedApps: List<String>,
        val muterReady: Boolean,
    )

    private val muter = SessionMuter(ctx)
    private var handle = 0L
    private var record: AudioRecord? = null
    private var track: AudioTrack? = null
    private var outRate = 48000
    private var trackLatencyMs = 0
    @Volatile private var running = false
    /** Video lip-sync mode: originals un-muted, CINEMAX output paused (Android's effects take over). */
    @Volatile private var passThrough = false
    private val threads = ArrayList<Thread>()
    private val statBuf = FloatArray(NativeEngine.STATS)
    private val wake = Object()

    /** Starts capturing; returns an error message, or null when running. */
    @SuppressLint("MissingPermission")
    fun start(): String? {
        if (!NativeEngine.loaded) return "CINEMAX engine library is missing from this build"
        val am = ctx.getSystemService(AudioManager::class.java)
        outRate = am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toIntOrNull() ?: 48000
        val burst = (am.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER)?.toIntOrNull() ?: 240).coerceIn(64, 960)

        val capCfg = AudioPlaybackCaptureConfiguration.Builder(projection)
            .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
            .addMatchingUsage(AudioAttributes.USAGE_GAME)
            .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
            .excludeUid(Process.myUid())
            .build()
        val inFmt = AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
            .setSampleRate(CAPTURE_RATE).setChannelMask(AudioFormat.CHANNEL_IN_STEREO).build()
        val minRec = AudioRecord.getMinBufferSize(CAPTURE_RATE, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_FLOAT)
        val rec = try {
            AudioRecord.Builder().setAudioFormat(inFmt).setAudioPlaybackCaptureConfig(capCfg)
                .setBufferSizeInBytes(maxOf(minRec * 2, CAPTURE_RATE / 5 * 8)).build()
        } catch (t: Throwable) {
            return "Android refused audio capture: ${t.message ?: t.javaClass.simpleName}"
        }
        if (rec.state != AudioRecord.STATE_INITIALIZED) { rec.release(); return "Android refused audio capture" }

        val outAttr = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_UNKNOWN)
            .setAllowedCapturePolicy(AudioAttributes.ALLOW_CAPTURE_BY_NONE)   // never capture our own output
            .build()
        val outFmt = AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
            .setSampleRate(outRate).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build()
        val minTrack = AudioTrack.getMinBufferSize(outRate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_FLOAT)
        val trk = try {
            AudioTrack.Builder().setAudioAttributes(outAttr).setAudioFormat(outFmt)
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .setBufferSizeInBytes(maxOf(minTrack, burst * 8 * 2)).build()
        } catch (t: Throwable) {
            rec.release()
            return "Could not open the audio output: ${t.message ?: t.javaClass.simpleName}"
        }
        // Keep the output queue short: every frame queued here is added delay. It grows
        // (one burst at a time) only if the phone actually starts to stutter.
        runCatching { trk.bufferSizeInFrames = burst * 3 }
        trackLatencyMs = trk.bufferSizeInFrames * 1000 / outRate

        record = rec
        track = trk
        handle = NativeEngine.nCreate(CAPTURE_RATE, outRate)
        running = true

        threads += thread("cinemax-capture", Process.THREAD_PRIORITY_URGENT_AUDIO) { captureLoop(rec) }
        threads += thread("cinemax-render", Process.THREAD_PRIORITY_URGENT_AUDIO) { renderLoop(trk, burst) }
        threads += thread("cinemax-muter", Process.THREAD_PRIORITY_DEFAULT) { muterLoop() }
        Log.i(TAG, "started: capture $CAPTURE_RATE Hz, output $outRate Hz, burst $burst, track buffer ${trk.bufferSizeInFrames} frames")
        return null
    }

    fun stop() {
        if (!running && handle == 0L) return
        running = false
        synchronized(wake) { wake.notifyAll() }
        runCatching { record?.stop() }
        runCatching { track?.pause(); track?.flush() }
        for (t in threads) runCatching { t.join(1500) }
        threads.clear()
        muter.releaseAll()   // originals audible again before anything else
        runCatching { record?.release() }
        runCatching { track?.release() }
        record = null; track = null
        if (handle != 0L) { NativeEngine.nDestroy(handle); handle = 0L }
        Log.i(TAG, "stopped")
    }

    fun setPassThrough(on: Boolean) {
        if (passThrough == on) return
        passThrough = on
        if (on) muter.releaseAll()
        poke()
        Log.i(TAG, if (on) "video: originals un-muted, Android effects take over (no delay)" else "CINEMAX engine processing again")
    }

    /** Re-scan other apps' audio now (a player started, paused or stopped). */
    fun poke() = synchronized(wake) { wake.notifyAll() }

    /** The ear test plays on its own: CINEMAX's own output stays silent meanwhile. */
    @Volatile var silenced = false

    fun setParams(p: SoundParams, headphones: Boolean, smartTone: Boolean, bypass: Boolean, earKey: String, tv: FloatArray, speaker: Boolean,
                  volumeDb: Float = 0f) {
        val h = handle
        if (h == 0L) return
        val base = if (p.preset == PresetId.CUSTOM) p.base else p.preset
        NativeEngine.nSetParams(h, p.preset.key, base.key, NativeEngine.customValues(p), headphones, smartTone, bypass,
            if (speaker) PHONE_SPEAKER_HIGHPASS_HZ else 0f, earKey, "", 0, tv + volumeDb)   // [5] = media volume (dB)
    }

    fun stats(): Stats? {
        val h = handle
        if (h == 0L) return null
        NativeEngine.nStats(h, statBuf)
        val delay = statBuf[NativeEngine.FILL_MS] + statBuf[NativeEngine.CHUNK_MS] + trackLatencyMs + CAPTURE_PATH_MS
        return Stats(
            addedDelayMs = delay.toInt(), cpuPercent = statBuf[NativeEngine.CPU], xruns = statBuf[NativeEngine.XRUNS].toInt(),
            smartBassDb = statBuf[NativeEngine.SMART_BASS], smartTrebleDb = statBuf[NativeEngine.SMART_TREBLE],
            movieProbability = statBuf[NativeEngine.MOVIE_PROB], confidence = statBuf[NativeEngine.CONFIDENCE],
            playing = statBuf[NativeEngine.SINCE_SIGNAL] < 1f,
            processedApps = muter.processedApps, unprocessedApps = muter.unprocessedApps,
            muterReady = muter.hasPermission(),
        )
    }

    // ------------------------------------------------------------------ threads
    private fun captureLoop(rec: AudioRecord) {
        val buf = FloatArray(CHUNK_FRAMES * 2)
        rec.startRecording()
        while (running) {
            val n = rec.read(buf, 0, buf.size, AudioRecord.READ_BLOCKING)
            if (n > 0) NativeEngine.nPush(handle, buf, n / 2)
            else if (n < 0 && running) { fail("Android stopped delivering audio (error $n)"); return }
        }
    }

    private var underruns = 0

    private fun renderLoop(trk: AudioTrack, burst: Int) {
        val frames = maxOf(burst, 128)
        val buf = FloatArray(frames * 2)
        var playing = false
        while (running) {
            val since = if (passThrough) 1.0e9 else NativeEngine.nSecondsSinceSignal(handle)
            if (!playing) {
                if (since < 0.2) {
                    NativeEngine.nResync(handle)
                    trk.play()
                    playing = true
                } else {
                    Thread.sleep(10)
                    continue
                }
            } else if (since > IDLE_PAUSE_S) {
                // Nothing playing: stop the output so the phone can sleep.
                trk.pause(); trk.flush()
                playing = false
                continue
            }
            NativeEngine.nRender(handle, buf, frames)
            if (silenced) buf.fill(0f)
            val w = trk.write(buf, 0, buf.size, AudioTrack.WRITE_BLOCKING)
            val u = trk.underrunCount
            if (u > underruns) {
                underruns = u
                val grown = minOf(trk.bufferSizeInFrames + burst, trk.bufferCapacityInFrames, outRate / 20)
                if (grown > trk.bufferSizeInFrames) {
                    trk.bufferSizeInFrames = grown
                    trackLatencyMs = trk.bufferSizeInFrames * 1000 / outRate
                    Log.i(TAG, "output stuttered: buffer now ${trk.bufferSizeInFrames} frames")
                }
            }
            if (w < 0 && running) { fail("The audio output stopped working (error $w)"); return }
        }
    }

    private var pass = 0

    private fun muterLoop() {
        if (!muter.hasPermission()) Log.w(TAG, "DUMP not granted: originals cannot be muted")
        while (running) {
            if (!passThrough) runCatching { muter.update(NativeEngine.nSecondsSinceSignal(handle)) }
                .onFailure { Log.w(TAG, "muter update failed", it) }
            if (!running) { muter.releaseAll(); break }   // a failure raced with this pass
            if (++pass % 4 == 0) stats()?.let {
                Log.i(TAG, "stats: delay ~${it.addedDelayMs} ms (fifo %.1f ms, chunk %.1f ms, track $trackLatencyMs ms), cpu %.1f%%, xruns ${it.xruns}, drift %.0f ppm, playing=${it.playing}, processing=${it.processedApps}, blocked=${it.unprocessedApps}".format(
                    statBuf[NativeEngine.FILL_MS], statBuf[NativeEngine.CHUNK_MS], it.cpuPercent, statBuf[NativeEngine.DRIFT_PPM]))
            }
            synchronized(wake) { if (running) wake.wait(MUTER_PERIOD_MS) }
        }
    }

    /** Never leave other apps muted while CINEMAX can't play: un-mute first, then report. */
    private fun fail(why: String) {
        Log.e(TAG, why)
        running = false
        muter.releaseAll()
        onFailure(why)
    }

    private fun thread(name: String, priority: Int, body: () -> Unit) = Thread({
        Process.setThreadPriority(priority)
        try { body() } catch (t: Throwable) { if (running) fail("$name stopped: ${t.message ?: t.javaClass.simpleName}") }
    }, name).also { it.start() }

    companion object {
        private const val TAG = "CinemaxCapture"
        private const val CAPTURE_RATE = 48000
        private const val CHUNK_FRAMES = 480          // 10 ms
        private const val IDLE_PAUSE_S = 3.0
        private const val MUTER_PERIOD_MS = 1500L
        // Android's own capture path (tee -> internal submix pipe -> record thread), which
        // CINEMAX can't see. Measured acoustically on a Galaxy SM-E156B (Android 16): the
        // processed sound arrived 0.27-0.33 s after the original's presentation time.
        private const val CAPTURE_PATH_MS = 175
        private const val PHONE_SPEAKER_HIGHPASS_HZ = 180f
    }
}
