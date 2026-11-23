package app.cinemax.mobile

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.media.projection.MediaProjectionConfig
import android.media.projection.MediaProjectionManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.MotionEvent
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import app.cinemax.mobile.audio.Spectrum
import app.cinemax.mobile.ui.CinemaxApp

class MainActivity : ComponentActivity() {
    private val spectrum = Spectrum { Cinemax.spectrum.value = it }

    private val askAudio = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) spectrum.start()
    }
    private val askNotifications = registerForActivityResult(ActivityResultContracts.RequestPermission()) { }
    /** Head tracking: the front camera (asked only when the user switches it on). */
    private val askCamera = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) Cinemax.setHeadTracking(true)
    }

    /** Android's "start recording or casting" prompt; CINEMAX only uses the audio. */
    private val askCapture = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { r ->
        val data = r.data
        if (r.resultCode == RESULT_OK && data != null) Cinemax.startFullEngine(r.resultCode, data)
        else Cinemax.reportFull(FullEngineState(FullEngineState.Status.NEEDS_CONSENT, "You declined the capture prompt"))
    }
    private val askAudioForEngine = registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        if (granted) launchCapturePrompt()
        else Cinemax.reportFull(FullEngineState(FullEngineState.Status.NEEDS_CONSENT, "CINEMAX needs the audio permission to hear other apps"))
    }
    private var promptedThisVisit = false
    /** The opening screen shows when the screen is first created: nothing is asked before it ends. */
    private var introPending = false
    /** CINEMAX's own "Start the engine?" message, shown once the app is fully open. Android's
     *  screen-share prompt only comes after the user taps Continue. */
    private val askEngine = androidx.compose.runtime.mutableStateOf(false)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        introPending = savedInstanceState == null
        enableEdgeToEdge()
        Cinemax.init(this)

        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            askNotifications.launch(Manifest.permission.POST_NOTIFICATIONS)

        // CINEMAX is on by default: opening the app starts processing (also after it closed itself).
        if (Cinemax.userWantsOn()) Cinemax.setEnabled(true)

        // CINEMAX closed itself because nothing was playing: close the screen too.
        lifecycleScope.launch {
            Cinemax.closeRequests.collect { finishAndRemoveTask() }
        }

        setContent {
            CinemaxApp(
                onStartFullEngine = { requestFullEngine() },
                onRequestSpectrum = { askAudio.launch(Manifest.permission.RECORD_AUDIO) },
                hasSpectrumPermission = { hasAudioPermission() },
                onOpenBatterySettings = {
                    startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName")))
                },
                onOpenSoundSettings = {
                    // Samsung's "Sound quality and effects" answers the standard effect-panel intent.
                    val panel = Intent(android.media.audiofx.AudioEffect.ACTION_DISPLAY_AUDIO_EFFECT_CONTROL_PANEL)
                    runCatching { startActivity(panel) }.onFailure { startActivity(Intent(Settings.ACTION_SOUND_SETTINGS)) }
                },
                onRequestCamera = { askCamera.launch(Manifest.permission.CAMERA) },
                hasCameraPermission = {
                    ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
                },
                askEngine = askEngine.value,
                onEngineAnswer = { yes ->
                    askEngine.value = false
                    if (yes) requestFullEngine()
                    else Cinemax.reportFull(FullEngineState(FullEngineState.Status.NEEDS_CONSENT))
                },
            )
        }
    }

    override fun onResume() {
        super.onResume()
        Cinemax.lastInteraction = SystemClock.elapsedRealtime()
        if (Cinemax.userWantsOn() && !Cinemax.state.value.enabled) Cinemax.setEnabled(true)
        if (hasAudioPermission()) spectrum.start()
        Cinemax.refreshSetup()
        // Head tracking needs the service to declare the camera while CINEMAX is on screen.
        if (Cinemax.state.value.headTracking && Cinemax.state.value.enabled)
            startService(Intent(this, CinemaxService::class.java).setAction(CinemaxService.ACTION_REFRESH))
        // Opening CINEMAX starts the full engine (one tap on Android's prompt).
        val s = Cinemax.state.value
        if (!promptedThisVisit && s.fullWanted && !s.fullRunning && s.full.status != FullEngineState.Status.STARTING) {
            promptedThisVisit = true
            val wait = if (introPending) INTRO_MS else 0L
            introPending = false
            window.decorView.postDelayed({
                val now = Cinemax.state.value
                if (lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.RESUMED) &&
                    now.fullWanted && !now.fullRunning && now.full.status != FullEngineState.Status.STARTING) askEngine.value = true
            }, wait + SETTLE_MS)
        }
    }

    override fun onStop() {
        promptedThisVisit = false
        askEngine.value = false
        super.onStop()
    }

    private fun requestFullEngine() {
        if (!hasAudioPermission()) askAudioForEngine.launch(Manifest.permission.RECORD_AUDIO)
        else launchCapturePrompt()
    }

    private fun launchCapturePrompt() {
        val mpm = getSystemService(MediaProjectionManager::class.java)
        val intent = if (Build.VERSION.SDK_INT >= 34)
            mpm.createScreenCaptureIntent(MediaProjectionConfig.createConfigForDefaultDisplay())
        else mpm.createScreenCaptureIntent()
        askCapture.launch(intent)
    }

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        Cinemax.lastInteraction = SystemClock.elapsedRealtime()
        return super.dispatchTouchEvent(ev)
    }

    override fun onPause() {
        spectrum.stop()
        super.onPause()
    }

    private fun hasAudioPermission() =
        ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    companion object {
        /** Length of the opening screen (CinemaxApp's Intro) plus its fade. */
        private const val INTRO_MS = 3300L
        /** A moment on the Home screen before asking anything. */
        private const val SETTLE_MS = 600L
    }
}
