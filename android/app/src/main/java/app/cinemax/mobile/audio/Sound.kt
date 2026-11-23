package app.cinemax.mobile.audio

import org.json.JSONArray
import org.json.JSONObject

/** Presets share names and intent with CINEMAX for Windows. */
enum class PresetId(val key: String, val title: String, val tagline: String) {
    CINEMA("cinema", "CINEMA", "Cinematic movie sound: deep controlled bass, clear voices, wide stage."),
    IMAX("imax", "IMAX-STYLE", "Wide, enveloping large-screen style with powerful low end."),
    THEATRE("theatre", "THEATRE", "Balanced theatre-style sound with natural room ambience."),
    CONCERT_HALL("concert-hall", "CONCERT HALL", "Music as if played live in a hall: natural reverb, wide stage."),
    MUSIC("music", "MUSIC", "Balanced musical reproduction with gentle width and full dynamics."),
    DEEP_BASS("deep-bass", "DEEP BASS", "Strong but protected bass."),
    GAMING("gaming", "GAMING", "Clear positional detail and footsteps, minimal room colour."),
    DIALOGUE("dialogue", "DIALOGUE", "Voices forward and intelligible; background reduced."),
    NIGHT("night", "NIGHT MODE", "Controlled dynamic range for quiet late-night listening."),
    DOLBY_STYLE("dolby-style", "DOLBY-STYLE", "Atmos-style 7.1.4: voices on a real centre, sound around and above you, rich bass."),
    CUSTOM("custom", "CUSTOM", "Your own settings for this device."),
    FLAT("neutral", "FLAT", "Neutral reference: only the peak limiter is active.");

    companion object {
        fun fromKey(k: String?) = entries.firstOrNull { it.key == k }
    }
}

enum class ListeningMode(val key: String) { MANUAL("manual"), MUSIC("music"), MOVIES("movies"), AUTO("auto");
    companion object { fun fromKey(k: String?) = entries.firstOrNull { it.key == k } }
}

/** Centre frequencies of the 10 EQ bands (same as the desktop app). */
val EQ_FREQS = listOf(31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000)

/** Upper edges of the bands for Android's DynamicsProcessing EQ (geometric midpoints). */
val EQ_EDGES = floatArrayOf(44f, 88f, 177f, 354f, 707f, 1414f, 2828f, 5657f, 11314f, 20000f)

data class SoundParams(
    val preset: PresetId = PresetId.FLAT,
    val preampDb: Float = 0f,
    val eq: List<Float> = List(10) { 0f },  // dB per band, -12..+12
    val bassDb: Float = 0f,                  // bass boost 0..12
    val subBassDb: Float = 0f,               // -6..+9
    val dialogue: Float = 0f,                // 0..1 voice clarity
    val surround: Float = 0f,                // 0..1 virtual surround strength
    val room: Float = 0f,                    // 0..1 room / hall ambience
    val venue: Float = 0.25f,                // 0..1 theatre .. concert hall
    val compression: Float = 0f,             // 0..1
    val loudness: Boolean = false,           // lift quiet passages (+3 dB before compression)
    val ceilingDb: Float = -1f,              // limiter ceiling
    /** Smart EQ: listens to the music / film and gently evens out its tone (CINEMAX engine only). */
    val ieqOn: Boolean = false,
    val ieqProfile: Int = 0,                 // 0 balanced, 1 warm, 2 detailed
    val ieqAmount: Float = 0.6f,             // 0..1
    /** Atmos-style 7.1.4 up-mix (CINEMAX engine): voices on a real centre, ambience around and above. */
    val immersive: Boolean = false,
    /** Volume-aware bass: adds back the bass / top a lower media volume takes away (both engines). */
    val volumeAware: Boolean = false,
    /** For CUSTOM: the preset these settings were edited from (the full engine keeps
     *  that preset's extra character - height, width, harmonics - under your edits). */
    val base: PresetId = PresetId.FLAT,
) {
    /** Final gain per band (dB) after folding bass, sub-bass and dialogue into the EQ. */
    fun bandGains(): FloatArray {
        val bassW = floatArrayOf(1f, 1f, 0.6f, 0.15f, 0f, 0f, 0f, 0f, 0f, 0f)
        val subW = floatArrayOf(1f, 0.6f, 0.1f, 0f, 0f, 0f, 0f, 0f, 0f, 0f)
        val dlgW = floatArrayOf(0f, 0f, -0.5f, -2.5f, 0f, 1.5f, 3.5f, 4f, 1f, 0f)
        return FloatArray(10) { i ->
            (preampDb + eq[i] + bassDb * bassW[i] + subBassDb * subW[i] + dialogue * dlgW[i]).coerceIn(-15f, 15f)
        }
    }

    /** Venue size -> reverb time, the same mapping as the desktop Venue slider. */
    fun decaySeconds() = 0.45f + 1.65f * venue.coerceIn(0f, 1f)

    fun toJson(): JSONObject = JSONObject()
        .put("preset", preset.key).put("preampDb", preampDb.toDouble())
        .put("eq", JSONArray(eq.map { it.toDouble() }))
        .put("bassDb", bassDb.toDouble()).put("subBassDb", subBassDb.toDouble())
        .put("dialogue", dialogue.toDouble()).put("surround", surround.toDouble())
        .put("room", room.toDouble()).put("venue", venue.toDouble())
        .put("compression", compression.toDouble()).put("loudness", loudness)
        .put("ceilingDb", ceilingDb.toDouble()).put("base", base.key)
        .put("ieqOn", ieqOn).put("ieqProfile", ieqProfile).put("ieqAmount", ieqAmount.toDouble())
        .put("immersive", immersive).put("volumeAware", volumeAware)

    companion object {
        fun fromJson(o: JSONObject): SoundParams {
            val eqArr = o.optJSONArray("eq")
            return SoundParams(
                preset = PresetId.fromKey(o.optString("preset")) ?: PresetId.CUSTOM,
                preampDb = o.optDouble("preampDb", 0.0).toFloat(),
                eq = List(10) { i -> (eqArr?.optDouble(i, 0.0) ?: 0.0).toFloat() },
                bassDb = o.optDouble("bassDb", 0.0).toFloat(),
                subBassDb = o.optDouble("subBassDb", 0.0).toFloat(),
                dialogue = o.optDouble("dialogue", 0.0).toFloat(),
                surround = o.optDouble("surround", 0.0).toFloat(),
                room = o.optDouble("room", 0.0).toFloat(),
                venue = o.optDouble("venue", 0.25).toFloat(),
                compression = o.optDouble("compression", 0.0).toFloat(),
                loudness = o.optBoolean("loudness", false),
                ceilingDb = o.optDouble("ceilingDb", -1.0).toFloat(),
                base = PresetId.fromKey(o.optString("base")) ?: PresetId.FLAT,
                ieqOn = o.optBoolean("ieqOn", false),
                ieqProfile = o.optInt("ieqProfile", 0).coerceIn(0, 2),
                ieqAmount = o.optDouble("ieqAmount", 0.6).toFloat(),
                // settings saved before these existed: DOLBY-STYLE edits get them on
                immersive = o.optBoolean("immersive", o.optString("base") == PresetId.DOLBY_STYLE.key),
                volumeAware = o.optBoolean("volumeAware", o.optString("base") == PresetId.DOLBY_STYLE.key),
            )
        }
    }
}

/**
 * Volume-aware tone (same rule as the laptop's VolumeModeler): our hearing loses bass and
 * some top faster than mid-range as sound gets quieter. Nothing within 6 dB of full volume,
 * then +0.32 dB of bass and +0.09 dB of top per dB, capped at +9 / +3 dB. Returns (bass, top).
 */
fun volumeToneDb(attenuationDb: Float): Pair<Float, Float> {
    val a = (attenuationDb - 6f).coerceAtLeast(0f)
    return (0.32f * a).coerceAtMost(9f) to (0.09f * a).coerceAtMost(3f)
}

/** Media volume step -> approximate attenuation in dB (Android's default headset volume
 *  curve: 1 % -> -58 dB, 33 % -> -27 dB, 66 % -> -13 dB, 100 % -> 0 dB). */
fun mediaVolumeDb(index: Int, max: Int): Float {
    if (index <= 0 || max <= 0) return -96f
    val x = 100f * index / max
    val px = floatArrayOf(1f, 33f, 66f, 100f)
    val py = floatArrayOf(-58f, -27f, -13f, 0f)
    if (x <= px[0]) return py[0]
    for (i in 1 until px.size) if (x <= px[i]) return py[i - 1] + (py[i] - py[i - 1]) * (x - px[i - 1]) / (px[i] - px[i - 1])
    return 0f
}

/** Factory presets (values follow CINEMAX for Windows, Params.cpp). */
fun presetParams(id: PresetId): SoundParams = when (id) {
    PresetId.CINEMA -> SoundParams(id, -1.5f, listOf(2f, 3f, 1.5f, -0.5f, -1f, 0f, 1f, 1.5f, 1.5f, 1f),
        bassDb = 4f, subBassDb = 3f, dialogue = 0.45f, surround = 0.65f, room = 0.25f, venue = 0.3f, compression = 0.35f, loudness = true)
    PresetId.IMAX -> SoundParams(id, -2.5f, listOf(3f, 4f, 2f, 0f, -1f, -0.5f, 0.5f, 2f, 2.5f, 2f),
        bassDb = 6f, subBassDb = 5f, dialogue = 0.35f, surround = 0.9f, room = 0.4f, venue = 0.5f, compression = 0.3f, loudness = true)
    PresetId.THEATRE -> SoundParams(id, -1f, listOf(1f, 1.5f, 1f, 0f, 0f, 0f, 0.5f, 1f, 0.5f, 0f),
        bassDb = 3f, subBassDb = 1.5f, dialogue = 0.4f, surround = 0.55f, room = 0.45f, venue = 0.4f, compression = 0.3f, loudness = true)
    PresetId.CONCERT_HALL -> SoundParams(id, -1f, listOf(0f, 1f, 1f, 0.5f, 0f, 0f, -0.5f, 0f, 0.5f, 1f),
        bassDb = 1.5f, surround = 0.35f, room = 0.4f, venue = 0.65f)
    PresetId.MUSIC -> SoundParams(id, -1f, listOf(1f, 1.5f, 0.5f, 0f, 0f, 0f, 0f, 0.5f, 1f, 1.5f),
        bassDb = 2.5f, subBassDb = 1.5f, surround = 0.3f, room = 0.15f, venue = 0.25f)
    PresetId.DEEP_BASS -> SoundParams(id, -3f, listOf(4f, 5f, 3f, 0f, -1f, -0.5f, 0f, 0.5f, 1f, 0.5f),
        bassDb = 8f, subBassDb = 6f, surround = 0.4f, room = 0.1f, compression = 0.35f, loudness = true)
    PresetId.GAMING -> SoundParams(id, -1.5f, listOf(0f, 1f, 0f, -1f, -1f, 0f, 2f, 3f, 2.5f, 1f),
        bassDb = 2f, subBassDb = 1f, dialogue = 0.3f, surround = 0.8f, room = 0.1f, compression = 0.25f, loudness = true)
    PresetId.DIALOGUE -> SoundParams(id, 0f, listOf(-2f, -1f, 0f, -1.5f, 0f, 1f, 2.5f, 3f, 1f, 0f),
        dialogue = 0.85f, surround = 0.25f, compression = 0.5f, loudness = true)
    PresetId.NIGHT -> SoundParams(id, 0f, listOf(-3f, -2f, -1f, 0f, 0f, 0.5f, 1.5f, 1.5f, 0.5f, 0f),
        subBassDb = -2f, dialogue = 0.6f, surround = 0.35f, compression = 0.85f, loudness = true, ceilingDb = -3f)
    PresetId.DOLBY_STYLE -> SoundParams(id, -1f, listOf(1.5f, 2f, 1f, -0.5f, -1f, 0f, 1f, 1.5f, 1.5f, 1f),
        bassDb = 4f, subBassDb = 3f, dialogue = 0.5f, surround = 1f, room = 0.3f, venue = 0.3f, compression = 0.25f, loudness = true,
        ieqOn = true, ieqProfile = 0, ieqAmount = 0.7f, immersive = true, volumeAware = true)
    PresetId.FLAT, PresetId.CUSTOM -> SoundParams(id)
}
