#include "Pipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace cinemax::android
{
//==============================================================================
int StereoFifo::write (const float* src, int frames) noexcept
{
    const uint64_t w = writeIdx.load (std::memory_order_relaxed);
    const uint64_t r = readIdx.load (std::memory_order_acquire);
    const int space = capacity - (int) (w - r);
    const int n = std::min (frames, std::max (0, space));
    for (int i = 0; i < n; ++i)
    {
        const size_t pos = (size_t) ((w + (uint64_t) i) % (uint64_t) capacity) * 2;
        data[pos] = src[2 * i];
        data[pos + 1] = src[2 * i + 1];
    }
    writeIdx.store (w + (uint64_t) n, std::memory_order_release);
    return frames - n;
}

int StereoFifo::read (float* dst, int maxFrames) noexcept
{
    const uint64_t r = readIdx.load (std::memory_order_relaxed);
    const uint64_t w = writeIdx.load (std::memory_order_acquire);
    const int n = std::min (maxFrames, (int) (w - r));
    for (int i = 0; i < n; ++i)
    {
        const size_t pos = (size_t) ((r + (uint64_t) i) % (uint64_t) capacity) * 2;
        dst[2 * i] = data[pos];
        dst[2 * i + 1] = data[pos + 1];
    }
    readIdx.store (r + (uint64_t) n, std::memory_order_release);
    return n;
}

//==============================================================================
double Pipeline::nowSeconds() noexcept
{
    using namespace std::chrono;
    return duration<double> (steady_clock::now().time_since_epoch()).count();
}

Pipeline::Pipeline (int captureRate, int outputRate)
    : inRate (captureRate), outRate (outputRate)
{
    fifo.prepare (inRate);   // 1 s
    resampler.prepare (2, (double) inRate, (double) outRate, true, 0.6);
    chain.prepare ((double) outRate, dsp::ChannelLayout::fromMask (0x3, 2));
    meters.resetForNewStream ((double) outRate);

    drainBuf.assign ((size_t) inRate / 4 * 2, 0.0f);   // 250 ms
    const int maxRender = 8192;
    planarData.assign ((size_t) maxRender * 2, 0.0f);
    planar[0] = planarData.data();
    planar[1] = planarData.data() + maxRender;
    outL.assign (maxRender, 0.0f);
    outR.assign (maxRender, 0.0f);
}

double Pipeline::secondsSinceSignal() const noexcept
{
    return nowSeconds() - lastSignalTime.load (std::memory_order_relaxed);
}

void Pipeline::push (const float* interleaved, int frames) noexcept
{
    if (frames <= 0) return;
    const double now = nowSeconds();
    lastPacketTime.store (now, std::memory_order_relaxed);

    float peak = 0.0f;
    for (int i = 0; i < frames * 2; ++i)
        peak = std::max (peak, std::abs (interleaved[i]));
    if (peak > 1.0e-4f)   // -80 dBFS: something is really playing
        lastSignalTime.store (now, std::memory_order_relaxed);
    MeterBus::storeMax (inputPeak, peak);

    // Reads that return back to back belong to one delivery from Android: the buffer
    // target must cover the whole delivery, not just one read.
    burstFrames = (now - lastPushTime) < 0.003 ? burstFrames + frames : frames;
    lastPushTime = now;
    if (burstFrames > maxChunkFrames.load (std::memory_order_relaxed))
        maxChunkFrames.store (std::min (burstFrames, inRate / 5), std::memory_order_relaxed);

    const int dropped = fifo.write (interleaved, frames);
    if (dropped > 0)
        droppedFrames.fetch_add ((uint32_t) dropped, std::memory_order_relaxed);
}

void Pipeline::render (float* out, int frames) noexcept
{
    frames = std::min (frames, (int) outL.size());

    ProcessingParams p;
    if (paramsIn.read (p))
        chain.setParams (p);

    if (resyncRequested.exchange (false, std::memory_order_acq_rel))
    {
        // Output restarts after a pause: drop stale silence but keep the newest 60 ms,
        // which hold the start of the sound that woke us up.
        fifo.keepNewest (inRate * 60 / 1000);
        resampler.reset();
        state = State::Priming;
    }

    // Target buffer: half a capture delivery + one render period + a jitter margin that
    // grows by 4 ms after every dropout (e.g. the phone slows down with the screen off)
    // and shrinks back by 1 ms per 20 s of clean playback.
    const double chunk = (double) maxChunkFrames.load (std::memory_order_relaxed);
    const double newTarget = 0.5 * chunk + (double) frames * inRate / outRate + marginMs * 0.001 * inRate;
    if (std::abs (newTarget - targetFill) > 1.0)
    {
        targetFill = newTarget;
        maxFill = targetFill + 0.08 * inRate;
        if (state == State::Priming) fillSmooth = targetFill;
    }

    // ---- move captured audio into the resampler -----------------------------------
    for (;;)
    {
        const int n = fifo.read (drainBuf.data(), (int) drainBuf.size() / 2);
        if (n <= 0) break;
        const int lost = resampler.pushInterleaved (drainBuf.data(), n, 2);
        if (lost > 0) droppedFrames.fetch_add ((uint32_t) lost, std::memory_order_relaxed);
    }

    const double now = nowSeconds();
    const bool streamFlowing = (now - lastPacketTime.load (std::memory_order_relaxed)) < 0.1;
    const double fillBefore = resampler.bufferedFrames();
    int produced = 0;

    if (state == State::Priming && fillBefore >= targetFill)
    {
        state = State::Running;
        fillSmooth = fillBefore;
        integ = 0.0;
        fadeInRemaining = 64;
    }

    if (state == State::Running)
    {
        produced = resampler.produce (planar, frames);
        cleanSeconds += (double) frames / outRate;
        if (cleanSeconds > 20.0 && marginMs > 6.0) { marginMs -= 1.0; cleanSeconds = 0.0; }
        if (produced < frames)
        {
            if (streamFlowing)
            {
                meters.xruns.fetch_add (1, std::memory_order_relaxed);
                marginMs = std::min (40.0, marginMs + 4.0);
                cleanSeconds = 0.0;
            }
            state = State::Priming;
            const int fade = std::min (64, produced);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < fade; ++i)
                    planar[c][produced - fade + i] *= 1.0f - (float) (i + 1) / (float) fade;
        }
        if (fadeInRemaining > 0)
        {
            const int fade = std::min (fadeInRemaining, produced);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < fade; ++i)
                    planar[c][i] *= (float) (64 - fadeInRemaining + i + 1) / 64.0f;
            fadeInRemaining -= fade;
        }
    }
    for (int c = 0; c < 2; ++c)
        std::fill (planar[c] + produced, planar[c] + frames, 0.0f);

    // ---- the CINEMAX DSP chain (identical to Windows) -------------------------------
    const double t0 = nowSeconds();
    chain.process (planar, frames, 1.0f, outL.data(), outR.data(), &meters);
    const double dspTime = nowSeconds() - t0;

    float pk = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        out[2 * i] = outL[(size_t) i];
        out[2 * i + 1] = outR[(size_t) i];
        pk = std::max (pk, std::max (std::abs (outL[(size_t) i]), std::abs (outR[(size_t) i])));
    }
    MeterBus::storeMax (outputPeak, pk);

    // ---- drift control (PI on the smoothed buffer level) -----------------------------
    const double fill = 0.5 * (fillBefore + resampler.bufferedFrames());
    const double dt = (double) frames / outRate;
    if (state == State::Running)
    {
        if (fill > maxFill)
        {
            // Output stalled (e.g. Bluetooth hiccup): drop the excess so the delay
            // returns to normal instead of lagging behind for good.
            resampler.discard ((int) (fill - targetFill));
            fillSmooth = targetFill;
            meters.xruns.fetch_add (1, std::memory_order_relaxed);
        }
        else
        {
            fillSmooth += (fill - fillSmooth) * std::min (1.0, dt / 0.5);
            const double errMs = (fillSmooth - targetFill) * 1000.0 / inRate;
            integ = std::clamp (integ + errMs * dt, -400.0, 400.0);
            const double ppm = std::clamp (30.0 * errMs + 3.0 * integ, -2000.0, 2000.0);
            resampler.setDriftPpm (ppm);
        }
    }

    cpuSmooth += ((dspTime / std::max (1.0e-6, dt)) * 100.0 - cpuSmooth) * 0.05;
    meters.cpuPercent.store ((float) cpuSmooth, std::memory_order_relaxed);
    fillMsOut.store ((float) (fill * 1000.0 / inRate), std::memory_order_relaxed);
    meters.driftPpm.store ((float) resampler.getDriftPpm(), std::memory_order_relaxed);
}

void Pipeline::readStats (PipelineStats& s) noexcept
{
    s.fillMs = fillMsOut.load (std::memory_order_relaxed);
    s.driftPpm = meters.driftPpm.load (std::memory_order_relaxed);
    s.xruns = (float) meters.xruns.load (std::memory_order_relaxed);
    s.inputPeak = inputPeak.exchange (0.0f, std::memory_order_relaxed);
    s.outputPeak = outputPeak.exchange (0.0f, std::memory_order_relaxed);
    s.movieProbability = meters.movieProbability.load (std::memory_order_relaxed);
    s.confidence = meters.contentConfidence.load (std::memory_order_relaxed);
    s.smartBassDb = meters.smartBassDb.load (std::memory_order_relaxed);
    s.smartTrebleDb = meters.smartTrebleDb.load (std::memory_order_relaxed);
    s.cpuPercent = meters.cpuPercent.load (std::memory_order_relaxed);
    s.secondsSinceSignal = (float) std::min (1.0e6, secondsSinceSignal());
    s.limiterGrDb = meters.limiterGrDb.exchange (0.0f, std::memory_order_relaxed);
    s.chunkMs = (float) (maxChunkFrames.load (std::memory_order_relaxed) * 1000.0 / inRate);
    s.dropped = (float) droppedFrames.load (std::memory_order_relaxed);
}
} // namespace cinemax::android
