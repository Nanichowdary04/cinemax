#pragma once

#include <vector>

namespace cinemax::dsp
{
/** Variable-ratio multichannel resampler (Kaiser-windowed sinc, 256 interpolated
    phases). Converts the capture rate to the output rate AND absorbs the small
    clock drift between two independent audio devices: the ratio is nudged in ppm
    by the pipeline's buffer-level controller, so the FIFO never runs dry or
    overflows (no periodic clicks).

    Owns its input FIFO. Single-threaded: push and produce happen on the audio
    thread. No allocation after prepare(). */
class Resampler
{
public:
    void prepare (int numChannels, double inRate, double outRate, bool highQuality, double capacitySeconds = 0.5);
    void reset() noexcept;

    void setDriftPpm (double ppm) noexcept;
    double getDriftPpm() const noexcept { return driftPpm; }

    /** Appends interleaved frames (srcChannels may differ: extra ignored, missing = 0).
        Returns the number of frames that did not fit (dropped, newest kept). */
    int pushInterleaved (const float* data, int frames, int srcChannels) noexcept;
    int pushSilence (int frames) noexcept;

    /** Input frames available beyond the interpolation history (fractional). */
    double bufferedFrames() const noexcept;

    /** Drops 'frames' of the oldest buffered input (resynchronisation). */
    void discard (int frames) noexcept;

    bool canProduce (int outFrames) const noexcept;

    /** Produces up to outFrames planar frames; returns how many were produced. */
    int produce (float* const* out, int outFrames) noexcept;

    int numChannels() const noexcept { return channels; }
    double inputRate() const noexcept { return inRate; }
    double outputRate() const noexcept { return outRate; }
    int latencyFrames() const noexcept { return half; }

private:
    void compact() noexcept;

    int channels = 2;
    double inRate = 48000.0, outRate = 48000.0;
    double baseStep = 1.0, step = 1.0, driftPpm = 0.0;
    int taps = 16, half = 8;
    static constexpr int kPhases = 256;
    std::vector<float> table;        // (kPhases + 1) * taps
    std::vector<float> buffer;       // channels * capacity, planar
    int capacity = 0;
    int writePos = 0;                // next write index
    double readPos = 0.0;            // centre position of the next output sample
    std::vector<float> kernel;       // taps (scratch)
};

} // namespace cinemax::dsp
