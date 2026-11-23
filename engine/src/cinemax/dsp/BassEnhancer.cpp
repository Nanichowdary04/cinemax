#include "BassEnhancer.h"

namespace cinemax::dsp
{
namespace
{
    constexpr int kControlBlock = 32;
    constexpr double kLfThreshold = 0.5;   // -6 dBFS peak in the low band before LF limiting
}

void BassEnhancer::prepare (double sampleRate) noexcept
{
    fs = sampleRate;

    shelf.prepare (fs, FilterType::LowShelf, 90.0, 0.7071, 0.0);
    sub.prepare (fs, FilterType::Peak, 45.0, 1.0, 0.0);

    hgEnvAttack  = timeCoeff (fs, 2.0);
    hgEnvRelease = timeCoeff (fs, 60.0);
    hgDesignedFreq = -1.0;
    designHarmonicBands (90.0);
    harmonics.reset (0.0f);
    harmonicsTarget = 0.0f;

    splitL.c = splitR.c = design::make (FilterType::LowPass, fs, 120.0, 0.7071, 0.0);
    lfEnvAttack   = timeCoeff (fs, 3.0);
    lfEnvRelease  = timeCoeff (fs, 200.0);
    lfGainAttack  = timeCoeff (fs, 5.0);
    lfGainRelease = timeCoeff (fs, 150.0);

    hpfDesignedHz = hpfTargetHz = 0.0f;
    hpfMix.reset (0.0f);

    reset();
}

void BassEnhancer::reset() noexcept
{
    shelf.reset();
    sub.reset();
    for (auto* b : { &hgInHp, &hgInLp1, &hgInLp2, &hgOutHp1, &hgOutHp2, &hgOutLp, &splitL, &splitR, &hpfL1, &hpfR1, &hpfL2, &hpfR2 })
        b->reset();
    hgEnv = 0.0;
    lfEnv = 0.0;
    lfGain = 1.0;
    lfReductionDb = 0.0f;
}

void BassEnhancer::designHarmonicBands (double bassFreq) noexcept
{
    if (std::abs (bassFreq - hgDesignedFreq) < 1.0)
        return;

    hgDesignedFreq = bassFreq;
    const double inTop = std::clamp (bassFreq * 1.25, 60.0, 200.0);

    hgInHp.c   = design::make (FilterType::HighPass, fs, 25.0, 0.7071, 0.0);
    hgInLp1.c  = design::make (FilterType::LowPass, fs, inTop, 0.5412, 0.0);   // 4th-order Butterworth
    hgInLp2.c  = design::make (FilterType::LowPass, fs, inTop, 1.3066, 0.0);
    hgOutHp1.c = design::make (FilterType::HighPass, fs, inTop, 0.5412, 0.0);
    hgOutHp2.c = design::make (FilterType::HighPass, fs, inTop, 1.3066, 0.0);
    hgOutLp.c  = design::make (FilterType::LowPass, fs, 1200.0, 0.7071, 0.0);
}

void BassEnhancer::setParams (const DspParams& p, float speakerHighPassHz) noexcept
{
    const bool on = p.bassEnabled;

    shelf.setTarget (p.bassFreq, 0.7071, on ? p.bassBoostDb : 0.0f);
    sub.setTarget (45.0, 1.0, on ? p.subBassDb : 0.0f);
    harmonicsTarget = on ? p.bassHarmonics : 0.0f;
    designHarmonicBands (p.bassFreq);

    protect = p.bassProtect;
    hpfTargetHz = (on && p.bassProtect) ? speakerHighPassHz : 0.0f;
}

void BassEnhancer::process (float* left, float* right, int numSamples) noexcept
{
    harmonics.setTarget (harmonicsTarget, (int) (0.05 * fs));

    // Protective high-pass: design once for the device, cross-fade the effect in/out.
    if (hpfTargetHz > 0.0f && std::abs (hpfTargetHz - hpfDesignedHz) > 0.5f)
    {
        hpfDesignedHz = hpfTargetHz;
        hpfL1.c = hpfR1.c = design::make (FilterType::HighPass, fs, hpfDesignedHz, 0.5412, 0.0);
        hpfL2.c = hpfR2.c = design::make (FilterType::HighPass, fs, hpfDesignedHz, 1.3066, 0.0);
    }
    hpfMix.setTarget (hpfTargetHz > 0.0f ? 1.0f : 0.0f, (int) (0.1 * fs));

    float maxReduction = 0.0f;

    for (int start = 0; start < numSamples; start += kControlBlock)
    {
        const int n = std::min (kControlBlock, numSamples - start);
        float* l = left + start;
        float* r = right + start;

        shelf.advance (n);
        sub.advance (n);

        for (int i = 0; i < n; ++i)
        {
            float xl = l[i], xr = r[i];

            // --- harmonic generator, driven by the unboosted low band -------------
            const float h = harmonics.next();
            float harmonic = 0.0f;
            {
                const float mono = 0.5f * (xl + xr);
                float b = hgInHp.process (mono);
                b = hgInLp2.process (hgInLp1.process (b));

                const double a = std::abs ((double) b);
                hgEnv = a > hgEnv ? a + (hgEnv - a) * hgEnvAttack : a + (hgEnv - a) * hgEnvRelease;

                if (h > 0.0001f)
                {
                    const double env = hgEnv + 1.0e-9;
                    const double xn = std::clamp ((double) b / env, -1.0, 1.0);
                    const double x2 = xn * xn;
                    const double t2 = 2.0 * x2 - 1.0;               // 2nd harmonic
                    const double t3 = xn * (4.0 * x2 - 3.0);        // 3rd harmonic
                    const double t4 = 8.0 * x2 * x2 - 8.0 * x2 + 1.0; // 4th harmonic
                    harmonic = (float) ((0.55 * t2 + 0.35 * t3 + 0.2 * t4) * env);
                }

                // keep the output band filters running so enabling is click-free
                harmonic = hgOutLp.process (hgOutHp2.process (hgOutHp1.process (harmonic)));
            }

            // --- shelf + sub ------------------------------------------------------
            if (! shelf.isTransparent()) { xl = shelf.process (0, xl); xr = shelf.process (1, xr); }
            if (! sub.isTransparent())   { xl = sub.process (0, xl);   xr = sub.process (1, xr); }

            const float hg = h * 1.4f * harmonic;
            xl += hg;
            xr += hg;

            // --- LF limiter (x + (g-1)*lf keeps perfect reconstruction at g = 1) --
            const float lfl = splitL.process (xl);
            const float lfr = splitR.process (xr);

            if (protect)
            {
                const double peak = std::max (std::abs ((double) lfl), std::abs ((double) lfr));
                lfEnv = peak > lfEnv ? peak + (lfEnv - peak) * lfEnvAttack : peak + (lfEnv - peak) * lfEnvRelease;

                const double want = lfEnv > kLfThreshold ? kLfThreshold / lfEnv : 1.0;
                lfGain = want < lfGain ? want + (lfGain - want) * lfGainAttack : want + (lfGain - want) * lfGainRelease;
            }
            else
            {
                lfGain = 1.0 + (lfGain - 1.0) * lfGainRelease;
            }

            if (lfGain < 0.99999)
            {
                const float k = (float) (lfGain - 1.0);
                xl += k * lfl;
                xr += k * lfr;
            }

            // --- protective high-pass (cross-faded) ------------------------------
            const float m = hpfMix.next();
            if (m > 0.0f && hpfDesignedHz > 0.0f)
            {
                const float hl = hpfL2.process (hpfL1.process (xl));
                const float hr = hpfR2.process (hpfR1.process (xr));
                xl += m * (hl - xl);
                xr += m * (hr - xr);
            }

            l[i] = xl;
            r[i] = xr;
        }

        maxReduction = std::max (maxReduction, (float) -gainToDb ((float) lfGain));
    }

    lfReductionDb = maxReduction;
}

} // namespace cinemax::dsp
