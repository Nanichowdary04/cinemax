#include "Equalizer.h"

namespace cinemax::dsp
{
namespace
{
    constexpr int kControlBlock = 32;
}

void Equalizer::prepare (double sampleRate) noexcept
{
    fs = sampleRate;

    for (int i = 0; i < kNumEqBands; ++i)
        filters[(size_t) i].prepare (fs, FilterType::Peak, kDefaultEqFreqs[(size_t) i], 1.41, 0.0);

    filters[kNumEqBands + 0].prepare (fs, FilterType::LowShelf, 100.0, 0.7071, 0.0);
    filters[kNumEqBands + 1].prepare (fs, FilterType::Peak, 1000.0, 0.9, 0.0);
    filters[kNumEqBands + 2].prepare (fs, FilterType::HighShelf, 8000.0, 0.7071, 0.0);

    preamp.reset (1.0f);
    preampTarget = 1.0f;
}

void Equalizer::reset() noexcept
{
    for (auto& f : filters)
        f.reset();
}

void Equalizer::setParams (const DspParams& p) noexcept
{
    const bool on = p.eqEnabled;

    for (int i = 0; i < kNumEqBands; ++i)
    {
        const auto& b = p.eqBands[(size_t) i];
        filters[(size_t) i].setTarget (b.freq, b.q, on ? b.gainDb : 0.0f);
    }

    filters[kNumEqBands + 0].setTarget (100.0, 0.7071, on ? p.bassDb : 0.0f);
    filters[kNumEqBands + 1].setTarget (1000.0, 0.9, on ? p.midDb : 0.0f);
    filters[kNumEqBands + 2].setTarget (8000.0, 0.7071, on ? p.trebleDb : 0.0f);

    preampTarget = dbToGain (on ? p.preampDb : 0.0f);
}

void Equalizer::process (float* left, float* right, int numSamples) noexcept
{
    preamp.setTarget (preampTarget, (int) (0.03 * fs));

    for (int start = 0; start < numSamples; start += kControlBlock)
    {
        const int n = std::min (kControlBlock, numSamples - start);
        float* l = left + start;
        float* r = right + start;

        for (int i = 0; i < n; ++i)
        {
            const float g = preamp.next();
            l[i] *= g;
            r[i] *= g;
        }

        for (auto& f : filters)
        {
            f.advance (n);
            if (f.isTransparent())
                continue;

            for (int i = 0; i < n; ++i)
            {
                l[i] = f.process (0, l[i]);
                r[i] = f.process (1, r[i]);
            }
        }
    }
}

} // namespace cinemax::dsp
