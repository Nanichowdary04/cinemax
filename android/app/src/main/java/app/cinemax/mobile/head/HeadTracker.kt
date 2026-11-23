package app.cinemax.mobile.head

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.hardware.display.DisplayManager
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Display
import app.cinemax.mobile.capture.NativeEngine
import java.util.concurrent.Executor
import kotlin.math.abs

/** What the head tracker is doing, for the screen. */
data class HeadStatus(
    /** off | starting | tracking | no-face | no-camera | camera-busy | camera-denied | error */
    val status: String = "off",
    val message: String = "",
    val yaw: Float = 0f,
    val fps: Float = 0f,
)

/**
 * Follows your head with the front camera so the 3D sound stays on the phone screen when you
 * turn (like AirPods). The same face analysis as the laptop runs in libcinemax.so on small
 * frames (~640x480, at most 15 per second); the pose goes straight to the audio engine.
 * Frames are analysed in memory and dropped - nothing is recorded, saved or sent.
 */
class HeadTracker(private val ctx: Context, private val onStatus: (HeadStatus) -> Unit) {
    @Volatile var flip = false

    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null
    private var face = 0L
    private var running = false
    private var sensorOrientation = 270
    private val out = FloatArray(5)
    private var lastFrameMs = 0L
    private var fps = 0f
    private var lastReportMs = 0L
    private var status = HeadStatus()

    fun start() {
        if (running) return
        running = true
        val t = HandlerThread("cinemax-head").also { it.start() }
        thread = t
        handler = Handler(t.looper)
        report(HeadStatus("starting", "Starting the camera…"))
        handler?.post { open() }
    }

    fun stop() {
        if (!running) return
        running = false
        val h = handler
        h?.post {
            closeCamera()
            if (face != 0L) { NativeEngine.nFaceDestroy(face); face = 0L }
        }
        thread?.quitSafely()
        runCatching { thread?.join(1500) }
        thread = null
        handler = null
        report(HeadStatus("off"))
    }

    // ------------------------------------------------------------------ camera (tracker thread)
    @SuppressLint("MissingPermission")
    private fun open() {
        if (!running) return
        if (ctx.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            report(HeadStatus("camera-denied", "Allow CINEMAX to use the camera (Settings > Apps > CINEMAX > Permissions).")); return
        }
        if (face == 0L) {
            runCatching { System.loadLibrary("onnxruntime") }
            val model = runCatching { ctx.assets.open("models/blaze_face_short_range.onnx").use { it.readBytes() } }.getOrNull()
            face = if (model != null && NativeEngine.loaded) NativeEngine.nFaceCreate(model) else 0L
            if (face == 0L) {
                val why = if (model == null) "the face model is missing from this build" else NativeEngine.nFaceError()
                report(HeadStatus("error", "Head tracking can't start: $why")); return
            }
        }
        val cm = ctx.getSystemService(CameraManager::class.java)
        val id = cm.cameraIdList.firstOrNull {
            cm.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
        }
        if (id == null) { report(HeadStatus("no-camera", "This phone has no front camera.")); return }
        val ch = cm.getCameraCharacteristics(id)
        sensorOrientation = ch.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 270
        val map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val sizes = map?.getOutputSizes(ImageFormat.YUV_420_888)?.toList().orEmpty()
        val size = sizes.filter { it.width >= 320 && it.height >= 240 }
            .minByOrNull { abs(it.width - 640) + abs(it.height - 480) * 2 } ?: Size(640, 480)
        val fpsRange = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)?.let { ranges ->
            ranges.filter { it.upper <= 15 }.maxByOrNull { it.upper * 100 + it.lower }
                ?: ranges.minByOrNull { it.upper * 100 - it.lower }
        }

        val r = ImageReader.newInstance(size.width, size.height, ImageFormat.YUV_420_888, 2)
        r.setOnImageAvailableListener({ onFrame(it) }, handler)
        reader = r

        try {
            cm.openCamera(id, object : CameraDevice.StateCallback() {
                override fun onOpened(device: CameraDevice) {
                    if (!running) { device.close(); return }
                    camera = device
                    startSession(device, r, fpsRange)
                }
                override fun onDisconnected(device: CameraDevice) {
                    device.close()
                    if (camera === device) camera = null
                    retryLater("camera-busy", "Another app is using the camera.")
                }
                override fun onError(device: CameraDevice, error: Int) {
                    device.close()
                    if (camera === device) camera = null
                    if (error == ERROR_CAMERA_IN_USE || error == ERROR_MAX_CAMERAS_IN_USE)
                        retryLater("camera-busy", "Another app is using the camera.")
                    else if (error == ERROR_CAMERA_DISABLED)
                        retryLater("camera-denied", "The camera is turned off for apps (quick settings or device policy).")
                    else
                        retryLater("error", "The camera stopped (error $error); retrying.")
                }
            }, handler)
        } catch (t: Throwable) {
            Log.w(TAG, "openCamera failed", t)
            retryLater("camera-busy", "The camera can't be opened right now (${t.message ?: t.javaClass.simpleName}).")
        }
    }

    private fun startSession(device: CameraDevice, r: ImageReader, fpsRange: Range<Int>?) {
        val h = handler ?: return
        val exec = Executor { h.post(it) }
        val cb = object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(s: CameraCaptureSession) {
                if (!running) { s.close(); return }
                session = s
                runCatching {
                    val req = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                        addTarget(r.surface)
                        if (fpsRange != null) set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange)
                    }.build()
                    s.setRepeatingRequest(req, null, h)
                    Log.i(TAG, "camera ${r.width}x${r.height}, sensor turned $sensorOrientation deg, fps range $fpsRange")
                    report(HeadStatus("no-face", "Looking for your face…"))
                }.onFailure { retryLater("error", "The camera could not start (${it.message}).") }
            }
            override fun onConfigureFailed(s: CameraCaptureSession) {
                retryLater("error", "The camera could not be set up.")
            }
        }
        runCatching {
            device.createCaptureSession(SessionConfiguration(SessionConfiguration.SESSION_REGULAR,
                listOf(OutputConfiguration(r.surface)), exec, cb))
        }.onFailure { retryLater("error", "The camera could not be set up (${it.message}).") }
    }

    private fun retryLater(state: String, message: String) {
        closeCamera()
        report(HeadStatus(state, message))
        handler?.postDelayed({ if (running && camera == null) open() }, 5000)
    }

    private fun closeCamera() {
        runCatching { session?.close() }
        session = null
        runCatching { camera?.close() }
        camera = null
        runCatching { reader?.close() }
        reader = null
    }

    // ------------------------------------------------------------------ frames
    private fun onFrame(r: ImageReader) {
        val img = runCatching { r.acquireLatestImage() }.getOrNull() ?: return
        try {
            val now = SystemClock.elapsedRealtime()
            if (now - lastFrameMs < 60) return          // at most ~15 analyses per second
            val dt = (now - lastFrameMs).coerceIn(1, 1000) / 1000f
            lastFrameMs = now
            fps = if (fps <= 0f) 1f / dt else fps + 0.1f * (1f / dt - fps)
            if (face == 0L || img.planes.size < 3) return
            val p = img.planes
            NativeEngine.nFaceAnalyse(face, p[0].buffer, p[1].buffer, p[2].buffer, p[0].rowStride, p[1].rowStride,
                p[1].pixelStride, img.width, img.height, expectedRotation(), flip, out)
            if (now - lastReportMs >= 400) {
                lastReportMs = now
                val tracking = out[0] > 0.5f
                report(if (tracking) HeadStatus("tracking", "", out[1], fps)
                       else HeadStatus("no-face", "Looking for your face…", out[1], fps))
            }
        } finally {
            img.close()
        }
    }

    /** Clockwise turn that stands the front camera's picture upright on the current screen. */
    private fun expectedRotation(): Int {
        val dm = ctx.getSystemService(DisplayManager::class.java)
        val rot = dm.getDisplay(Display.DEFAULT_DISPLAY)?.rotation ?: 0
        return (sensorOrientation + rot * 90) % 360
    }

    private fun report(s: HeadStatus) {
        if (s == status) return
        status = s
        onStatus(s)
    }

    companion object { private const val TAG = "CinemaxHead" }
}
