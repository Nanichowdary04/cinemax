// JNI bridge for app.cinemax.mobile.capture.NativeEngine.
#include "Pipeline.h"

#include "cinemax/dsp/Hrtf.h"
#include "cinemax/head/FaceAnalyser.h"
#include "onnxruntime_c_api.h"

#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

using cinemax::android::Pipeline;
using cinemax::android::PipelineStats;

namespace
{
Pipeline* get (jlong h) { return reinterpret_cast<Pipeline*> (h); }

/** Head pose from the front-camera tracker; every pipeline's 3D sound follows it. */
cinemax::HeadPose gHeadPose;

std::string keyOf (JNIEnv* env, jstring s)
{
    if (s == nullptr) return {};
    const char* c = env->GetStringUTFChars (s, nullptr);
    std::string k (c != nullptr ? c : "");
    if (c != nullptr) env->ReleaseStringUTFChars (s, c);
    return k;
}

/** "auto" = the dummy head (most neutral start), then KEMAR, then the first set; "model" = head model. */
int resolveEars (const std::string& key)
{
    using cinemax::dsp::HrtfLibrary;
    if (key == "model") return -1;
    if (! key.empty() && key != "auto")
        if (const int i = HrtfLibrary::indexOf (key); i >= 0) return i;
    for (const char* k : { "ku100", "kemar" })
        if (const int i = HrtfLibrary::indexOf (k); i >= 0) return i;
    return HrtfLibrary::count() > 0 ? 0 : -1;
}

/** Phone "custom" values on top of the laptop preset they were edited from. Order:
    preampDb, eq[10], bassDb, subBassDb, dialogue, surround, room, venue, compression,
    loudness (0/1), ceilingDb, [Smart EQ on, profile, amount], [immersive, volume-aware]. */
void applyCustom (cinemax::DspParams& u, const float* v, int n)
{
    if (n < 20) return;
    int i = 0;
    u.preampDb = v[i++];
    for (int b = 0; b < cinemax::kNumEqBands; ++b)
        u.eqBands[(size_t) b].gainDb = v[i++];
    u.eqEnabled = true;

    u.bassBoostDb = v[i++];
    u.subBassDb = v[i++];
    if (u.bassBoostDb > 0.05f || std::abs (u.subBassDb) > 0.05f) u.bassEnabled = true;

    u.dialogueAmount = v[i++];
    u.dialogueEnabled = u.dialogueAmount > 0.01f;

    u.surroundIntensity = v[i++];
    u.roomAmount = v[i++];
    u.roomSize = v[i++];
    if (u.surroundIntensity > 0.01f || u.roomAmount > 0.01f) u.surroundEnabled = true;

    u.compressorAmount = v[i++];
    u.loudnessEnabled = v[i++] > 0.5f;
    if (u.compressorAmount > 0.01f || u.loudnessEnabled) u.dynamicsEnabled = true;

    u.limiterCeilingDb = v[i++];

    if (n >= 23)
    {
        u.ieqEnabled = v[i++] > 0.5f;
        u.ieqProfile = (int) std::lround (v[i++]);
        u.ieqAmount = v[i++];
    }
    if (n >= 25)
    {
        u.immersive = v[i++] > 0.5f;
        u.volumeModeler = v[i++] > 0.5f;
    }
}
} // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nCreate (JNIEnv*, jclass, jint captureRate, jint outputRate)
{
    auto* p = new Pipeline ((int) captureRate, (int) outputRate);
    p->setHeadPose (&gHeadPose);
    return reinterpret_cast<jlong> (p);
}

JNIEXPORT void JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nDestroy (JNIEnv*, jclass, jlong h)
{
    delete get (h);
}

JNIEXPORT void JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nPush (JNIEnv* env, jclass, jlong h, jfloatArray buf, jint frames)
{
    auto* data = static_cast<float*> (env->GetPrimitiveArrayCritical (buf, nullptr));
    if (data == nullptr) return;
    get (h)->push (data, (int) frames);
    env->ReleasePrimitiveArrayCritical (buf, data, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nRender (JNIEnv* env, jclass, jlong h, jfloatArray buf, jint frames)
{
    auto* data = static_cast<float*> (env->GetPrimitiveArrayCritical (buf, nullptr));
    if (data == nullptr) return;
    get (h)->render (data, (int) frames);
    env->ReleasePrimitiveArrayCritical (buf, data, 0);
}

JNIEXPORT jdouble JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nSecondsSinceSignal (JNIEnv*, jclass, jlong h)
{
    return get (h)->secondsSinceSignal();
}

JNIEXPORT void JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nResync (JNIEnv*, jclass, jlong h)
{
    get (h)->resync();
}

JNIEXPORT void JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nSetParams (JNIEnv* env, jclass, jlong h, jstring presetKey, jstring baseKey,
                                                       jfloatArray custom, jboolean headphones, jboolean smartTone,
                                                       jboolean bypass, jfloat speakerHighPassHz, jstring earKey,
                                                       jstring auditionKey, jint auditionId, jfloatArray tvSetup)
{
    const auto id = cinemax::presetFromKey (keyOf (env, presetKey)).value_or (cinemax::PresetId::Neutral);

    cinemax::DspParams u;
    if (id == cinemax::PresetId::Custom)
    {
        auto base = cinemax::presetFromKey (keyOf (env, baseKey)).value_or (cinemax::PresetId::Neutral);
        if (base == cinemax::PresetId::Custom) base = cinemax::PresetId::Neutral;
        u = cinemax::makePreset (base);
        const jsize n = env->GetArrayLength (custom);
        std::vector<float> v ((size_t) n);
        env->GetFloatArrayRegion (custom, 0, n, v.data());
        applyCustom (u, v.data(), (int) n);
    }
    else
    {
        u = cinemax::makePreset (id);   // exactly the laptop's preset
    }
    u.preset = id;
    u.surroundMode = headphones ? cinemax::SurroundMode::Headphones : cinemax::SurroundMode::Speakers;
    cinemax::clampParams (u);

    cinemax::ProcessingParams p;
    p.user = u;
    p.headphones = headphones;
    p.smartTone = smartTone;
    p.bypass = bypass;
    p.speakerHighPassHz = speakerHighPassHz;
    p.hrtfSet = resolveEars (keyOf (env, earKey));

    // Ear test: "" = off, "model" = head model, otherwise a measured set.
    const auto aKey = keyOf (env, auditionKey);
    if (aKey.empty()) p.auditionSet = -2;
    else if (aKey == "model") p.auditionSet = -1;
    else { const int i = cinemax::dsp::HrtfLibrary::indexOf (aKey); p.auditionSet = i >= 0 ? i : -2; }
    p.auditionId = (unsigned) auditionId;

    // TV output: isTv, speakers (1 TV / 2 soundbar / 3 receiver), size ", distance m, lip-sync ms.
    if (tvSetup != nullptr && env->GetArrayLength (tvSetup) >= 5)
    {
        float t[5] {};
        env->GetFloatArrayRegion (tvSetup, 0, 5, t);
        p.tv.isTv = t[0] > 0.5f;
        p.tv.speakers = std::clamp ((int) std::lround (t[1]), 1, 3);
        p.tv.sizeInch = std::clamp (t[2], 24.0f, 120.0f);
        p.tv.distanceM = std::clamp (t[3], 0.8f, 8.0f);
        p.tv.lipSyncMs = std::clamp (t[4], 0.0f, 300.0f);
    }
    // [5]: the media volume (dB), applied by Android after CINEMAX - drives the volume-aware tone.
    if (tvSetup != nullptr && env->GetArrayLength (tvSetup) >= 6)
    {
        float vol = 0.0f;
        env->GetFloatArrayRegion (tvSetup, 5, 1, &vol);
        p.listenVolumeDb = std::clamp (vol, -96.0f, 0.0f);
    }
    get (h)->setParams (p);
}

JNIEXPORT void JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nStats (JNIEnv* env, jclass, jlong h, jfloatArray out)
{
    PipelineStats s {};
    get (h)->readStats (s);
    const float v[PipelineStats::kCount] { s.fillMs, s.driftPpm, s.xruns, s.inputPeak, s.outputPeak, s.movieProbability,
                                          s.confidence, s.smartBassDb, s.smartTrebleDb, s.cpuPercent,
                                          s.secondsSinceSignal, s.limiterGrDb, s.chunkMs, s.dropped };
    env->SetFloatArrayRegion (out, 0, std::min ((jsize) PipelineStats::kCount, env->GetArrayLength (out)), v);
}

//==============================================================================
// Real ears: the measured HRTF sets (assets/hrtf/*.chrtf). Install once, before any pipeline
// is created. Returns key, name, info for every set that loaded.
JNIEXPORT jobjectArray JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nInstallHrtf (JNIEnv* env, jclass, jobjectArray files)
{
    std::vector<std::shared_ptr<const cinemax::dsp::HrtfSet>> sets;
    const jsize n = files != nullptr ? env->GetArrayLength (files) : 0;
    for (jsize i = 0; i < n; ++i)
    {
        auto b = (jbyteArray) env->GetObjectArrayElement (files, i);
        if (b == nullptr) continue;
        const jsize len = env->GetArrayLength (b);
        std::vector<jbyte> data ((size_t) len);
        env->GetByteArrayRegion (b, 0, len, data.data());
        env->DeleteLocalRef (b);
        std::string error;
        if (auto set = cinemax::dsp::parseHrtf (data.data(), data.size(), error))
            sets.push_back (set);
        else
            __android_log_print (ANDROID_LOG_WARN, "CinemaxNative", "HRTF set %d ignored: %s", (int) i, error.c_str());
    }
    cinemax::dsp::HrtfLibrary::install (sets);

    jclass strCls = env->FindClass ("java/lang/String");
    auto out = env->NewObjectArray ((jsize) sets.size() * 3, strCls, nullptr);
    for (size_t i = 0; i < sets.size(); ++i)
    {
        const std::string* f[3] { &sets[i]->key, &sets[i]->name, &sets[i]->info };
        for (int k = 0; k < 3; ++k)
        {
            jstring js = env->NewStringUTF (f[k]->c_str());
            env->SetObjectArrayElement (out, (jsize) (i * 3 + (size_t) k), js);
            env->DeleteLocalRef (js);
        }
    }
    return out;
}

} // extern "C"

//==============================================================================
// Head tracking with the front camera (same face analysis as the laptop).
namespace
{
std::mutex gOrtLock;
void* gOrtLib = nullptr;
const OrtApi* gOrtApi = nullptr;
std::string gFaceError;

double nowSeconds()
{
    using namespace std::chrono;
    return duration<double> (steady_clock::now().time_since_epoch()).count();
}

struct FaceTracker
{
    cinemax::head::FaceAnalyser fa;
    int rotation = -1;            // locked reading of the camera picture; -1 = finding it
    int hint = -1;                // expected reading from the sensor and screen orientation
    float probeSum[4] {};
    int probeFrames = 0, frameCount = 0;
    double lastFace = -1.0e9, lastFrame = 0.0;
    float outYaw = 0.0f, outPitch = 0.0f;
};
} // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nFaceCreate (JNIEnv* env, jclass, jbyteArray model)
{
    {
        std::lock_guard<std::mutex> g (gOrtLock);
        if (gOrtApi == nullptr)
        {
            if (gOrtLib == nullptr) gOrtLib = dlopen ("libonnxruntime.so", RTLD_NOW | RTLD_LOCAL);
            if (gOrtLib == nullptr) { gFaceError = std::string ("ONNX Runtime is missing: ") + (dlerror() ?: "?"); return 0; }
            using GetBase = const OrtApiBase* (*) ();
            auto getBase = (GetBase) dlsym (gOrtLib, "OrtGetApiBase");
            if (getBase == nullptr || (gOrtApi = getBase()->GetApi (ORT_API_VERSION)) == nullptr)
            {
                gFaceError = "ONNX Runtime has the wrong version";
                return 0;
            }
        }
    }
    const jsize len = model != nullptr ? env->GetArrayLength (model) : 0;
    std::vector<jbyte> bytes ((size_t) len);
    if (len > 0) env->GetByteArrayRegion (model, 0, len, bytes.data());
    auto* t = new FaceTracker();
    std::string error;
    if (! t->fa.load (gOrtApi, bytes.data(), bytes.size(), error))
    {
        gFaceError = "The face model could not be loaded: " + error;
        delete t;
        return 0;
    }
    return reinterpret_cast<jlong> (t);
}

JNIEXPORT jstring JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nFaceError (JNIEnv* env, jclass)
{
    return env->NewStringUTF (gFaceError.c_str());
}

JNIEXPORT void JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nFaceDestroy (JNIEnv*, jclass, jlong h)
{
    gHeadPose.active.store (false);
    gHeadPose.yaw.store (0.0f);
    gHeadPose.pitch.store (0.0f);
    delete reinterpret_cast<FaceTracker*> (h);
}

/** One YUV_420_888 camera frame. 'hint' = expected clockwise rotation to upright (from the
    sensor and screen orientation). out: [face seen in the last second, yaw, pitch, score,
    reading used (-1 = still finding it)]. Writes the head pose for the audio engine. */
JNIEXPORT void JNICALL
Java_app_cinemax_mobile_capture_NativeEngine_nFaceAnalyse (JNIEnv* env, jclass, jlong h, jobject yBuf, jobject uBuf,
                                                          jobject vBuf, jint yRowStride, jint uvRowStride, jint uvPixelStride,
                                                          jint width, jint height, jint hint, jboolean flip, jfloatArray out)
{
    auto* t = reinterpret_cast<FaceTracker*> (h);
    auto* y = static_cast<const uint8_t*> (env->GetDirectBufferAddress (yBuf));
    auto* u = static_cast<const uint8_t*> (env->GetDirectBufferAddress (uBuf));
    auto* v = static_cast<const uint8_t*> (env->GetDirectBufferAddress (vBuf));
    if (t == nullptr || y == nullptr || u == nullptr || v == nullptr) return;

    const double now = nowSeconds();
    const float dt = (float) std::clamp (now - t->lastFrame, 0.005, 0.5);
    t->lastFrame = now;
    ++t->frameCount;

    hint = ((hint % 360) + 360) % 360 / 90 * 90;
    if (hint != t->hint)
    {
        // The screen turned (portrait <-> landscape): find which way up the picture is again.
        t->hint = hint;
        t->rotation = -1;
        t->probeFrames = 0;
        std::fill (std::begin (t->probeSum), std::end (t->probeSum), 0.0f);
    }

    bool found = false;
    if (t->rotation < 0)
    {
        // Try all four readings over a few frames; upright faces score clearly highest. The
        // expected reading wins unless another one is clearly better.
        if (t->frameCount % 2 == 0)
        {
            for (int k = 0; k < 4; ++k)
                t->probeSum[k] += t->fa.probeYuv (y, yRowStride, u, v, uvRowStride, uvPixelStride, width, height, k * 90);
            if (++t->probeFrames >= 3)
            {
                int best = 0;
                for (int k = 1; k < 4; ++k)
                    if (t->probeSum[k] > t->probeSum[best]) best = k;
                const int hk = t->hint / 90;
                const int pick = t->probeSum[best] - t->probeSum[hk] > 0.15f ? best : hk;
                if (t->probeSum[pick] / (float) t->probeFrames >= 0.5f)
                {
                    t->rotation = pick * 90;
                    t->fa.resetCalibration();
                    __android_log_print (ANDROID_LOG_INFO, "CinemaxHead", "camera picture read at %d deg (expected %d; scores %.2f %.2f %.2f %.2f)",
                                         t->rotation, t->hint, t->probeSum[0] / 3, t->probeSum[1] / 3, t->probeSum[2] / 3, t->probeSum[3] / 3);
                }
                t->probeFrames = 0;
                std::fill (std::begin (t->probeSum), std::end (t->probeSum), 0.0f);
            }
        }
    }
    else
    {
        found = t->fa.analyseYuv (y, yRowStride, u, v, uvRowStride, uvPixelStride, width, height, t->rotation, dt);
        if (found)
        {
            t->outYaw = t->fa.yaw;
            t->outPitch = t->fa.pitch;
            t->lastFace = now;
        }
        else if (now - t->lastFace > 4.0)
        {
            t->rotation = -1;     // gone for a while: check the orientation again when you're back
        }
    }

    const bool recent = now - t->lastFace <= 1.0;
    if (! recent)
    {
        // Lost you (put the phone down, looked away): glide back to straight ahead.
        const float step = 60.0f * dt;
        t->outYaw -= std::clamp (t->outYaw, -step, step);
        t->outPitch -= std::clamp (t->outPitch, -step, step);
        t->fa.resetFilters();
    }
    const float yaw = flip ? -t->outYaw : t->outYaw;
    gHeadPose.yaw.store (yaw, std::memory_order_relaxed);
    gHeadPose.pitch.store (t->outPitch, std::memory_order_relaxed);
    gHeadPose.active.store (true, std::memory_order_relaxed);

    if (out != nullptr && env->GetArrayLength (out) >= 5)
    {
        const float o[5] { recent ? 1.0f : 0.0f, yaw, t->outPitch, t->fa.score, (float) t->rotation };
        env->SetFloatArrayRegion (out, 0, 5, o);
    }
}

} // extern "C"
