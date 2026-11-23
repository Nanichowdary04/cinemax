package app.cinemax.mobile.capture

import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.media.audiofx.DynamicsProcessing
import android.os.Process
import android.os.SystemClock
import android.util.Log
import java.util.concurrent.TimeUnit

/**
 * Silences the ORIGINAL sound of apps whose audio CINEMAX is capturing and replaying
 * processed, so you only hear the CINEMAX version.
 *
 * How: Android lets an app attach an audio effect to another app's audio session. A
 * DynamicsProcessing effect with -120 dB input gain on that session mutes what the
 * speaker / headphones receive from it, while Android's internal capture still gets
 * the untouched audio (capture happens before effects).
 *
 * Safety:
 *  - Only apps that ALLOW capture are muted (attribute flags + the app's manifest).
 *    Apps that block capture (e.g. Spotify, Netflix) keep playing normally.
 *  - If a muted app plays but the capture stays silent for a few seconds, its mute is
 *    removed at once and the app is left alone for the rest of the session.
 *  - Effects belong to the CINEMAX process: if CINEMAX stops or crashes, Android
 *    removes them and every app is heard normally again.
 *
 * Finding other apps' audio sessions needs the DUMP permission (granted once over
 * USB: adb shell pm grant app.cinemax.mobile android.permission.DUMP).
 */
class SessionMuter(private val ctx: Context) {
    data class Player(
        val piid: Int, val uid: Int, val state: String, val usage: String,
        val content: String, val flags: Int, val session: Int,
    )

    private class Mute(val effect: DynamicsProcessing, val uid: Int) {
        var startedSince = 0L   // elapsedRealtime when the player was last seen "started", 0 = not playing
    }

    private val muted = HashMap<Int, Mute>()                // session -> effect
    private val capturePolicy = HashMap<Int, Boolean>()      // uid -> manifest allows capture
    private val blockedUids = HashSet<Int>()                 // heard nothing in capture: never mute again
    private val labels = HashMap<Int, String>()

    /** Packages currently muted (processed by CINEMAX) and those left alone. */
    @Volatile var processedApps: List<String> = emptyList(); private set
    @Volatile var unprocessedApps: List<String> = emptyList(); private set

    fun hasPermission() = ctx.checkSelfPermission(DUMP) == PackageManager.PERMISSION_GRANTED

    /**
     * One update pass (background thread). [secondsSinceSignal] = time since the capture
     * last contained sound.
     */
    @Synchronized fun update(secondsSinceSignal: Double) {
        val players = scan() ?: return
        val now = SystemClock.elapsedRealtime()
        val processed = LinkedHashSet<String>()
        val unprocessed = LinkedHashSet<String>()

        // A "blocked" app turns out to be heard after all (it is the only thing playing and
        // the capture has sound): it was a false alarm, process it again.
        val startedOthers = players.filter { it.state == "started" && it.uid != myUid && it.usage in MEDIA_USAGES }
        val solo = startedOthers.map { it.uid }.distinct().singleOrNull()
        if (solo != null && solo in blockedUids && secondsSinceSignal < 0.5 &&
            startedOthers.all { it.flags and (FLAG_NO_MEDIA_PROJECTION or FLAG_NO_SYSTEM_CAPTURE) == 0 }) {
            Log.i(TAG, "uid $solo is captured after all: processing it again")
            blockedUids -= solo
        }

        val wanted = HashMap<Int, Player>()
        for (p in players) {
            if (p.session <= 0 || p.uid == myUid || p.state == "released") continue
            if (p.usage !in MEDIA_USAGES) continue
            if (p.uid in blockedUids || !capturable(p)) {
                if (p.state == "started") unprocessed += label(p.uid)
                continue
            }
            wanted[p.session] = p
        }

        // Release mutes of sessions that disappeared.
        val gone = muted.keys.filter { it !in wanted }
        for (s in gone) release(s, "session ended")

        for ((session, p) in wanted) {
            val m = muted[session] ?: mute(session, p.uid)?.also { muted[session] = it } ?: continue
            if (p.state == "started") {
                processed += label(p.uid)
                if (m.startedSince == 0L) m.startedSince = now
                // Safety: playing for 4 s but the capture heard nothing -> this app's audio
                // is not reaching CINEMAX. Un-mute it immediately and leave it alone.
                val playingFor = (now - m.startedSince) / 1000.0
                if (playingFor > 4.0 && secondsSinceSignal > 4.0) {
                    Log.w(TAG, "uid ${p.uid} plays but capture is silent: un-muting and leaving it unprocessed")
                    blockedUids += p.uid
                    muted.entries.filter { it.value.uid == p.uid }.map { it.key }.forEach { release(it, "not captured") }
                    processed -= label(p.uid)
                    unprocessed += label(p.uid)
                }
            } else {
                m.startedSince = 0L
            }
        }
        processedApps = processed.toList()
        unprocessedApps = unprocessed.toList()
    }

    @Synchronized fun releaseAll() {
        for (s in muted.keys.toList()) release(s, "engine stopped")
        processedApps = emptyList()
        unprocessedApps = emptyList()
    }

    // ------------------------------------------------------------------ muting
    private fun mute(session: Int, uid: Int): Mute? = try {
        val cfg = DynamicsProcessing.Config.Builder(
            DynamicsProcessing.VARIANT_FAVOR_FREQUENCY_RESOLUTION, 2,   // the only variant Android implements
            false, 1, false, 1, false, 1, false,   // no stages: only the input gain is used
        ).build()
        val dp = DynamicsProcessing(MUTE_PRIORITY, session, cfg)
        dp.setInputGainAllChannelsTo(MUTE_DB)
        dp.enabled = true
        if (!dp.hasControl()) Log.w(TAG, "session $session: another app controls its effects")
        Log.i(TAG, "muted original: session $session (uid $uid, ${label(uid)})")
        Mute(dp, uid)
    } catch (t: Throwable) {
        Log.w(TAG, "could not mute session $session", t)
        null
    }

    private fun release(session: Int, why: String) {
        val m = muted.remove(session) ?: return
        runCatching { m.effect.enabled = false; m.effect.release() }
        Log.i(TAG, "un-muted session $session ($why)")
    }

    // ------------------------------------------------------------------ capture policy
    private fun capturable(p: Player): Boolean {
        if (p.flags and (FLAG_NO_MEDIA_PROJECTION or FLAG_NO_SYSTEM_CAPTURE) != 0) return false
        if (p.uid < Process.FIRST_APPLICATION_UID && p.uid != SHELL_UID) return false
        return capturePolicy.getOrPut(p.uid) { manifestAllowsCapture(p.uid) }
    }

    /**
     * An app's manifest can forbid capture (android:allowAudioPlaybackCapture="false", and
     * apps targeting Android 9 or older default to "forbid"). Android records that as a
     * private ApplicationInfo flag. If the flag can't be read, the app is treated as
     * capturable and the runtime check in [update] (silent capture -> un-mute) protects it.
     */
    private fun manifestAllowsCapture(uid: Int): Boolean {
        val pkgs = ctx.packageManager.getPackagesForUid(uid) ?: return false
        return pkgs.all { pkg ->
            val ai = runCatching { ctx.packageManager.getApplicationInfo(pkg, 0) }.getOrNull() ?: return@all false
            val field = privateFlagsField
            val bit = allowCaptureBit
            val flags = field?.getInt(ai)
            Log.i(TAG, "$pkg privateFlags=0x${flags?.let { Integer.toHexString(it) }}")
            if (flags == null || bit == null) true else (flags and bit) != 0
        }.also { Log.i(TAG, "uid $uid (${pkgs.joinToString()}): capture ${if (it) "allowed" else "blocked by the app"}") }
    }

    private val privateFlagsField by lazy {
        runCatching { ApplicationInfo::class.java.getField("privateFlags") }
            .onFailure { Log.w(TAG, "privateFlags not readable; relying on the runtime check") }.getOrNull()
    }
    private val allowCaptureBit by lazy {
        runCatching { ApplicationInfo::class.java.getField("PRIVATE_FLAG_ALLOW_AUDIO_PLAYBACK_CAPTURE").getInt(null) }
            .getOrElse { PRIVATE_FLAG_ALLOW_AUDIO_PLAYBACK_CAPTURE }
    }

    private fun label(uid: Int): String = labels.getOrPut(uid) {
        val pm = ctx.packageManager
        val pkg = pm.getPackagesForUid(uid)?.firstOrNull() ?: return@getOrPut "uid $uid"
        runCatching { pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString() }.getOrDefault(pkg)
    }

    // ------------------------------------------------------------------ discovery
    /** Active players from `dumpsys audio` (needs DUMP). Null when unavailable. */
    fun scan(): List<Player>? {
        if (!hasPermission()) return null
        val text = run("dumpsys", "audio") ?: return null
        val out = LinkedHashMap<Int, Player>()
        for (line in text.lineSequence()) {
            val i = line.indexOf("AudioPlaybackConfiguration piid:")
            if (i < 0) continue
            val m = PLAYER.find(line, i) ?: continue
            val (piid, uid, state, usage, content, flags, session) = m.destructured
            out[piid.toInt()] = Player(piid.toInt(), uid.toInt(), state, usage, content, flags.toLong(16).toInt(), session.toInt())
        }
        return out.values.toList()
    }

    private fun run(vararg cmd: String): String? = try {
        val p = ProcessBuilder(*cmd).redirectErrorStream(true).start()
        val text = p.inputStream.bufferedReader().readText()
        if (!p.waitFor(3, TimeUnit.SECONDS)) p.destroy()
        text
    } catch (t: Throwable) {
        Log.w(TAG, "${cmd.joinToString(" ")} failed", t)
        null
    }

    companion object {
        private const val TAG = "CinemaxMuter"
        private const val SHELL_UID = 2000   // adb test signals
        /** ApplicationInfo.PRIVATE_FLAG_ALLOW_AUDIO_PLAYBACK_CAPTURE (hidden constant). */
        private const val PRIVATE_FLAG_ALLOW_AUDIO_PLAYBACK_CAPTURE = 1 shl 27
        const val DUMP = "android.permission.DUMP"
        private const val MUTE_PRIORITY = 1_000_000
        private const val MUTE_DB = -120f
        private const val FLAG_NO_MEDIA_PROJECTION = 1 shl 10
        private const val FLAG_NO_SYSTEM_CAPTURE = 1 shl 12
        private val MEDIA_USAGES = setOf("USAGE_MEDIA", "USAGE_GAME", "USAGE_UNKNOWN")
        private val myUid = Process.myUid()
        private val PLAYER = Regex(
            """AudioPlaybackConfiguration piid:(\d+) .*?u/pid:(-?\d+)/-?\d+ state:(\w+) attr:AudioAttributes: usage=(\w+) content=(\w+) flags=0x([0-9A-Fa-f]+).*?sessionId:(-?\d+)"""
        )
    }
}
