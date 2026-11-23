package app.cinemax.mobile.audio

import android.content.Context
import android.content.pm.PackageManager
import android.util.Log
import java.util.concurrent.TimeUnit

/**
 * Detects the phone maker's own sound processing that runs AFTER CINEMAX on the output
 * (Samsung Dolby Atmos, Adapt Sound, Samsung Equaliser). Those re-shape CINEMAX's sound
 * (Dolby adds its own 3D virtualiser, leveller and automatic EQ), which is why a phone
 * can sound different from the laptop with the same preset.
 *
 * Read-only: reads `dumpsys media.audio_flinger` (needs the DUMP permission granted for
 * the full engine). CINEMAX never changes these settings itself - it only tells the user.
 */
object PhoneEffects {
    private const val TAG = "CinemaxPhoneFx"

    /** Names of active maker effects, e.g. ["Dolby Atmos", "Adapt Sound"]; empty if none or unknown. */
    fun detect(ctx: Context): List<String> {
        if (ctx.checkSelfPermission("android.permission.DUMP") != PackageManager.PERMISSION_GRANTED) return emptyList()
        val text = try {
            val p = ProcessBuilder("dumpsys", "media.audio_flinger").redirectErrorStream(true).start()
            val t = p.inputStream.bufferedReader().readText()
            if (!p.waitFor(3, TimeUnit.SECONDS)) p.destroy()
            t
        } catch (t: Throwable) {
            Log.w(TAG, "audio_flinger dump failed", t)
            return emptyList()
        }
        val found = LinkedHashSet<String>()
        for (line in text.lineSequence()) {
            val l = line.trim()
            // Samsung Dolby Atmos stage: "D-Action| EN: 1, Profile: 0, ..."
            if (l.startsWith("D-Action|") && l.contains("EN: 1")) found += "Dolby Atmos"
            // SoundAlive: "SA-Action| Adapt: 1, Gain: ..." and "SA-Action| Preset: 3, BAND: 9, EQ: ..."
            if (l.startsWith("SA-Action|") && l.contains("Adapt: 1")) found += "Adapt Sound"
            if (l.startsWith("SA-Action|") && l.contains("EQ:")) {
                val eq = l.substringAfter("EQ:").substringBefore(",").trim().split(Regex("\\s+")).mapNotNull { it.toIntOrNull() }
                val preset = Regex("Preset: (-?\\d+)").find(l)?.groupValues?.get(1)?.toIntOrNull() ?: -1
                if (eq.any { it != 0 } || preset > 0) found += "Samsung Equaliser"
            }
        }
        return found.toList()
    }
}
