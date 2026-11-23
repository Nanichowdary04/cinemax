#include "HeadTracker.h"

#include "cinemax/win/Win32Common.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "cinemax/head/FaceAnalyser.h"
#include "onnxruntime_c_api.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace cinemax
{
namespace
{
    double now()
    {
        using namespace std::chrono;
        return duration<double> (steady_clock::now().time_since_epoch()).count();
    }

    /** ONNX Runtime from the exe folder (loaded once, kept for the process lifetime) and the
        face model from models\. The app runs without them; head tracking then reports why. */
    bool loadAnalyser (head::FaceAnalyser& fa, const std::wstring& dir, std::string& error)
    {
        static std::mutex lock;
        static HMODULE dll = nullptr;
        const OrtApi* api = nullptr;
        {
            std::lock_guard<std::mutex> g (lock);
            if (dll == nullptr)
                dll = LoadLibraryExW ((dir + L"\\onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (dll == nullptr) { error = "onnxruntime.dll is missing next to CINEMAX.exe"; return false; }
            using GetBase = const OrtApiBase* (ORT_API_CALL*) (void);
            auto getBase = (GetBase) GetProcAddress (dll, "OrtGetApiBase");
            if (getBase == nullptr || (api = getBase()->GetApi (ORT_API_VERSION)) == nullptr)
            {
                error = "onnxruntime.dll has the wrong version";
                return false;
            }
        }
        std::vector<char> model;
        {
            const std::wstring path = dir + L"\\models\\blaze_face_short_range.onnx";
            HANDLE f = CreateFileW (path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) { error = "models\\blaze_face_short_range.onnx is missing"; return false; }
            LARGE_INTEGER size {};
            GetFileSizeEx (f, &size);
            model.resize ((size_t) size.QuadPart);
            DWORD got = 0;
            const BOOL readOk = ReadFile (f, model.data(), (DWORD) model.size(), &got, nullptr);
            CloseHandle (f);
            if (! readOk || got != (DWORD) model.size()) { error = "models\\blaze_face_short_range.onnx could not be read"; return false; }
        }
        return fa.load (api, model.data(), model.size(), error);
    }

    //==========================================================================
    struct Camera
    {
        ComPtr<IMFSourceReader> reader;
        UINT32 width = 0, height = 0;
        LONG defaultStride = 0;

        /** Opens the first video capture device in shared mode and asks for RGB32 frames near 640x480. */
        HRESULT open()
        {
            ComPtr<IMFAttributes> attr;
            HRESULT hr = MFCreateAttributes (&attr, 1);
            if (FAILED (hr)) return hr;
            attr->SetGUID (MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
            IMFActivate** devices = nullptr;
            UINT32 count = 0;
            hr = MFEnumDeviceSources (attr.Get(), &devices, &count);
            if (FAILED (hr)) return hr;
            if (count == 0) { CoTaskMemFree (devices); return MF_E_NOT_FOUND; }

            // Share the camera with other apps (Windows 11 24H2+); ignored by older builds.
            devices[0]->SetUINT32 (MF_DEVSOURCE_ATTRIBUTE_FRAMESERVER_SHARE_MODE, 1);
            ComPtr<IMFMediaSource> source;
            hr = devices[0]->ActivateObject (IID_PPV_ARGS (&source));
            for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
            CoTaskMemFree (devices);
            if (FAILED (hr)) return hr;

            ComPtr<IMFAttributes> rattr;
            MFCreateAttributes (&rattr, 2);
            rattr->SetUINT32 (MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   // converts to RGB32 for us
            rattr->SetUINT32 (MF_LOW_LATENCY, TRUE);
            hr = MFCreateSourceReaderFromMediaSource (source.Get(), rattr.Get(), &reader);
            if (FAILED (hr)) return hr;

            // Pick the native mode closest to 640 wide with at least 15 fps (small = low latency).
            const DWORD stream = (DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM;
            ComPtr<IMFMediaType> best;
            double bestScore = 1e18;
            for (DWORD i = 0;; ++i)
            {
                ComPtr<IMFMediaType> t;
                if (FAILED (reader->GetNativeMediaType (stream, i, &t))) break;
                UINT32 w = 0, h = 0, num = 0, den = 1;
                MFGetAttributeSize (t.Get(), MF_MT_FRAME_SIZE, &w, &h);
                MFGetAttributeRatio (t.Get(), MF_MT_FRAME_RATE, &num, &den);
                const double fps = den ? (double) num / den : 0.0;
                if (w < 320) continue;
                const double score = std::abs ((double) w - 640.0) + (fps < 14.5 ? 5000.0 : 0.0) + (fps > 31.0 ? 50.0 : 0.0);
                if (score < bestScore) { bestScore = score; best = t; }
            }
            if (best)
                reader->SetCurrentMediaType (stream, nullptr, best.Get());   // may be refused in shared mode: fine

            ComPtr<IMFMediaType> native;
            hr = reader->GetCurrentMediaType (stream, &native);
            if (FAILED (hr)) return hr;
            MFGetAttributeSize (native.Get(), MF_MT_FRAME_SIZE, &width, &height);

            ComPtr<IMFMediaType> out;
            MFCreateMediaType (&out);
            out->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            MFSetAttributeSize (out.Get(), MF_MT_FRAME_SIZE, width, height);
            hr = reader->SetCurrentMediaType (stream, nullptr, out.Get());
            if (FAILED (hr)) return hr;

            ComPtr<IMFMediaType> cur;
            reader->GetCurrentMediaType (stream, &cur);
            MFGetAttributeSize (cur.Get(), MF_MT_FRAME_SIZE, &width, &height);
            UINT32 strideU = 0;
            if (SUCCEEDED (cur->GetUINT32 (MF_MT_DEFAULT_STRIDE, &strideU)))
                defaultStride = (LONG) strideU;
            else
                MFGetStrideForBitmapInfoHeader (MFVideoFormat_RGB32.Data1, width, &defaultStride);
            return S_OK;
        }
    };

    std::string hrText (HRESULT hr)
    {
        char buf[32];
        std::snprintf (buf, sizeof (buf), "0x%08lX", (unsigned long) hr);
        return buf;
    }
}

//==============================================================================
HeadTracker::HeadTracker (HeadPose& p) : pose (p) {}

HeadTracker::~HeadTracker()
{
    setActive (false);
}

void HeadTracker::setActive (bool shouldRun)
{
    if (shouldRun && ! thread.joinable())
    {
        stopFlag = false;
        setStatus ("starting");
        thread = std::thread ([this] { run(); });
    }
    else if (! shouldRun && thread.joinable())
    {
        stopFlag = true;
        thread.join();
        pose.active = false;
        pose.yaw = 0.0f;
        pose.pitch = 0.0f;
        setStatus ("off");
    }
}

HeadTracker::Status HeadTracker::status() const
{
    std::lock_guard<std::mutex> g (statusLock);
    return current;
}

void HeadTracker::setStatus (const char* s, std::string message)
{
    std::lock_guard<std::mutex> g (statusLock);
    if (current.status != s)
        win::log::info ("Head tracking: %s %s", s, message.c_str());
    current.status = s;
    current.message = std::move (message);
}

bool HeadTracker::loadFaceAnalyser (head::FaceAnalyser& fa, const std::wstring& dir, std::string& error)
{
    return loadAnalyser (fa, dir, error);
}

HeadTracker::ImageResult HeadTracker::analyseImage (const std::wstring& dir, const unsigned char* bgra, int width, int height, int frames)
{
    ImageResult r;
    head::FaceAnalyser fa;
    if (! loadAnalyser (fa, dir, r.error)) return r;
    const auto t0 = now();
    for (int i = 0; i < frames; ++i)
        r.found = fa.analyseBgra (bgra, (ptrdiff_t) width * 4, width, height, 1.0f / 30.0f);
    r.msPerFrame = (float) ((now() - t0) * 1000.0 / std::max (1, frames));
    r.score = fa.score;
    r.rawYaw = fa.rawYaw;
    r.rawPitch = fa.rawPitch;
    return r;
}

//==============================================================================
void HeadTracker::run()
{
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    MFStartup (MF_VERSION, MFSTARTUP_LITE);

    head::FaceAnalyser fa;
    std::string loadError;
    if (! loadAnalyser (fa, win::paths::exeDir(), loadError))
    {
        setStatus ("error", "Head tracking files are missing or damaged: " + loadError);
        while (! stopFlag) Sleep (100);
        MFShutdown();
        CoUninitialize();
        return;
    }

    while (! stopFlag)
    {
        Camera cam;
        const HRESULT hr = cam.open();
        if (FAILED (hr))
        {
            if (hr == MF_E_NOT_FOUND)
                setStatus ("no-camera", "No camera found.");
            else if (hr == E_ACCESSDENIED)
                setStatus ("camera-denied", "Windows is blocking the camera: Settings > Privacy & security > Camera > let desktop apps use it.");
            else if (hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED || hr == MF_E_HW_MFT_FAILED_START_STREAMING
                     || hr == HRESULT_FROM_WIN32 (ERROR_SHARING_VIOLATION))
                setStatus ("camera-busy", "Another app is using the camera.");
            else
                setStatus ("error", "The camera could not be opened (" + hrText (hr) + ").");
            for (int i = 0; i < 50 && ! stopFlag; ++i) Sleep (100);   // retry in 5 s
            continue;
        }
        win::log::info ("Head tracking: camera %ux%u", cam.width, cam.height);

        fa.resetTracking();
        double lastFace = 0, lastFrame = now();
        float fps = 0.0f, outYaw = 0.0f, outPitch = 0.0f;
        pose.active = true;

        while (! stopFlag)
        {
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            const HRESULT rhr = cam.reader->ReadSample ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
            if (FAILED (rhr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)))
            {
                setStatus ("error", "The camera stopped (" + hrText (rhr) + "); retrying.");
                break;
            }
            if (! sample)
                continue;

            const double t = now();
            const float dt = (float) std::clamp (t - lastFrame, 0.005, 0.5);
            lastFrame = t;
            fps = fps <= 0.0f ? 1.0f / dt : fps + 0.1f * (1.0f / dt - fps);

            ComPtr<IMFMediaBuffer> buf;
            if (FAILED (sample->ConvertToContiguousBuffer (&buf)))
                continue;
            BYTE* base = nullptr;
            LONG pitch = 0;
            ComPtr<IMF2DBuffer> b2;
            BYTE* raw = nullptr;
            DWORD maxLen = 0, curLen = 0;
            const bool locked2d = SUCCEEDED (buf.As (&b2)) && SUCCEEDED (b2->Lock2D (&base, &pitch));
            if (! locked2d)
            {
                if (FAILED (buf->Lock (&raw, &maxLen, &curLen))) continue;
                pitch = cam.defaultStride != 0 ? cam.defaultStride : (LONG) cam.width * 4;
                base = pitch < 0 ? raw + (size_t) (cam.height - 1) * (size_t) (-pitch) : raw;
            }
            const bool found = fa.analyseBgra (base, (ptrdiff_t) pitch, (int) cam.width, (int) cam.height, dt);
            if (locked2d) b2->Unlock2D(); else buf->Unlock();

            if (found)
            {
                outYaw = fa.yaw;
                outPitch = fa.pitch;
                lastFace = t;
            }
            if (t - lastFace > 1.0)
            {
                // Lost you (left the desk, looking away): glide back to straight ahead.
                const float step = 60.0f * dt;
                outYaw -= std::clamp (outYaw, -step, step);
                outPitch -= std::clamp (outPitch, -step, step);
                fa.resetFilters();
                setStatus ("no-face", "Looking for your face");
            }
            else
            {
                setStatus ("tracking");
            }

            const float y = flip ? -outYaw : outYaw;
            pose.yaw.store (y, std::memory_order_relaxed);
            pose.pitch.store (outPitch, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> g (statusLock);
                current.yaw = y;
                current.pitch = outPitch;
                current.fps = fps;
            }
        }
        pose.yaw = 0.0f;
        pose.pitch = 0.0f;
    }

    pose.active = false;
    MFShutdown();
    CoUninitialize();
}

} // namespace cinemax
