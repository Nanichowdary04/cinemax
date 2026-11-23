#pragma once

#include "Filters.h"
#include "../core/Params.h"

#include <vector>

namespace cinemax::dsp
{
/** Speaker tuning for a TV output, after the user's EQ:
    - TV speakers (small, often firing down or backwards): protective high-pass at 55 Hz, body
      back around 160 Hz, voices forward around 2.8 kHz, a little air at 10 kHz
    - soundbar: a touch of body and presence only (soundbars are tuned already)
    - AV receiver / home theatre: flat (real speakers)
    All changes glide; not a TV = transparent. */
class TvSpeakerTuning
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setSetup (const TvSetup& tv) noexcept;
    void process (float* left, float* right, int numSamples) noexcept;

private:
    double fs = 48000.0;
    SmoothedFilter highPass, body, presence, air;
    bool hpOn = false;
};

/** Lip-sync: delays the finished output by up to 300 ms so the sound matches a TV whose
    picture processing runs late. Changing the delay dips the sound for ~10 ms instead of
    clicking. Allocation happens only in prepare(). */
class LipSyncDelay
{
public:
    static constexpr double kMaxMs = 300.0;

    void prepare (double sampleRate);
    void reset() noexcept;
    void setDelayMs (float ms) noexcept;
    void process (float* left, float* right, int numSamples) noexcept;
    int delaySamples() const noexcept { return current; }

private:
    double fs = 48000.0;
    std::vector<float> bufL, bufR;
    int mask = 0, write = 0;
    int current = 0, target = 0;
    float gain = 1.0f, gainStep = 0.0f;
    int phase = 0;   // 0 steady, 1 fading out, 2 fading in
    bool stale = false;   // buffers skipped while idle: clear before use
};

/** Half-angle (degrees) of the speakers seen from the seat, for the speaker virtualiser. */
float tvSpeakerHalfAngle (const TvSetup& tv) noexcept;

} // namespace cinemax::dsp
