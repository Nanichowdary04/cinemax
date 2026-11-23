// Phone audio path for the full CINEMAX engine:
//
//   other apps' audio (captured by Android, pre-volume, pre-effects)
//     -> capture thread: push() into a lock-free FIFO
//     -> render thread:  drift-compensating resampler -> DspChain (same code as the
//                        Windows app) -> AudioTrack
//
// The capture clock (Android's internal submix) and the output clock (speaker /
// Bluetooth) are independent; a PI controller nudges the resampler ratio in ppm so
// the buffer stays at its target (no periodic clicks, no growing delay). This is the
// same controller as the Windows WASAPI pipeline.
#pragma once

#include "cinemax/core/Metering.h"
#include "cinemax/core/Params.h"
#include "cinemax/dsp/DspChain.h"
#include "cinemax/dsp/Resampler.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace cinemax::android
{
/** Single-producer / single-consumer ring of interleaved stereo frames. */
class StereoFifo
{
public:
    void prepare (int capacityFrames)
    {
        data.assign ((size_t) capacityFrames * 2, 0.0f);
        capacity = capacityFrames;
        writeIdx.store (0);
        readIdx.store (0);
    }

    /** Producer. Returns frames dropped because the consumer fell behind. */
    int write (const float* src, int frames) noexcept;
    /** Consumer. Returns frames read. */
    int read (float* dst, int maxFrames) noexcept;
    /** Consumer: forget everything buffered except the newest 'keepFrames'. */
    void keepNewest (int keepFrames) noexcept
    {
        const uint64_t w = writeIdx.load (std::memory_order_acquire);
        const uint64_t r = readIdx.load (std::memory_order_relaxed);
        if (w - r > (uint64_t) keepFrames) readIdx.store (w - (uint64_t) keepFrames, std::memory_order_release);
    }
    int available() const noexcept
    {
        return (int) (writeIdx.load (std::memory_order_acquire) - readIdx.load (std::memory_order_acquire));
    }

private:
    std::vector<float> data;
    int capacity = 0;
    std::atomic<uint64_t> writeIdx { 0 }, readIdx { 0 };
};

struct PipelineStats
{
    float fillMs, driftPpm, xruns, inputPeak, outputPeak, movieProbability, confidence,
          smartBassDb, smartTrebleDb, cpuPercent, secondsSinceSignal, limiterGrDb, chunkMs, dropped;
    static constexpr int kCount = 14;
};

class Pipeline
{
public:
    Pipeline (int captureRate, int outputRate);

    /** Capture thread: interleaved stereo float frames from AudioRecord. */
    void push (const float* interleaved, int frames) noexcept;

    /** Render thread: fills 'frames' interleaved stereo frames for AudioTrack. */
    void render (float* interleaved, int frames) noexcept;

    /** Render thread: drop everything buffered and start priming again (after a pause). */
    void resync() noexcept { resyncRequested.store (true, std::memory_order_release); }

    /** Any thread: new parameters (picked up at the next render call). */
    void setParams (const ProcessingParams& p) noexcept { paramsIn.write (p); }

    /** Before rendering starts: the head pose the 3D sound follows (front-camera tracker). */
    void setHeadPose (const HeadPose* pose) noexcept { chain.setHeadPose (pose); }

    /** Any thread. */
    void readStats (PipelineStats& s) noexcept;
    double secondsSinceSignal() const noexcept;

private:
    static double nowSeconds() noexcept;

    const int inRate, outRate;
    StereoFifo fifo;
    dsp::Resampler resampler;
    dsp::DspChain chain;
    TripleBuffer<ProcessingParams> paramsIn;
    MeterBus meters;

    std::vector<float> drainBuf, planarData, outL, outR;
    float* planar[2] {};

    enum class State { Priming, Running } state = State::Priming;
    double targetFill = 0, maxFill = 0, fillSmooth = 0, integ = 0, cpuSmooth = 0;
    double marginMs = 6.0;          // scheduling-jitter margin; grows after a dropout
    double cleanSeconds = 0.0;      // time since the last dropout
    int fadeInRemaining = 0;

    std::atomic<bool> resyncRequested { false };
    std::atomic<double> lastSignalTime { -1.0e9 }, lastPacketTime { -1.0e9 };
    std::atomic<int> maxChunkFrames { 480 };
    double lastPushTime = 0.0;   // capture thread only
    int burstFrames = 0;         // capture thread only
    std::atomic<float> inputPeak { 0 }, outputPeak { 0 }, fillMsOut { 0 };
    std::atomic<uint32_t> droppedFrames { 0 };
};
} // namespace cinemax::android
