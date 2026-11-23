#include "WasapiPipeline.h"
#include "../dsp/DspChain.h"
#include "../dsp/Resampler.h"

#include "cinemax/win/Win32Common.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <future>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace cinemax::audio
{
namespace
{
    // All CINEMAX render streams share one audio session (one entry in the volume mixer).
    constexpr GUID kSessionGuid = { 0x7b5d2c8e, 0x3a41, 0x4c8f, { 0x9e, 0x2d, 0x6c, 0x1f, 0x0a, 0x9b, 0x4e, 0x17 } };

    enum class SampleType { Float32, Int16, Int24, Int32, Unsupported };

    struct StreamFormat
    {
        int channels = 0;
        int rate = 0;
        unsigned mask = 0;
        int blockAlign = 0;
        SampleType type = SampleType::Unsupported;
    };

    StreamFormat describe (const WAVEFORMATEX* wf)
    {
        StreamFormat f;
        f.channels = wf->nChannels;
        f.rate = (int) wf->nSamplesPerSec;
        f.blockAlign = wf->nBlockAlign;

        bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        bool isPcm = wf->wFormatTag == WAVE_FORMAT_PCM;

        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22)
        {
            auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*> (wf);
            f.mask = ext->dwChannelMask;
            isFloat = IsEqualGUID (ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
            isPcm = IsEqualGUID (ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != 0;
        }

        if (isFloat && wf->wBitsPerSample == 32)            f.type = SampleType::Float32;
        else if (isPcm && wf->wBitsPerSample == 16)         f.type = SampleType::Int16;
        else if (isPcm && wf->wBitsPerSample == 24)         f.type = SampleType::Int24;
        else if (isPcm && wf->wBitsPerSample == 32)         f.type = SampleType::Int32;

        return f;
    }

    /** Converts device samples to interleaved float. */
    void toFloat (const BYTE* src, float* dst, int frames, const StreamFormat& f) noexcept
    {
        const int count = frames * f.channels;
        switch (f.type)
        {
            case SampleType::Float32:
                std::memcpy (dst, src, sizeof (float) * (size_t) count);
                break;
            case SampleType::Int16:
            {
                auto* s = reinterpret_cast<const int16_t*> (src);
                for (int i = 0; i < count; ++i) dst[i] = (float) s[i] * (1.0f / 32768.0f);
                break;
            }
            case SampleType::Int24:
                for (int i = 0; i < count; ++i)
                {
                    const BYTE* p = src + (size_t) i * 3;
                    const int32_t v = (int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24) >> 8;
                    dst[i] = (float) v * (1.0f / 8388608.0f);
                }
                break;
            case SampleType::Int32:
            {
                auto* s = reinterpret_cast<const int32_t*> (src);
                for (int i = 0; i < count; ++i) dst[i] = (float) ((double) s[i] * (1.0 / 2147483648.0));
                break;
            }
            case SampleType::Unsupported:
                std::memset (dst, 0, sizeof (float) * (size_t) count);
                break;
        }
    }

    inline float sat (float x) noexcept { return x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x); }

    /** Writes stereo float to the device buffer (mono devices get the average). */
    void fromStereo (const float* l, const float* r, BYTE* dst, int frames, const StreamFormat& f) noexcept
    {
        const int ch = f.channels;
        for (int i = 0; i < frames; ++i)
        {
            for (int c = 0; c < ch; ++c)
            {
                float v;
                if (ch == 1)       v = 0.5f * (l[i] + r[i]);
                else if (c == 0)   v = l[i];
                else if (c == 1)   v = r[i];
                else               v = 0.0f;

                const size_t idx = (size_t) i * (size_t) ch + (size_t) c;
                switch (f.type)
                {
                    case SampleType::Float32: reinterpret_cast<float*> (dst)[idx] = v; break;
                    case SampleType::Int16:   reinterpret_cast<int16_t*> (dst)[idx] = (int16_t) std::lround (sat (v) * 32767.0f); break;
                    case SampleType::Int24:
                    {
                        const int32_t s = (int32_t) std::lround ((double) sat (v) * 8388607.0);
                        BYTE* p = dst + idx * 3;
                        p[0] = (BYTE) (s & 0xff); p[1] = (BYTE) ((s >> 8) & 0xff); p[2] = (BYTE) ((s >> 16) & 0xff);
                        break;
                    }
                    case SampleType::Int32:   reinterpret_cast<int32_t*> (dst)[idx] = (int32_t) std::llround ((double) sat (v) * 2147483647.0); break;
                    case SampleType::Unsupported: break;
                }
            }
        }
    }

    double nowSeconds() noexcept
    {
        static const double freq = []
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency (&f);
            return (double) f.QuadPart;
        }();
        LARGE_INTEGER c;
        QueryPerformanceCounter (&c);
        return (double) c.QuadPart / freq;
    }
}

//==============================================================================
struct WasapiPipeline::Impl
{
    const HeadPose* headPose = nullptr;

    Impl (TripleBuffer<ProcessingParams>& p, MeterBus& m, std::atomic<float>& g, std::atomic<float>* d)
        : paramsIn (p), meters (m), inputGain (g), deviceVolumeDb (d)
    {
        // (head pose pointer is set through WasapiPipeline::setHeadPose)
        stopEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);
        renderEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    }

    ~Impl()
    {
        CloseHandle (stopEvent);
        CloseHandle (renderEvent);
    }

    TripleBuffer<ProcessingParams>& paramsIn;
    MeterBus& meters;
    std::atomic<float>& inputGain;
    std::atomic<float>* deviceVolumeDb = nullptr;

    HANDLE stopEvent = nullptr, renderEvent = nullptr;
    std::thread thread;
    std::atomic<bool> running { false };

    mutable std::mutex infoLock;
    PipelineConfig cfg;
    PipelineFormat fmt;
    ProcessingParams initialParams;

    std::function<void (long, std::string)>* errorCallback = nullptr;

    // --- audio-thread state -----------------------------------------------------------
    ComPtr<IAudioClient> captureClient, renderClient;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioRenderClient> render;
    StreamFormat inFmt, outFmt;
    UINT32 renderBufferFrames = 0;

    dsp::DspChain chain;
    dsp::Resampler resampler;
    std::vector<float> convertBuf;                 // interleaved capture -> float
    std::vector<float> planarData;                 // resampled input, planar
    float* planar[dsp::kMaxInputChannels] {};
    std::vector<float> outL, outR;

    enum class State { Priming, Running } state = State::Priming;
    double targetFill = 0.0, maxFill = 0.0, fillSmooth = 0.0, integ = 0.0;
    double lastPacketTime = 0.0, lastSignalTime = -10.0;
    double cpuSmooth = 0.0;
    int fadeInRemaining = 0;
    int timeouts = 0;

    //==================================================================================
    StartResult initialise()
    {
        StartResult r;
        auto fail = [&r] (HRESULT hr, const char* what)
        {
            r.ok = false;
            r.hr = hr;
            r.message = std::string (what) + ": " + win::hresultToString (hr);
            return r;
        };

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator));
        if (FAILED (hr)) return fail (hr, "Cannot create device enumerator");

        ComPtr<IMMDevice> srcDevice, outDevice;
        hr = enumerator->GetDevice (cfg.sourceId.c_str(), &srcDevice);
        if (FAILED (hr)) return fail (hr, "Virtual source device not found");
        hr = enumerator->GetDevice (cfg.outputId.c_str(), &outDevice);
        if (FAILED (hr)) return fail (hr, "Output device not found");

        // ---- loopback capture on the virtual device ----------------------------------
        hr = srcDevice->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**> (captureClient.GetAddressOf()));
        if (FAILED (hr)) return fail (hr, "Cannot open virtual device");

        WAVEFORMATEX* capFmt = nullptr;
        hr = captureClient->GetMixFormat (&capFmt);
        if (FAILED (hr)) return fail (hr, "Cannot read virtual device format");
        inFmt = describe (capFmt);

        hr = captureClient->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                        2'000'000 /* 200 ms */, 0, capFmt, nullptr);
        CoTaskMemFree (capFmt);
        if (FAILED (hr)) return fail (hr, "Cannot start loopback capture");
        if (inFmt.type == SampleType::Unsupported) return fail (E_NOTIMPL, "Unsupported virtual device sample format");

        hr = captureClient->GetService (IID_PPV_ARGS (&capture));
        if (FAILED (hr)) return fail (hr, "Cannot get capture service");

        // ---- event-driven shared render on the real device ---------------------------
        hr = outDevice->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**> (renderClient.GetAddressOf()));
        if (FAILED (hr)) return fail (hr, "Cannot open output device");

        WAVEFORMATEX* outWf = nullptr;
        hr = renderClient->GetMixFormat (&outWf);
        if (FAILED (hr)) return fail (hr, "Cannot read output device format");
        outFmt = describe (outWf);

        const REFERENCE_TIME requested = (REFERENCE_TIME) std::clamp (cfg.bufferMs, 10, 200) * 10'000;
        hr = renderClient->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                       requested, 0, outWf, &kSessionGuid);
        CoTaskMemFree (outWf);
        if (FAILED (hr)) return fail (hr, "Cannot start output stream");
        if (outFmt.type == SampleType::Unsupported) return fail (E_NOTIMPL, "Unsupported output sample format");

        hr = renderClient->SetEventHandle (renderEvent);
        if (FAILED (hr)) return fail (hr, "Cannot set render event");
        hr = renderClient->GetBufferSize (&renderBufferFrames);
        if (FAILED (hr)) return fail (hr, "Cannot query render buffer");
        hr = renderClient->GetService (IID_PPV_ARGS (&render));
        if (FAILED (hr)) return fail (hr, "Cannot get render service");

        {
            ComPtr<IAudioSessionControl> session;
            if (SUCCEEDED (renderClient->GetService (IID_PPV_ARGS (&session))))
                session->SetDisplayName (L"CINEMAX", nullptr);
        }

        REFERENCE_TIME streamLatency = 0;
        renderClient->GetStreamLatency (&streamLatency);

        // ---- DSP + buffers (all allocation happens here, before the loop) ------------
        const int procChannels = std::min (inFmt.channels, dsp::kMaxInputChannels);
        const auto layout = dsp::ChannelLayout::fromMask (inFmt.mask, procChannels);

        chain.prepare ((double) outFmt.rate, layout);
        chain.setHeadPose (headPose);
        chain.setParams (initialParams);

        resampler.prepare (procChannels, (double) inFmt.rate, (double) outFmt.rate, cfg.highQualityResampling, 0.6);

        convertBuf.assign ((size_t) inFmt.rate / 2 * (size_t) inFmt.channels, 0.0f);    // 500 ms
        planarData.assign ((size_t) procChannels * (size_t) renderBufferFrames, 0.0f);
        for (int c = 0; c < procChannels; ++c)
            planar[c] = planarData.data() + (size_t) c * (size_t) renderBufferFrames;
        outL.assign (renderBufferFrames, 0.0f);
        outR.assign (renderBufferFrames, 0.0f);

        const double fifoTargetMs = std::max (8.0, cfg.bufferMs * 0.5 + 5.0);
        targetFill = fifoTargetMs * 0.001 * inFmt.rate;
        maxFill = targetFill + std::max (0.08, fifoTargetMs * 0.004) * inFmt.rate;
        fillSmooth = targetFill;
        integ = 0.0;
        state = State::Priming;
        timeouts = 0;

        meters.resetForNewStream ((double) outFmt.rate);

        // Pre-roll silence so the device has data before the first event.
        BYTE* data = nullptr;
        if (SUCCEEDED (render->GetBuffer (renderBufferFrames, &data)))
            render->ReleaseBuffer (renderBufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);

        hr = captureClient->Start();
        if (FAILED (hr)) return fail (hr, "Cannot start capture");
        hr = renderClient->Start();
        if (FAILED (hr)) return fail (hr, "Cannot start render");

        {
            std::lock_guard<std::mutex> lock (infoLock);
            fmt.inputRate = inFmt.rate;
            fmt.outputRate = outFmt.rate;
            fmt.inputChannels = inFmt.channels;
            fmt.outputChannels = outFmt.channels;
            fmt.inputMask = inFmt.mask;
            fmt.bufferMs = 1000.0 * renderBufferFrames / outFmt.rate;
            fmt.latencyMs = fifoTargetMs + fmt.bufferMs + streamLatency / 10'000.0
                          + 1000.0 * chain.latencySamples() / outFmt.rate
                          + 1000.0 * resampler.latencyFrames() / inFmt.rate;
        }

        r.ok = true;
        return r;
    }

    void release()
    {
        if (captureClient) captureClient->Stop();
        if (renderClient) renderClient->Stop();
        capture.Reset();
        render.Reset();
        captureClient.Reset();
        renderClient.Reset();
    }

    //==================================================================================
    HRESULT serviceCapture() noexcept
    {
        for (;;)
        {
            UINT32 packet = 0;
            HRESULT hr = capture->GetNextPacketSize (&packet);
            if (FAILED (hr)) return hr;
            if (packet == 0) return S_OK;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer (&data, &frames, &flags, nullptr, nullptr);
            if (FAILED (hr)) return hr;

            const double now = nowSeconds();
            lastPacketTime = now;

            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
            {
                resampler.pushSilence ((int) frames);
            }
            else
            {
                const int maxFrames = (int) (convertBuf.size() / (size_t) inFmt.channels);
                int done = 0;
                while (done < (int) frames)
                {
                    const int n = std::min (maxFrames, (int) frames - done);
                    toFloat (data + (size_t) done * (size_t) inFmt.blockAlign, convertBuf.data(), n, inFmt);

                    float peak = 0.0f;
                    for (int i = 0; i < n * inFmt.channels; ++i)
                        peak = std::max (peak, std::abs (convertBuf[(size_t) i]));
                    if (peak > 3.0e-5f)   // above ~ -90 dBFS: real signal is flowing
                        lastSignalTime = now;

                    if (resampler.pushInterleaved (convertBuf.data(), n, inFmt.channels) > 0)
                        meters.xruns.fetch_add (1, std::memory_order_relaxed);
                    done += n;
                }
            }

            hr = capture->ReleaseBuffer (frames);
            if (FAILED (hr)) return hr;
        }
    }

    HRESULT serviceRender() noexcept
    {
        UINT32 padding = 0;
        HRESULT hr = renderClient->GetCurrentPadding (&padding);
        if (FAILED (hr)) return hr;

        const int avail = (int) (renderBufferFrames - padding);
        if (avail <= 0)
            return S_OK;

        ProcessingParams p;
        if (paramsIn.read (p))
            chain.setParams (p);

        const double now = nowSeconds();
        const bool streamFlowing = (now - lastPacketTime) < 0.05;
        const int procCh = resampler.numChannels();

        // ---- pull input through the drift-compensating resampler ---------------------
        const double fillBefore = resampler.bufferedFrames();
        int produced = 0;

        if (state == State::Priming && resampler.bufferedFrames() >= targetFill)
        {
            state = State::Running;
            fillSmooth = resampler.bufferedFrames();
            fadeInRemaining = 64;
        }

        if (state == State::Running)
        {
            produced = resampler.produce (planar, avail);

            if (produced < avail)
            {
                // Ran dry. If the source is still delivering packets this is a real
                // underrun (count it); otherwise playback simply stopped.
                if (streamFlowing)
                    meters.xruns.fetch_add (1, std::memory_order_relaxed);
                state = State::Priming;

                const int fade = std::min (64, produced);
                for (int c = 0; c < procCh; ++c)
                    for (int i = 0; i < fade; ++i)
                        planar[c][produced - fade + i] *= 1.0f - (float) (i + 1) / (float) fade;
            }

            if (fadeInRemaining > 0)
            {
                const int fade = std::min (fadeInRemaining, produced);
                for (int c = 0; c < procCh; ++c)
                    for (int i = 0; i < fade; ++i)
                        planar[c][i] *= (float) (64 - fadeInRemaining + i + 1) / 64.0f;
                fadeInRemaining -= fade;
            }
        }

        for (int c = 0; c < procCh; ++c)
            std::fill (planar[c] + produced, planar[c] + avail, 0.0f);

        // ---- DSP ---------------------------------------------------------------------
        const double t0 = nowSeconds();
        if (deviceVolumeDb != nullptr)
            chain.setDeviceVolumeDb (deviceVolumeDb->load (std::memory_order_relaxed));
        chain.process (planar, avail, inputGain.load (std::memory_order_relaxed), outL.data(), outR.data(), &meters);
        const double dspTime = nowSeconds() - t0;

        BYTE* out = nullptr;
        hr = render->GetBuffer ((UINT32) avail, &out);
        if (FAILED (hr)) return hr;
        if (cfg.muteOutput)
        {
            hr = render->ReleaseBuffer ((UINT32) avail, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        else
        {
            fromStereo (outL.data(), outR.data(), out, avail, outFmt);
            hr = render->ReleaseBuffer ((UINT32) avail, 0);
        }
        if (FAILED (hr)) return hr;

        // ---- drift control (PI on the smoothed FIFO level) -----------------------------
        // Use the mean of the level before and after consuming this period, so the
        // controller targets the average occupancy instead of the post-read minimum.
        const double fill = 0.5 * (fillBefore + resampler.bufferedFrames());
        const double dt = (double) avail / outFmt.rate;

        if (state == State::Running)
        {
            if (fill > maxFill)
            {
                // The output device stalled (e.g. Bluetooth hiccup): drop the excess to
                // get latency back down instead of lagging behind forever.
                resampler.discard ((int) (fill - targetFill));
                fillSmooth = targetFill;
                meters.xruns.fetch_add (1, std::memory_order_relaxed);
            }
            else
            {
                fillSmooth += (fill - fillSmooth) * std::min (1.0, dt / 0.5);
                const double errMs = (fillSmooth - targetFill) * 1000.0 / inFmt.rate;
                integ = std::clamp (integ + errMs * dt, -400.0, 400.0);
                const double ppm = std::clamp (30.0 * errMs + 3.0 * integ, -2000.0, 2000.0);
                resampler.setDriftPpm (ppm);
            }
        }

        // ---- stats -----------------------------------------------------------------------
        cpuSmooth += ((dspTime / std::max (1.0e-6, dt)) * 100.0 - cpuSmooth) * 0.05;
        meters.cpuPercent.store ((float) cpuSmooth, std::memory_order_relaxed);
        meters.bufferFillMs.store ((float) (fill * 1000.0 / inFmt.rate), std::memory_order_relaxed);
        meters.driftPpm.store ((float) resampler.getDriftPpm(), std::memory_order_relaxed);
        meters.active.store ((now - lastSignalTime) < 0.3, std::memory_order_relaxed);

        return S_OK;
    }

    /** Returns S_OK when stopped normally, otherwise the failing HRESULT. */
    HRESULT runLoop() noexcept
    {
        HANDLE waits[2] = { stopEvent, renderEvent };

        for (;;)
        {
            const DWORD w = WaitForMultipleObjects (2, waits, FALSE, 200);

            if (w == WAIT_OBJECT_0)
                return S_OK;

            if (w == WAIT_TIMEOUT)
            {
                // No render event for 200 ms. Allow a few (device sleeping / BT
                // renegotiation) before declaring the output dead.
                if (++timeouts >= 15)
                    return AUDCLNT_E_DEVICE_INVALIDATED;
            }
            else
            {
                timeouts = 0;
            }

            HRESULT hr = serviceCapture();
            if (FAILED (hr)) return hr;

            hr = serviceRender();
            if (FAILED (hr)) return hr;
        }
    }

    void threadMain (std::promise<StartResult> ready)
    {
        win::ComScope com (COINIT_MULTITHREADED);

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);
        if (mmcss != nullptr)
            AvSetMmThreadPriority (mmcss, AVRT_PRIORITY_HIGH);

        StartResult result = initialise();
        const bool ok = result.ok;
        if (! ok)
            release();

        ready.set_value (result);

        if (ok)
        {
            const HRESULT hr = runLoop();
            release();

            if (hr != S_OK)
            {
                running = false;
                const std::string msg = "Audio stream stopped: " + win::hresultToString (hr);
                win::log::warn ("%s", msg.c_str());
                if (errorCallback != nullptr && *errorCallback)
                    (*errorCallback) ((long) hr, msg);
            }
        }

        if (mmcss != nullptr)
            AvRevertMmThreadCharacteristics (mmcss);

        running = false;
    }
};

//==============================================================================
WasapiPipeline::WasapiPipeline (TripleBuffer<ProcessingParams>& params, MeterBus& meters, std::atomic<float>& inputGain,
                                std::atomic<float>* deviceVolumeDb)
    : impl (std::make_unique<Impl> (params, meters, inputGain, deviceVolumeDb))
{
    impl->errorCallback = &onStreamError;
}

WasapiPipeline::~WasapiPipeline()
{
    stop();
}

StartResult WasapiPipeline::start (const PipelineConfig& config, const ProcessingParams& initialParams)
{
    stop();

    {
        std::lock_guard<std::mutex> lock (impl->infoLock);
        impl->cfg = config;
        impl->fmt = {};
    }
    impl->initialParams = initialParams;
    ResetEvent (impl->stopEvent);

    std::promise<StartResult> ready;
    auto future = ready.get_future();

    impl->running = true;
    impl->thread = std::thread ([this, p = std::move (ready)]() mutable { impl->threadMain (std::move (p)); });

    if (future.wait_for (std::chrono::seconds (6)) != std::future_status::ready)
    {
        // Initialisation hung (driver issue). Ask the thread to stop and report failure.
        SetEvent (impl->stopEvent);
        if (impl->thread.joinable())
            impl->thread.join();
        impl->running = false;
        return { false, (long) E_FAIL, "Audio device did not respond while starting" };
    }

    StartResult r = future.get();
    if (! r.ok)
    {
        if (impl->thread.joinable())
            impl->thread.join();
        impl->running = false;
    }
    return r;
}

void WasapiPipeline::stop()
{
    if (impl->thread.joinable())
    {
        SetEvent (impl->stopEvent);
        impl->thread.join();
    }
    impl->running = false;
}

void WasapiPipeline::setHeadPose (const HeadPose* pose) noexcept
{
    impl->headPose = pose;
}

bool WasapiPipeline::isRunning() const noexcept
{
    return impl->running.load();
}

PipelineFormat WasapiPipeline::format() const
{
    std::lock_guard<std::mutex> lock (impl->infoLock);
    return impl->fmt;
}

PipelineConfig WasapiPipeline::config() const
{
    std::lock_guard<std::mutex> lock (impl->infoLock);
    return impl->cfg;
}

} // namespace cinemax::audio
