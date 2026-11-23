#pragma once

#include "Filters.h"
#include "../core/Params.h"

#include <vector>

namespace cinemax::dsp
{
//==============================================================================
/** ITU-R BS.1770 K-weighted loudness meter (momentary 400 ms / short-term 3 s),
    computed from 100 ms blocks - allocation free after prepare(). */
class LoudnessMeter
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void process (const float* left, const float* right, int numSamples) noexcept;

    float shortTermLufs() const noexcept { return shortTerm; }
    float momentaryLufs() const noexcept { return momentary; }
    /** Mean loudness of all 100 ms blocks above -70 LUFS since reset (diagnostics / tests). */
    float integratedLufs() const noexcept
    {
        return integratedBlocks > 0 ? (float) (-0.691 + 10.0 * std::log10 (integratedSum / integratedBlocks)) : -70.0f;
    }
    /** Incremented every time a new 100 ms block completes. */
    unsigned blockCounter() const noexcept { return blocks; }

private:
    double fs = 48000.0;
    Biquad preL, preR, rlbL, rlbR;
    double acc = 0.0;
    int accCount = 0, blockSize = 4800;
    std::array<double, 30> history {};
    int historyPos = 0, historyFilled = 0;
    float shortTerm = -70.0f, momentary = -70.0f;
    unsigned blocks = 0;
    double integratedSum = 0.0;
    long integratedBlocks = 0;
};

//==============================================================================
/** Slow, open-loop loudness normaliser driven by the K-weighted short-term
    loudness of its input. Gated so silence / room tone is never pumped up. */
class LoudnessNormalizer
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void setParams (bool enabled, float targetLufs, float maxBoostDb) noexcept;
    void process (float* left, float* right, int numSamples) noexcept;

    float currentGainDb() const noexcept { return gainDb; }
    float inputLoudness() const noexcept { return meter.shortTermLufs(); }

private:
    double fs = 48000.0;
    LoudnessMeter meter;
    bool enabled = false;
    float target = -18.0f, maxBoost = 9.0f;
    float gainDb = 0.0f, desiredDb = 0.0f;
    unsigned lastBlock = 0;
    Ramp gain;
};

//==============================================================================
/** Feed-forward stereo-linked compressor with soft knee, detector side-chain
    high-passed at 90 Hz (bass doesn't pump the mix) and automatic partial make-up.
    'amount' 0..1 maps to threshold / ratio / make-up. */
class Compressor
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;
    void setAmount (float newAmount) noexcept { amountTarget = std::clamp (newAmount, 0.0f, 1.0f); }
    /** 1 = automatic make-up gain, 0 = none (when a loudness normaliser follows and sets the level). */
    void setMakeupScale (float s) noexcept { makeupScale = std::clamp (s, 0.0f, 1.0f); }
    void process (float* left, float* right, int numSamples) noexcept;
    float gainReductionDb() const noexcept { return grReport; }

private:
    double fs = 48000.0;
    Biquad scL, scR;
    double env = 0.0, envCoeff = 0.0, attack = 0.0, release = 0.0;
    double grDb = 0.0;
    float amountTarget = 0.0f, makeupScale = 1.0f;
    Glide amount;
    float amountCoeff = 0.0f;
    float grReport = 0.0f;
};

//==============================================================================
/** Look-ahead peak limiter. Guarantees |output| <= ceiling (sample peak):
    required gain -> sliding minimum over the look-ahead window -> instant-attack /
    smooth-release -> moving average over the same window. Every value averaged is
    <= the gain the peak needs, so the peak can never pass the ceiling. */
class Limiter
{
public:
    void prepare (double sampleRate, double lookaheadMs = 1.5, double releaseMs = 80.0);
    void reset() noexcept;
    void setCeilingDb (float db) noexcept { ceilingTarget = dbToGain (std::clamp (db, -12.0f, 0.0f)); }
    void process (float* left, float* right, int numSamples) noexcept;

    float gainReductionDb() const noexcept { return grReport; }
    int latencySamples() const noexcept { return lookahead; }

private:
    double fs = 48000.0;
    int lookahead = 72, window = 73;
    std::vector<float> delayL, delayR;
    int delayPos = 0;

    // sliding minimum (monotonic deque over a ring)
    std::vector<float> dqValue;
    std::vector<int> dqIndex;
    int dqHead = 0, dqTail = 0, dqMask = 0;
    int sampleIndex = 0;

    // moving average of the released gain
    std::vector<float> avgRing;
    int avgPos = 0;
    double avgSum = 0.0;
    int avgRecalc = 0;

    double released = 1.0, releaseCoeff = 0.0;
    float ceiling = 0.891f, ceilingTarget = 0.891f;
    float grReport = 0.0f;
};

} // namespace cinemax::dsp
