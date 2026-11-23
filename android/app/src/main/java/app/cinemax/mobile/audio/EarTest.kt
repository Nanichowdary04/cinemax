package app.cinemax.mobile.audio

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.os.Handler
import android.os.Looper
import android.os.Process
import android.util.Log
import app.cinemax.mobile.capture.NativeEngine

/**
 * "Find my ears": plays the engine's test walk - noise bursts in front, right, behind, left
 * and above (0.83 s each) - through one real-ear set, on its own output. It works whether or
 * not the CINEMAX engine is running; the service keeps everything else quiet meanwhile.
 */
class EarTest(private val ctx: Context) {
    private val main = Handler(Looper.getMainLooper())
    @Volatile private var stopRequested = false
    private var worker: Thread? = null
    private var nextId = 1

    val playing get() = worker?.isAlive == true

    /** Plays the walk with 'key' ("model" = head model); onDone runs on the main thread. */
    fun play(key: String, onDone: () -> Unit) {
        stop()
        if (!NativeEngine.loaded) { onDone(); return }
        stopRequested = false
        val id = nextId++
        worker = Thread({
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
            try { run(key, id) } catch (t: Throwable) { Log.w(TAG, "ear test failed", t) }
            main.post(onDone)
        }, "cinemax-eartest").also { it.start() }
    }

    fun stop() {
        stopRequested = true
        worker?.join(1000)
        worker = null
    }

    private fun run(key: String, id: Int) {
        val am = ctx.getSystemService(AudioManager::class.java)
        val rate = am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toIntOrNull() ?: 48000
        val h = NativeEngine.nCreate(48000, rate)
        try {
            // A neutral chain (limiter only) whose output is replaced by the test walk.
            NativeEngine.nSetParams(h, "neutral", "neutral", FloatArray(0), true, false, false, 0f,
                key, key, id, FloatArray(5))
            val attr = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .setAllowedCapturePolicy(AudioAttributes.ALLOW_CAPTURE_BY_NONE)
                .build()
            val fmt = AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                .setSampleRate(rate).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build()
            val min = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_FLOAT)
            val track = AudioTrack.Builder().setAudioAttributes(attr).setAudioFormat(fmt)
                .setTransferMode(AudioTrack.MODE_STREAM).setBufferSizeInBytes(maxOf(min, rate / 10 * 8)).build()
            try {
                val frames = 480
                val buf = FloatArray(frames * 2)
                val total = (WALK_SECONDS * rate).toInt()
                var done = 0
                track.play()
                while (!stopRequested && done < total) {
                    NativeEngine.nRender(h, buf, frames)
                    if (track.write(buf, 0, buf.size, AudioTrack.WRITE_BLOCKING) < 0) break
                    done += frames
                }
                if (!stopRequested) {
                    // let the queued tail play out
                    buf.fill(0f)
                    repeat(rate / 10 / frames + 1) { track.write(buf, 0, buf.size, AudioTrack.WRITE_BLOCKING) }
                }
                track.stop()
            } finally {
                track.release()
            }
        } finally {
            NativeEngine.nDestroy(h)
        }
    }

    companion object {
        private const val TAG = "CinemaxEarTest"
        /** Spatializer::renderAudition: 5 positions x 0.83 s. */
        const val STEP_MS = 830
        const val STEPS = 5
        private const val WALK_SECONDS = STEPS * STEP_MS / 1000.0 + 0.1
    }
}
