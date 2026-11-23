package app.cinemax.mobile.audio

import android.media.audiofx.Visualizer
import android.util.Log
import kotlin.math.hypot
import kotlin.math.log10
import kotlin.math.pow

/**
 * Real spectrum of the phone's output mix (Android Visualizer on session 0).
 * Needs the RECORD_AUDIO permission; only runs while the app is on screen.
 * Produces 32 log-spaced bands (40 Hz - 16 kHz), 0..1.
 */
class Spectrum(private val onFrame: (FloatArray) -> Unit) {
    private var viz: Visualizer? = null
    private val bands = 32
    private val smoothed = FloatArray(bands)

    fun start(): Boolean {
        stop()
        return try {
            val v = Visualizer(0)
            v.captureSize = Visualizer.getCaptureSizeRange()[1].coerceAtMost(1024)
            v.scalingMode = Visualizer.SCALING_MODE_NORMALIZED
            v.setDataCaptureListener(object : Visualizer.OnDataCaptureListener {
                override fun onWaveFormDataCapture(vz: Visualizer, waveform: ByteArray, samplingRate: Int) {}
                override fun onFftDataCapture(vz: Visualizer, fft: ByteArray, samplingRate: Int) = process(fft, samplingRate)
            }, (Visualizer.getMaxCaptureRate() * 3 / 4), false, true)
            v.enabled = true
            viz = v
            true
        } catch (t: Throwable) {
            Log.w("CinemaxSpectrum", "Visualizer unavailable", t)
            false
        }
    }

    fun stop() {
        try { viz?.enabled = false; viz?.release() } catch (_: Throwable) {}
        viz = null
        onFrame(FloatArray(0))
    }

    private fun process(fft: ByteArray, samplingRateMilliHz: Int) {
        val n = fft.size / 2
        if (n < 4) return
        val fs = samplingRateMilliHz / 1000.0
        val binHz = fs / (2.0 * n)
        val out = FloatArray(bands)
        for (b in 0 until bands) {
            val lo = 40.0 * (400.0).pow(b.toDouble() / bands)
            val hi = 40.0 * (400.0).pow((b + 1).toDouble() / bands)
            val k0 = (lo / binHz).toInt().coerceIn(1, n - 1)
            val k1 = (hi / binHz).toInt().coerceIn(k0, n - 1)
            var mag = 0.0
            for (k in k0..k1) {
                val re = fft[2 * k].toDouble()
                val im = fft[2 * k + 1].toDouble()
                mag = maxOf(mag, hypot(re, im))
            }
            val db = 20.0 * log10(mag / 128.0 + 1e-6)        // 0 dB = full scale
            val v = ((db + 60.0) / 60.0).coerceIn(0.0, 1.0).toFloat()
            smoothed[b] = if (v > smoothed[b]) v else smoothed[b] * 0.82f + v * 0.18f
            out[b] = smoothed[b]
        }
        onFrame(out)
    }
}
