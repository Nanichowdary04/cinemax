#pragma once

#include "Filters.h"
#include "../core/Params.h"

namespace cinemax::dsp
{
/** Bass enhancement (stereo):
    - low-shelf boost at bassFreq and a sub-bass peak around 45 Hz
    - psychoacoustic bass: Chebyshev harmonic generator on the envelope-normalised
      low band, so small drivers (laptop speakers, earbuds) convey fundamentals
      they cannot reproduce ("missing fundamental" effect)
    - protection: dynamic low-frequency limiter (only the LF band is turned down,
      perfect reconstruction when inactive) and an optional device-aware high-pass
      that keeps energy below the driver's range away from small speakers. */
class BassEnhancer
{
public:
    void prepare (double sampleRate) noexcept;
    void reset() noexcept;
    void setParams (const DspParams& p, float speakerHighPassHz) noexcept;
    void process (float* left, float* right, int numSamples) noexcept;

    /** Current LF limiter gain reduction in dB (positive). */
    float getGainReductionDb() const noexcept { return lfReductionDb; }

private:
    void designHarmonicBands (double bassFreq) noexcept;

    double fs = 48000.0;

    SmoothedFilter shelf, sub;

    // Harmonic generator (mono)
    Biquad hgInHp, hgInLp1, hgInLp2, hgOutHp1, hgOutHp2, hgOutLp;
    double hgEnv = 0.0, hgEnvAttack = 0.0, hgEnvRelease = 0.0;
    double hgDesignedFreq = -1.0;
    float harmonicsTarget = 0.0f;
    Ramp harmonics;

    // LF limiter
    Biquad splitL, splitR;
    double lfEnv = 0.0, lfEnvAttack = 0.0, lfEnvRelease = 0.0;
    double lfGain = 1.0, lfGainAttack = 0.0, lfGainRelease = 0.0;
    bool protect = true;
    float lfReductionDb = 0.0f;

    // Device-aware protective high-pass, cross-faded in/out
    Biquad hpfL1, hpfR1, hpfL2, hpfR2;
    float hpfDesignedHz = 0.0f, hpfTargetHz = 0.0f;
    Ramp hpfMix;
};

} // namespace cinemax::dsp
