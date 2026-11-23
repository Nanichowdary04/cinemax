// System-wide audio path:
//   Windows apps -> virtual render device (default output)
//   -> WASAPI loopback capture -> resampler/drift control -> DSP chain
//   -> WASAPI shared-mode event-driven render -> real output device
#pragma once

#include "../core/Metering.h"
#include "../core/Params.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace cinemax::audio
{
struct PipelineConfig
{
    std::wstring sourceId;          // virtual render endpoint captured via loopback
    std::wstring outputId;          // real render endpoint
    int bufferMs = 20;              // requested render buffer (10 / 20 / 40 / 80)
    bool highQualityResampling = false;
    bool muteOutput = false;        // diagnostics: run the full path but render silence
};

struct PipelineFormat
{
    double inputRate = 0, outputRate = 0;
    int inputChannels = 0, outputChannels = 0;
    unsigned inputMask = 0;
    double bufferMs = 0;            // granted render buffer
    double latencyMs = 0;           // estimated added latency
};

struct StartResult
{
    bool ok = false;
    long hr = 0;                    // HRESULT
    std::string message;
};

class WasapiPipeline
{
public:
    /** deviceVolumeDb (optional): the real output's volume (dB) for the volume-aware tone. */
    WasapiPipeline (TripleBuffer<ProcessingParams>& params, MeterBus& meters, std::atomic<float>& inputGain,
                    std::atomic<float>* deviceVolumeDb = nullptr);
    ~WasapiPipeline();

    /** Starts the audio thread and waits (max ~5 s) until both streams are running. */
    StartResult start (const PipelineConfig& config, const ProcessingParams& initialParams);

    /** Stops and joins the audio thread. Safe to call when not running. */
    void stop();

    bool isRunning() const noexcept;
    PipelineFormat format() const;
    PipelineConfig config() const;

    /** Called on the audio thread *after* its loop exited because a stream failed
        (device removed, Bluetooth dropped, format change...). Not called on stop(). */
    std::function<void (long hr, std::string message)> onStreamError;

    /** Head tracking pose for the spatial stage (owned by the engine; may be null).
        Takes effect the next time the pipeline starts. */
    void setHeadPose (const HeadPose* pose) noexcept;

    WasapiPipeline (const WasapiPipeline&) = delete;
    WasapiPipeline& operator= (const WasapiPipeline&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cinemax::audio
