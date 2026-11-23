#pragma once

#include "Filters.h"
#include "../core/Params.h"

namespace cinemax::dsp
{
/** Smart EQ (in the style of Dolby's "Intelligent Equalizer"): measures the long-term tonal
    balance of the programme in 10 octave bands and slowly steers it toward a target curve
    (Balanced / Warm / Detailed). It mostly lifts what the material lacks and cuts gently,
    so thin recordings get body and dull ones get air, while well-balanced ones barely change.

    - analysis: octave band-pass filters on the mono sum, ~2 s energy followers, gated so
      silence and very quiet passages never drive it
    - the measured curve is aligned to the target over 250 Hz - 4 kHz (overall level does
      not matter; the leveller handles loudness)
    - corrections are clamped (+5 / -4 dB, +-3 dB at the extremes) and glide over seconds,
      then applied with 10 peaking filters (stereo)
    Disabled = transparent (filters glide to flat). Real-time safe. */
class IntelligentEq
{
public:
    static constexpr int kBands = kNumEqBands;

    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setParams (const DspParams& p) noexcept;

    /** Measure the material (call with the signal *before* the user's EQ). */
    void analyse (const float* left, const float* right, int numSamples) noexcept;
    /** Apply the current correction. */
    void process (float* left, float* right, int numSamples) noexcept;

    float correctionDb (int band) const noexcept { return appliedDb[(size_t) band]; }

    /** Target curves (dB per octave band, 31.5 Hz .. 16 kHz). */
    static const std::array<float, kBands>& target (int profile) noexcept;

private:
    void updateCorrection (int numSamples) noexcept;

    double fs = 48000.0;
    bool enabled = false;
    int profile = 0;
    float amount = 0.6f;

    std::array<Biquad, kBands> bandFilters;
    std::array<double, kBands> bandEnergy {};
    double totalEnergy = 0.0;
    double energyCoeff = 0.0;
    bool primed = false;
    int sinceUpdate = 0;

    std::array<float, kBands> wantedDb {}, appliedDb {};
    std::array<SmoothedFilter, kBands> filters;
};

} // namespace cinemax::dsp
