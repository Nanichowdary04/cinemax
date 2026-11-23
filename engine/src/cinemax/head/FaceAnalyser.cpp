#include "FaceAnalyser.h"

#include "onnxruntime_c_api.h"

#include <algorithm>
#include <cmath>

namespace cinemax::head
{
namespace
{
    constexpr float kPiF = 3.14159265f;
    constexpr int kInput = 128;           // BlazeFace short-range input size
    constexpr int kAnchors = 896;
    constexpr float kScoreMin = 0.5f;
    constexpr float kNoseDepth = 0.3125f; // nose tip in front of the eyes, in eye-distances (2.0 / 6.4 cm)

    /** BlazeFace SSD anchors: 16x16 grid x 2 (stride 8) then 8x8 grid x 6 (strides 16,16,16). */
    std::vector<std::array<float, 2>> makeAnchors()
    {
        std::vector<std::array<float, 2>> a;
        a.reserve (kAnchors);
        for (auto [grid, perCell] : { std::pair<int, int> { 16, 2 }, std::pair<int, int> { 8, 6 } })
            for (int y = 0; y < grid; ++y)
                for (int x = 0; x < grid; ++x)
                    for (int i = 0; i < perCell; ++i)
                        a.push_back ({ (x + 0.5f) / (float) grid, (y + 0.5f) / (float) grid });
        return a;
    }

    float euroAlpha (float cutoff, float dt)
    {
        const float tau = 1.0f / (2.0f * kPiF * cutoff);
        return 1.0f / (1.0f + tau / dt);
    }

    inline float clamp255 (float v) { return std::clamp (v, 0.0f, 255.0f); }
}

//==============================================================================
float OneEuro::filter (float value, float dt)
{
    if (! primed) { x = value; dx = 0.0f; primed = true; return value; }
    const float d = (value - x) / dt;
    dx += euroAlpha (dCutoff, dt) * (d - dx);
    const float cutoff = minCutoff + beta * std::abs (dx);
    x += euroAlpha (cutoff, dt) * (value - x);
    return x;
}

//==============================================================================
FaceAnalyser::FaceAnalyser()
    : input ((size_t) kInput * kInput * 3, 0.0f), anchors (makeAnchors())
{
}

FaceAnalyser::~FaceAnalyser()
{
    if (api == nullptr) return;
    if (inputValue) api->ReleaseValue (inputValue);
    if (memory) api->ReleaseMemoryInfo (memory);
    if (session) api->ReleaseSession (session);
    if (options) api->ReleaseSessionOptions (options);
    if (env) api->ReleaseEnv (env);
}

bool FaceAnalyser::ok (void* status)
{
    auto* s = static_cast<OrtStatus*> (status);
    if (s == nullptr) return true;
    lastError = api->GetErrorMessage (s);
    api->ReleaseStatus (s);
    return false;
}

bool FaceAnalyser::load (const OrtApi* ortApi, const void* model, size_t modelBytes, std::string& error)
{
    api = ortApi;
    if (api == nullptr) { error = "ONNX Runtime has the wrong version"; return false; }
    if (model == nullptr || modelBytes < 1024) { error = "the face model is missing"; return false; }
    auto fail = [&] { error = lastError; return false; };
    if (! ok (api->CreateEnv (ORT_LOGGING_LEVEL_WARNING, "cinemax-head", &env))) return fail();
    if (! ok (api->CreateSessionOptions (&options))) return fail();
    ok (api->SetIntraOpNumThreads (options, 1));      // a few ms per frame on one core is plenty
    ok (api->SetInterOpNumThreads (options, 1));
    ok (api->SetSessionGraphOptimizationLevel (options, ORT_ENABLE_ALL));
    if (! ok (api->CreateSessionFromArray (env, model, modelBytes, options, &session))) return fail();
    if (! ok (api->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &memory))) return fail();
    const int64_t shape[4] { 1, kInput, kInput, 3 };
    if (! ok (api->CreateTensorWithDataAsOrtValue (memory, input.data(), input.size() * sizeof (float),
                                                   shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputValue)))
        return fail();
    return true;
}

float FaceAnalyser::runDetector (int& bestIndex)
{
    const char* inNames[] { "input" };
    const char* outNames[] { "regressors", "classificators" };
    OrtValue* outs[2] { nullptr, nullptr };
    float s = 0.0f;
    bestIndex = -1;
    if (ok (api->Run (session, nullptr, inNames, &inputValue, 1, outNames, 2, outs)))
    {
        float* r = nullptr;
        float* cls = nullptr;
        if (! ok (api->GetTensorMutableData (outs[0], (void**) &r)) || ! ok (api->GetTensorMutableData (outs[1], (void**) &cls))
            || r == nullptr || cls == nullptr)
        {
            for (auto* o : outs)
                if (o != nullptr) api->ReleaseValue (o);
            return 0.0f;
        }
        float bestLogit = -1e9f;
        for (int i = 0; i < kAnchors; ++i)
            if (cls[i] > bestLogit) { bestLogit = cls[i]; bestIndex = i; }
        s = 1.0f / (1.0f + std::exp (-std::clamp (bestLogit, -50.0f, 50.0f)));
        std::copy (r + bestIndex * 16, r + bestIndex * 16 + 16, reg.begin());
    }
    for (auto* o : outs)
        if (o != nullptr) api->ReleaseValue (o);
    return s;
}

void FaceAnalyser::resetTracking()
{
    haveFace = false;
    roiSide = 0;
    fYaw.reset();
    fPitch.reset();
}

void FaceAnalyser::resetCalibration()
{
    resetTracking();
    calib = 0;
    yawBase = pitchBase = 0.0f;
}

//==============================================================================
template <typename Pixel>
bool FaceAnalyser::analyse (int width, int height, float dt, bool probeOnly, Pixel&& rgbAt)
{
    if (session == nullptr) return false;
    const float W = (float) width, H = (float) height;
    const float savedRoi[3] { roiX, roiY, roiSide };
    if (probeOnly || ! haveFace || roiSide <= 0.0f)
    {
        roiSide = std::max (W, H);
        roiX = (W - roiSide) * 0.5f;
        roiY = (H - roiSide) * 0.5f;
    }

    // ---- region -> 128x128 RGB in [-1, 1] (bilinear) ------------------------------------
    const float scale = roiSide / (float) kInput;
    for (int y = 0; y < kInput; ++y)
    {
        const float fy = roiY + (y + 0.5f) * scale - 0.5f;
        const int y0 = (int) std::floor (fy);
        const float wy = fy - (float) y0;
        for (int x = 0; x < kInput; ++x)
        {
            const float fx = roiX + (x + 0.5f) * scale - 0.5f;
            const int x0 = (int) std::floor (fx);
            const float wx = fx - (float) x0;
            float rgb[3] { 0, 0, 0 };
            for (int k = 0; k < 4; ++k)
            {
                const int px = x0 + (k & 1), py = y0 + (k >> 1);
                if (px < 0 || py < 0 || px >= width || py >= height) continue;
                const float w = ((k & 1) ? wx : 1.0f - wx) * ((k >> 1) ? wy : 1.0f - wy);
                float r, g, b;
                rgbAt (px, py, r, g, b);
                rgb[0] += w * r;
                rgb[1] += w * g;
                rgb[2] += w * b;
            }
            float* o = input.data() + ((size_t) y * kInput + (size_t) x) * 3;
            o[0] = rgb[0] / 127.5f - 1.0f;
            o[1] = rgb[1] / 127.5f - 1.0f;
            o[2] = rgb[2] / 127.5f - 1.0f;
        }
    }

    // ---- detect -----------------------------------------------------------------------------
    int idx = -1;
    score = runDetector (idx);
    if (probeOnly)
    {
        roiX = savedRoi[0];
        roiY = savedRoi[1];
        roiSide = savedRoi[2];
        return score >= kScoreMin && idx >= 0;
    }
    if (score < kScoreMin || idx < 0)
    {
        haveFace = false;
        return false;
    }

    const auto& a = anchors[(size_t) idx];
    auto toFrame = [&] (float rx, float ry, float& fx, float& fy)
    {
        fx = roiX + (rx / kInput + a[0]) * roiSide;
        fy = roiY + (ry / kInput + a[1]) * roiSide;
    };
    float cx, cy, kx[6], ky[6];
    toFrame (reg[0], reg[1], cx, cy);
    const float faceSize = std::max (reg[2], reg[3]) / kInput * roiSide;
    for (int k = 0; k < 6; ++k)
        toFrame (reg[(size_t) (4 + 2 * k)], reg[(size_t) (5 + 2 * k)], kx[k], ky[k]);

    // next frame: a square around the face (more pixels on the face, better points)
    haveFace = faceSize > 8.0f;
    roiSide = std::clamp (faceSize * 2.6f, 96.0f, std::max (W, H));
    roiX = cx - roiSide * 0.5f;
    roiY = cy - roiSide * 0.5f;

    // ---- pose: remove roll, measure the nose against the eyes and mouth -----------------------
    // points: 0 subject's right eye (image left), 1 left eye, 2 nose tip, 3 mouth
    const float ex = kx[1] - kx[0], ey = ky[1] - ky[0];
    const float eyeDist = std::sqrt (ex * ex + ey * ey);
    if (eyeDist <= 4.0f)
        return false;
    const float roll = std::atan2 (ey, ex);
    const float c = std::cos (-roll), s = std::sin (-roll);
    const float mx = 0.5f * (kx[0] + kx[1]), my = 0.5f * (ky[0] + ky[1]);
    auto rel = [&] (int k, float& rx, float& ry)
    {
        const float dx = kx[k] - mx, dy = ky[k] - my;
        rx = (c * dx - s * dy) / eyeDist;
        ry = (s * dx + c * dy) / eyeDist;
    };
    float nx, ny, mxr, myr;
    rel (2, nx, ny);
    rel (3, mxr, myr);
    const float mouthDrop = std::max (myr, 0.3f);
    // Turning to your right moves the nose toward your right eye (image left).
    rawYaw = std::atan (-nx / kNoseDepth) * 180.0f / kPiF;
    rawPitch = ny / mouthDrop;                       // ~0.45 looking straight

    // Learn the neutral pose: quickly over the first frames, then very slowly and only from
    // frames close to it (you mostly look at the screen).
    if (calib < 15)
    {
        yawBase = (yawBase * (float) calib + rawYaw) / (float) (calib + 1);
        pitchBase = (pitchBase * (float) calib + rawPitch) / (float) (calib + 1);
        ++calib;
    }
    else if (std::abs (rawYaw - yawBase) < 15.0f)
    {
        const float k = dt / 40.0f;
        yawBase += k * (rawYaw - yawBase);
        pitchBase += k * (rawPitch - pitchBase);
    }
    const float pitchDeg = std::atan ((pitchBase - rawPitch) * mouthDrop / kNoseDepth) * 180.0f / kPiF;
    yaw = std::clamp (fYaw.filter (rawYaw - yawBase, dt), -75.0f, 75.0f);
    pitch = std::clamp (fPitch.filter (pitchDeg, dt), -40.0f, 40.0f);
    return true;
}

bool FaceAnalyser::analyseBgra (const uint8_t* base, ptrdiff_t pitchBytes, int width, int height, float dt)
{
    return analyse (width, height, dt, false, [&] (int px, int py, float& r, float& g, float& b)
    {
        const uint8_t* p = base + (ptrdiff_t) py * pitchBytes + (ptrdiff_t) px * 4;   // B G R A
        r = p[2];
        g = p[1];
        b = p[0];
    });
}

template <typename Fn>
bool FaceAnalyser::withYuv (const uint8_t* yp, int yRowStride, const uint8_t* up, const uint8_t* vp, int uvRowStride,
                            int uvPixelStride, int width, int height, int rotation, Fn&& fn)
{
    rotation = ((rotation % 360) + 360) % 360;
    const bool swap = rotation == 90 || rotation == 270;
    const int uw = swap ? height : width, uh = swap ? width : height;   // upright size
    return fn (uw, uh, [&] (int ux, int uy, float& r, float& g, float& b)
    {
        // upright pixel -> sensor pixel (the sensor picture turned clockwise by 'rotation')
        int sx, sy;
        switch (rotation)
        {
            case 90:  sx = uy;             sy = height - 1 - ux; break;
            case 180: sx = width - 1 - ux; sy = height - 1 - uy; break;
            case 270: sx = width - 1 - uy; sy = ux;              break;
            default:  sx = ux;             sy = uy;              break;
        }
        const float Y = yp[(size_t) sy * (size_t) yRowStride + (size_t) sx];
        const size_t c = (size_t) (sy >> 1) * (size_t) uvRowStride + (size_t) (sx >> 1) * (size_t) uvPixelStride;
        const float U = (float) up[c] - 128.0f, V = (float) vp[c] - 128.0f;
        r = clamp255 (Y + 1.402f * V);                       // camera YUV is full-range BT.601 (JFIF)
        g = clamp255 (Y - 0.344136f * U - 0.714136f * V);
        b = clamp255 (Y + 1.772f * U);
    });
}

bool FaceAnalyser::analyseYuv (const uint8_t* y, int yRowStride, const uint8_t* u, const uint8_t* v, int uvRowStride,
                               int uvPixelStride, int width, int height, int rotation, float dt)
{
    return withYuv (y, yRowStride, u, v, uvRowStride, uvPixelStride, width, height, rotation,
                    [&] (int uw, int uh, auto&& px) { return analyse (uw, uh, dt, false, px); });
}

float FaceAnalyser::probeYuv (const uint8_t* y, int yRowStride, const uint8_t* u, const uint8_t* v, int uvRowStride,
                              int uvPixelStride, int width, int height, int rotation)
{
    const float keep = score;
    withYuv (y, yRowStride, u, v, uvRowStride, uvPixelStride, width, height, rotation,
             [&] (int uw, int uh, auto&& px) { return analyse (uw, uh, 0.03f, true, px); });
    const float s = score;
    score = keep;
    return s;
}
} // namespace cinemax::head
