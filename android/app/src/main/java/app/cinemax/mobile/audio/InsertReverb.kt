package app.cinemax.mobile.audio

import android.media.audiofx.AudioEffect
import android.util.Log
import java.util.UUID

/**
 * Room / hall reverb as an INSERT effect on the global output.
 *
 * The public EnvironmentalReverb class always picks Android's *auxiliary* reverb, which
 * only hears apps that explicitly send audio to it - on session 0 that means silence.
 * The platform also ships an *insert* implementation of the same reverb; this class finds
 * it via AudioEffect.queryEffects() and creates it by UUID. Those two calls are not part
 * of the public SDK, so everything is guarded: if the phone refuses, [create] returns null
 * and the app reports that room ambience is unavailable.
 */
class InsertReverb private constructor(private val effect: AudioEffect, val implementor: String) {

    /** Android's stock reverb (NXP LVREV) derives its wet level from ROOM_LEVEL + REVERB_LEVEL - 2000 mB. */
    val isNxp get() = implementor.contains("NXP", ignoreCase = true)

    var enabled: Boolean
        get() = effect.enabled
        set(v) { effect.enabled = v }

    fun setShort(param: Int, value: Short) {
        intShort?.let { it.invoke(effect, param, value); return }
        arrays?.let { it.invoke(effect, intArrayOf(param), shortArrayOf(value)); return }
        bytes?.let { it.invoke(effect, le(param, 4), le(value.toInt(), 2)); return }
        error("no usable setParameter")
    }

    fun setInt(param: Int, value: Int) {
        intInt?.let { it.invoke(effect, param, value); return }
        // int32 value sent as two little-endian 16-bit halves (low half first).
        arrays?.let { it.invoke(effect, intArrayOf(param), shortArrayOf((value and 0xFFFF).toShort(), (value ushr 16).toShort())); return }
        bytes?.let { it.invoke(effect, le(param, 4), le(value, 4)); return }
        error("no usable setParameter")
    }

    fun release() = effect.release()

    companion object {
        private const val TAG = "CinemaxReverb"

        // android.media.audiofx.EnvironmentalReverb parameter ids
        const val ROOM_LEVEL = 0
        const val ROOM_HF_LEVEL = 1
        const val DECAY_TIME = 2
        const val DECAY_HF_RATIO = 3
        const val REFLECTIONS_LEVEL = 4
        const val REFLECTIONS_DELAY = 5
        const val REVERB_LEVEL = 6
        const val REVERB_DELAY = 7
        const val DIFFUSION = 8
        const val DENSITY = 9

        /** setParameter overloads this phone exposes (the platform hides some of them). */
        private val setters by lazy {
            AudioEffect::class.java.declaredMethods.filter { it.name == "setParameter" }.onEach {
                runCatching { it.isAccessible = true }
                Log.i(TAG, "available: setParameter(${it.parameterTypes.joinToString { t -> t.simpleName }})")
            }
        }
        private fun find(a: Class<*>?, b: Class<*>?) =
            setters.firstOrNull { it.parameterTypes.size == 2 && it.parameterTypes[0] == a && it.parameterTypes[1] == b }
        private val intInt by lazy { find(Int::class.javaPrimitiveType, Int::class.javaPrimitiveType) }
        private val intShort by lazy { find(Int::class.javaPrimitiveType, Short::class.javaPrimitiveType) }
        private val bytes by lazy { find(ByteArray::class.java, ByteArray::class.java) }
        private val arrays by lazy { find(IntArray::class.java, ShortArray::class.java) }

        /** Little-endian encoding, as the native effect expects. */
        private fun le(v: Int, size: Int) = ByteArray(size) { i -> (v shr (8 * i)).toByte() }

        fun create(priority: Int, session: Int): Pair<InsertReverb?, String?> {
            val desc = try {
                AudioEffect.queryEffects()?.firstOrNull {
                    it.type == AudioEffect.EFFECT_TYPE_ENV_REVERB && it.connectMode == AudioEffect.EFFECT_INSERT
                }
            } catch (t: Throwable) {
                return null to "Room ambience: effect list unavailable (${t.javaClass.simpleName})"
            } ?: return null to "Room ambience: this phone has no insert reverb"

            return try {
                val ctor = AudioEffect::class.java.getConstructor(
                    UUID::class.java, UUID::class.java, Int::class.javaPrimitiveType, Int::class.javaPrimitiveType)
                val fx = ctor.newInstance(desc.type, desc.uuid, priority, session)
                Log.i(TAG, "Insert reverb created: ${desc.name} (${desc.implementor})")
                InsertReverb(fx, desc.implementor ?: "") to null
            } catch (t: Throwable) {
                Log.w(TAG, "Insert reverb refused", t)
                null to "Room ambience: refused by the phone (${(t.cause ?: t).javaClass.simpleName})"
            }
        }
    }
}
