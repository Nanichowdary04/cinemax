// Head pose from one camera frame - shared by the laptop (webcam) and the phone (front camera).
//
// MediaPipe BlazeFace (short range) finds the face and six points on it through ONNX Runtime.
// The caller loads ONNX Runtime itself (LoadLibrary / dlopen) and hands over its API table, so
// the app keeps working when the library is missing. The six points become a head yaw / pitch:
//   - roll is removed by rotating the points into the eye line
//   - yaw   = atan(nose offset from the eye centre / 0.31 eye-distances)   (nose ~2 cm in
//             front of the eyes, eyes ~6.4 cm apart)
//   - pitch from where the nose sits between the eyes and the mouth
//   - the neutral pose is learned automatically (you mostly look at the screen)
//   - a One-Euro filter removes jitter without adding lag
// Frames are only read, never kept.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct OrtApi;
struct OrtEnv;
struct OrtSessionOptions;
struct OrtSession;
struct OrtMemoryInfo;
struct OrtValue;

namespace cinemax::head
{
/** One-Euro filter (Casiez et al.): smooth when still, responsive when moving. */
struct OneEuro
{
    float minCutoff = 1.0f, beta = 0.015f, dCutoff = 1.0f;
    float x = 0.0f, dx = 0.0f;
    bool primed = false;

    float filter (float value, float dt);
    void reset() { primed = false; }
};

class FaceAnalyser
{
public:
    FaceAnalyser();
    ~FaceAnalyser();
    FaceAnalyser (const FaceAnalyser&) = delete;
    FaceAnalyser& operator= (const FaceAnalyser&) = delete;

    /** 'api' comes from OrtGetApiBase()->GetApi(ORT_API_VERSION) of a loaded ONNX Runtime;
        'model' is the blaze_face_short_range.onnx file. Returns false and sets 'error'. */
    bool load (const OrtApi* api, const void* model, size_t modelBytes, std::string& error);
    bool loaded() const noexcept { return session != nullptr; }

    /** BGRA frame, top-down when pitchBytes > 0 (negative pitch: bottom-up). */
    bool analyseBgra (const uint8_t* base, ptrdiff_t pitchBytes, int width, int height, float dt);

    /** YUV 4:2:0 frame (Android YUV_420_888) of width x height, which must be rotated
        clockwise by 'rotation' degrees (0, 90, 180, 270) to stand upright. */
    bool analyseYuv (const uint8_t* y, int yRowStride, const uint8_t* u, const uint8_t* v, int uvRowStride,
                     int uvPixelStride, int width, int height, int rotation, float dt);

    /** Detector score (0..1) for the whole frame read with this rotation; changes no tracking
        state. Used to find which way up the camera picture is (upright faces score ~0.9,
        upside-down ones still ~0.65). */
    float probeYuv (const uint8_t* y, int yRowStride, const uint8_t* u, const uint8_t* v, int uvRowStride,
                    int uvPixelStride, int width, int height, int rotation);

    /** Forget the face position (next frame searches the whole picture). */
    void resetTracking();
    /** Also forget the learned neutral pose (camera or screen orientation changed). */
    void resetCalibration();
    /** Lost the face for a while: restart the smoothing from the next measurement. */
    void resetFilters() { fYaw.reset(); fPitch.reset(); }

    // Last results: yaw / pitch in degrees relative to the learned neutral pose (positive yaw =
    // turned to the user's right), raw values before calibration (for tests), detector score.
    float yaw = 0, pitch = 0, score = 0;
    float rawYaw = 0, rawPitch = 0;

private:
    template <typename Pixel>
    bool analyse (int width, int height, float dt, bool probeOnly, Pixel&& rgbAt);
    template <typename Fn>
    bool withYuv (const uint8_t* y, int yRowStride, const uint8_t* u, const uint8_t* v, int uvRowStride,
                  int uvPixelStride, int width, int height, int rotation, Fn&& fn);
    float runDetector (int& bestIndex);
    bool ok (void* status);

    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* options = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;
    OrtValue* inputValue = nullptr;
    std::vector<float> input;
    std::string lastError;

    std::vector<std::array<float, 2>> anchors;
    std::array<float, 16> reg {};
    bool haveFace = false;
    float roiX = 0, roiY = 0, roiSide = 0;          // square region analysed (upright frame pixels)
    float yawBase = 0, pitchBase = 0;
    int calib = 0;
    OneEuro fYaw, fPitch;
};
} // namespace cinemax::head
