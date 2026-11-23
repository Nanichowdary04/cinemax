#pragma once

#include "ChannelLayout.h"
#include "Filters.h"
#include "../core/Params.h"

namespace cinemax::dsp
{
/** Dialogue clarity, layout aware.
    - Stereo: mid/side. A voice EQ (de-mud at 250 Hz, clarity at 1.2 kHz, presence
      at 3 kHz) is applied to the mid (where dialogue is mixed); the side signal
      (music/ambience) follows the "background" level. Exact reconstruction when
      neutral.
    - Multichannel with a centre channel: voice EQ on the centre, background level
      on every other channel except LFE.
    - Mono: voice EQ only. */
class DialogueEnhancer
{
public:
    void prepare (double sampleRate, const ChannelLayout& layout) noexcept;
    void reset() noexcept;
    void setParams (const DspParams& p) noexcept;
    void process (float* const* channels, int numSamples) noexcept;

private:
    static constexpr int kVoiceFilters = 3;

    double fs = 48000.0;
    ChannelLayout layout;
    int centreIndex = -1;
    std::array<SmoothedFilter, kVoiceFilters> voice;
    float amountTarget = 0.0f, backgroundTarget = 1.0f;
    Ramp background;
    bool active = false;
};

} // namespace cinemax::dsp
