#pragma once

#include "Filters.h"
#include "../core/Params.h"

namespace cinemax::dsp
{
/** Preamp + 10 parametric bands + bass/mid/treble tone controls (stereo).
    All changes glide (no zipper noise, no restart). Disabling glides to flat. */
class Equalizer
{
public:
    static constexpr int kToneFilters = 3;
    static constexpr int kFilters = kNumEqBands + kToneFilters;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setParams (const DspParams& p) noexcept;
    void process (float* left, float* right, int numSamples) noexcept;

private:
    double fs = 48000.0;
    std::array<SmoothedFilter, kFilters> filters;
    Ramp preamp;
    float preampTarget = 1.0f;
};

} // namespace cinemax::dsp
