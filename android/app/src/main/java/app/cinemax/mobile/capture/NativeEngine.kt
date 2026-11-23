package app.cinemax.mobile.capture

import java.nio.ByteBuffer

/** JNI bridge to libcinemax.so: the laptop's DSP chain plus the capture/render pipeline. */
internal object NativeEngine {
    val loaded: Boolean = runCatching { System.loadLibrary("cinemax") }.isSuccess

    @JvmStatic external fun nCreate(captureRate: Int, outputRate: Int): Long
    @JvmStatic external fun nDestroy(h: Long)
    @JvmStatic external fun nPush(h: Long, interleaved: FloatArray, frames: Int)
    @JvmStatic external fun nRender(h: Long, interleaved: FloatArray, frames: Int)
    @JvmStatic external fun nResync(h: Long)
    @JvmStatic external fun nSecondsSinceSignal(h: Long): Double

    /**
     * earKey: "auto", "model" (head model) or a real-ear set key. auditionKey: "" = no ear test,
     * otherwise the set to play the test walk with (auditionId changes per start). tv: isTv,
     * speakers (1 TV / 2 soundbar / 3 receiver), screen size (inch), seat distance (m), lip-sync (ms).
     */
    @JvmStatic external fun nSetParams(
        h: Long, presetKey: String, baseKey: String, custom: FloatArray,
        headphones: Boolean, smartTone: Boolean, bypass: Boolean, speakerHighPassHz: Float,
        earKey: String, auditionKey: String, auditionId: Int, tv: FloatArray,
    )
    @JvmStatic external fun nStats(h: Long, out: FloatArray)

    /** Real ears: installs the measured sets (before any pipeline is created). Returns key, name, info per set. */
    @JvmStatic external fun nInstallHrtf(files: Array<ByteArray>): Array<String>

    /** Front-camera head tracking (libonnxruntime.so + the face model). 0 = failed, see nFaceError. */
    @JvmStatic external fun nFaceCreate(model: ByteArray): Long
    @JvmStatic external fun nFaceError(): String
    @JvmStatic external fun nFaceDestroy(h: Long)
    /** out: face seen in the last second (0/1), yaw, pitch, score, picture reading (-1 = finding it). */
    @JvmStatic external fun nFaceAnalyse(
        h: Long, y: ByteBuffer, u: ByteBuffer, v: ByteBuffer, yRowStride: Int, uvRowStride: Int, uvPixelStride: Int,
        width: Int, height: Int, hintRotation: Int, flip: Boolean, out: FloatArray,
    )

    /** Order of nStats values. */
    const val FILL_MS = 0; const val DRIFT_PPM = 1; const val XRUNS = 2; const val IN_PEAK = 3; const val OUT_PEAK = 4
    const val MOVIE_PROB = 5; const val CONFIDENCE = 6; const val SMART_BASS = 7; const val SMART_TREBLE = 8
    const val CPU = 9; const val SINCE_SIGNAL = 10; const val LIMITER_GR = 11; const val CHUNK_MS = 12; const val DROPPED = 13
    const val STATS = 14

    /** Values for the custom array: preamp, eq[10], bass, sub, dialogue, surround, room, venue,
     *  compression, loudness, ceiling, Smart EQ on, profile, amount. */
    fun customValues(p: app.cinemax.mobile.audio.SoundParams): FloatArray {
        val c = FloatArray(26)
        var i = 0
        c[i++] = p.preampDb
        for (b in 0 until 10) c[i++] = p.eq.getOrElse(b) { 0f }
        c[i++] = p.bassDb; c[i++] = p.subBassDb; c[i++] = p.dialogue
        c[i++] = p.surround; c[i++] = p.room; c[i++] = p.venue
        c[i++] = p.compression; c[i++] = if (p.loudness) 1f else 0f; c[i++] = p.ceilingDb
        c[i++] = if (p.ieqOn) 1f else 0f; c[i++] = p.ieqProfile.toFloat(); c[i++] = p.ieqAmount
        c[i++] = if (p.immersive) 1f else 0f; c[i] = if (p.volumeAware) 1f else 0f
        return c
    }
}
