package app.cinemax.mobile.audio

import android.media.audiofx.AudioEffect
import android.media.audiofx.DynamicsProcessing
import android.media.audiofx.Virtualizer
import android.util.Log
import kotlin.math.log10

/**
 * System-wide sound processing with Android's built-in effect engine, attached to the
 * global output mix (audio session 0), so every app - YouTube, Netflix, Spotify, games -
 * is affected without capturing or re-playing audio (no added delay).
 *
 *  - DynamicsProcessing: 10-band EQ (incl. bass / sub-bass / dialogue shaping),
 *    3-band compressor (low band = bass protection), output limiter
 *  - Virtualizer: virtual surround (strongest on headphones)
 *  - InsertReverb (insert Environmental Reverb): room / concert-hall ambience, decay from the Venue setting
 *
 * Each effect is created independently; [status] reports exactly which ones the phone
 * accepted, so the UI never claims something works when it doesn't.
 */
class AudioEngine {
    data class Status(
        val dynamics: Boolean = false,
        val virtualizer: Boolean = false,
        val reverb: Boolean = false,
        val notes: List<String> = emptyList(),
    ) {
        val anyActive get() = dynamics || virtualizer || reverb
    }

    private var dp: DynamicsProcessing? = null
    private var virt: Virtualizer? = null
    private var reverb: InsertReverb? = null

    var status = Status()
        private set

    /** True while the Virtualizer is really processing on the current output (Android
     *  switches it off by itself on outputs it doesn't support, e.g. the phone speaker). */
    var surroundActive = false
        private set

    // What apply() asked for, so "hold to compare" can restore it exactly.
    private var bypass = false
    private var wantVirt = false
    private var wantReverb = false
    private var wantBinaural = false

    fun start(): Status {
        stop()
        val notes = mutableListOf<String>()

        dp = try {
            val cfg = DynamicsProcessing.Config.Builder(
                DynamicsProcessing.VARIANT_FAVOR_FREQUENCY_RESOLUTION, 2,
                true, EQ_EDGES.size,   // pre-EQ
                true, 3,               // multiband compressor
                false, 0,              // post-EQ
                true                   // limiter
            ).setPreferredFrameDuration(10f).build()
            DynamicsProcessing(PRIORITY, GLOBAL_SESSION, cfg).also {
                it.enabled = !bypass
                if (!it.hasControl()) notes += "Another app controls the equaliser engine"
            }
        } catch (t: Throwable) {
            notes += "Equaliser / dynamics unavailable: ${t.message ?: t.javaClass.simpleName}"
            Log.w(TAG, "DynamicsProcessing failed", t)
            null
        }

        virt = try {
            Virtualizer(PRIORITY, GLOBAL_SESSION).also {
                if (!it.strengthSupported) notes += "Virtual surround strength is fixed on this phone"
            }
        } catch (t: Throwable) {
            notes += "Virtual surround unavailable: ${t.message ?: t.javaClass.simpleName}"
            Log.w(TAG, "Virtualizer failed", t)
            null
        }

        val (rv, rvNote) = InsertReverb.create(PRIORITY, GLOBAL_SESSION)
        reverb = rv
        if (rvNote != null) notes += rvNote

        status = Status(dp != null, virt != null, reverb != null, notes)
        Log.i(TAG, "Engine started: $status")
        return status
    }

    fun stop() {
        for (e in listOf<AudioEffect?>(dp, virt)) {
            try { e?.enabled = false; e?.release() } catch (_: Throwable) {}
        }
        try { reverb?.enabled = false; reverb?.release() } catch (_: Throwable) {}
        dp = null; virt = null; reverb = null
        status = Status()
        surroundActive = false
    }

    /** Applies parameters to the running effects (cheap; safe to call often). */
    fun apply(p: SoundParams, headphones: Boolean, volumeDb: Float = 0f) {
        dp?.let { d ->
            try {
                val gains = p.bandGains()
                if (p.volumeAware) {
                    // Volume-aware bass: a low shelf (~110 Hz) and a high shelf (~9 kHz) folded into the bands.
                    val (bass, top) = volumeToneDb(-volumeDb)
                    val lowW = floatArrayOf(1f, 0.95f, 0.55f, 0.15f, 0f, 0f, 0f, 0f, 0f, 0f)
                    val highW = floatArrayOf(0f, 0f, 0f, 0f, 0f, 0f, 0f, 0.15f, 0.55f, 1f)
                    for (i in gains.indices) gains[i] = (gains[i] + bass * lowW[i] + top * highW[i]).coerceIn(-15f, 15f)
                }
                for (i in EQ_EDGES.indices)
                    d.setPreEqBandAllChannelsTo(i, DynamicsProcessing.EqBand(true, EQ_EDGES[i], gains[i]))

                // Low band (< 150 Hz): bass protection whenever bass is boosted.
                val bassBoosted = p.bassDb > 0.5f || p.subBassDb > 0.5f || gains[0] > 3f || gains[1] > 3f
                val a = p.compression.coerceIn(0f, 1f)
                val thr = -12f - 18f * a
                val ratio = 1f + 5f * a
                val makeup = if (p.loudness) 0f else (-thr) * (1f - 1f / ratio) * 0.45f
                d.setMbcBandAllChannelsTo(0, DynamicsProcessing.MbcBand(true, 150f, 5f, 150f,
                    if (bassBoosted) maxOf(4f, ratio) else ratio, if (bassBoosted) minOf(-8f, thr) else thr,
                    6f, -90f, 1f, 0f, if (a > 0f) makeup else 0f))
                d.setMbcBandAllChannelsTo(1, DynamicsProcessing.MbcBand(true, 5000f, 10f, 180f,
                    ratio, thr, 8f, -90f, 1f, 0f, if (a > 0f) makeup else 0f))
                d.setMbcBandAllChannelsTo(2, DynamicsProcessing.MbcBand(true, 20000f, 5f, 120f,
                    ratio, thr, 8f, -90f, 1f, 0f, if (a > 0f) makeup else 0f))

                d.setInputGainAllChannelsTo(if (p.loudness) 3f else 0f)
                d.setLimiterAllChannelsTo(DynamicsProcessing.Limiter(true, true, 0, 1f, 60f, 10f, p.ceilingDb, 0f))
            } catch (t: Throwable) {
                Log.w(TAG, "DynamicsProcessing apply failed", t)
            }
        }

        virt?.let { v ->
            try {
                val s = (p.surround.coerceIn(0f, 1f) * 1000f).toInt().toShort()
                if (v.strengthSupported) v.setStrength(s)
                // Android's virtualizer only runs on outputs it recognises as headphones and
                // silently disables itself on Bluetooth earbuds that report the generic A2DP
                // type. Ask for binaural (headphone) processing explicitly on headphones.
                // On the phone speaker headphone processing would only colour the sound.
                // Android applies enable requests asynchronously (on its audio thread), so
                // keepSurround() re-asserts the mode once the effect is really running.
                wantVirt = s > 0
                wantBinaural = headphones
                v.enabled = wantVirt && !bypass
                val mode = if (headphones) Virtualizer.VIRTUALIZATION_MODE_BINAURAL else Virtualizer.VIRTUALIZATION_MODE_AUTO
                val forced = runCatching { v.forceVirtualizationMode(mode) }.onFailure { Log.w(TAG, "forceVirtualizationMode failed", it) }.getOrNull()
                Log.i(TAG, "Surround: strength $s, headphones=$headphones, force=$forced")
                keepSurround()
            } catch (t: Throwable) {
                Log.w(TAG, "Virtualizer apply failed", t)
            }
        }

        reverb?.let { r ->
            try {
                val room = p.room.coerceIn(0f, 1f) * (if (headphones) 1f else 0.65f)
                wantReverb = room > 0.01f
                if (!wantReverb) {
                    r.enabled = false
                } else {
                    // Insert reverb adds the reverberation on top of the untouched sound.
                    // Wet level as a share of the dry signal: Room 40 % -> 24 % (about -12 dB).
                    val venue = p.venue.coerceIn(0f, 1f)
                    val wetPct = 8f + 40f * room
                    val wetMb = (2000f * log10(wetPct / 100f)).toInt()
                    if (r.isNxp) {
                        // NXP: wet level = ROOM_LEVEL + REVERB_LEVEL - 2000 mB.
                        r.setShort(InsertReverb.ROOM_LEVEL, 0)
                        r.setShort(InsertReverb.REVERB_LEVEL, (2000 + wetMb).coerceIn(-9000, 2000).toShort())
                    } else {
                        // OpenSL ES meaning: room level = overall level, reverb level relative to it.
                        r.setShort(InsertReverb.ROOM_LEVEL, wetMb.coerceIn(-9000, 0).toShort())
                        r.setShort(InsertReverb.REVERB_LEVEL, 0)
                    }
                    r.setInt(InsertReverb.DECAY_TIME, (p.decaySeconds() * 1000f).toInt().coerceIn(100, 7000))
                    r.setShort(InsertReverb.DECAY_HF_RATIO, (800 - 300 * venue).toInt().toShort())
                    r.setShort(InsertReverb.ROOM_HF_LEVEL, (-500 - 500 * venue).toInt().toShort())
                    r.setShort(InsertReverb.REFLECTIONS_LEVEL, (-1800 + room * 1200).toInt().toShort())
                    r.setInt(InsertReverb.REFLECTIONS_DELAY, (8 + 20 * venue).toInt())
                    r.setInt(InsertReverb.REVERB_DELAY, (12 + 18 * venue).toInt())
                    r.setShort(InsertReverb.DIFFUSION, 1000)
                    r.setShort(InsertReverb.DENSITY, (300 + 700 * venue).toInt().toShort())   // NXP: room size
                    r.enabled = !bypass
                    Log.i(TAG, "Reverb set: decay %.2f s, wet %.0f%% (%d mB), size %.0f%%, %s".format(
                        p.decaySeconds(), wetPct, wetMb, 30 + 70 * venue, r.implementor))
                }
            } catch (t: Throwable) {
                Log.w(TAG, "Reverb apply failed", t)
            }
        }
    }

    /**
     * Called every second by the service. Reads whether Android's virtualizer is really
     * processing and, on headphones, re-requests headphone (binaural) mode if Android
     * switched it off (it does so on Bluetooth earbuds, after routing changes, and the
     * request is ignored while the effect is still starting up).
     */
    fun keepSurround() {
        val v = virt ?: run { surroundActive = false; return }
        if (!wantVirt || bypass) { surroundActive = false; return }
        var mode = runCatching { v.virtualizationMode }.getOrDefault(Virtualizer.VIRTUALIZATION_MODE_OFF)
        if (mode == Virtualizer.VIRTUALIZATION_MODE_OFF && wantBinaural) {
            val ok = runCatching { v.forceVirtualizationMode(Virtualizer.VIRTUALIZATION_MODE_BINAURAL) }.getOrDefault(false)
            mode = runCatching { v.virtualizationMode }.getOrDefault(Virtualizer.VIRTUALIZATION_MODE_OFF)
            if (mode != Virtualizer.VIRTUALIZATION_MODE_OFF) Log.i(TAG, "Surround re-asserted: mode=$mode (force=$ok)")
        }
        val active = mode != Virtualizer.VIRTUALIZATION_MODE_OFF
        if (active != surroundActive) Log.i(TAG, "Surround active=$active (mode=$mode)")
        surroundActive = active
    }

    /** "Hold to compare": switches every effect off (true) or back to the current settings (false). */
    fun setBypass(on: Boolean) {
        bypass = on
        runCatching { dp?.enabled = !on }
        runCatching { virt?.enabled = wantVirt && !on }
        runCatching { reverb?.enabled = wantReverb && !on }
        Log.i(TAG, if (on) "Bypass: original sound" else "Bypass off: CINEMAX sound")
    }

    companion object {
        private const val TAG = "CinemaxEngine"
        private const val PRIORITY = 999
        private const val GLOBAL_SESSION = 0
    }
}
