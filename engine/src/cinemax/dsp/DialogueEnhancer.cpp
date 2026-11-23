#include "DialogueEnhancer.h"

namespace cinemax::dsp
{
namespace
{
    constexpr int kControlBlock = 32;
}

void DialogueEnhancer::prepare (double sampleRate, const ChannelLayout& l) noexcept
{
    fs = sampleRate;
    layout = l;
    centreIndex = layout.indexOf (Speaker::FC);

    voice[0].prepare (fs, FilterType::Peak, 250.0, 1.0, 0.0);
    voice[1].prepare (fs, FilterType::Peak, 1200.0, 1.0, 0.0);
    voice[2].prepare (fs, FilterType::Peak, 3000.0, 0.8, 0.0);

    background.reset (1.0f);
    backgroundTarget = 1.0f;
    amountTarget = 0.0f;
    active = false;
}

void DialogueEnhancer::reset() noexcept
{
    for (auto& f : voice)
        f.reset();
}

void DialogueEnhancer::setParams (const DspParams& p) noexcept
{
    const float a = p.dialogueEnabled ? p.dialogueAmount : 0.0f;
    amountTarget = a;

    voice[0].setTarget (250.0, 1.0, -3.0 * a);
    voice[1].setTarget (1200.0, 1.0, 2.5 * a);
    voice[2].setTarget (3000.0, 0.8, 6.0 * a);

    backgroundTarget = dbToGain (p.dialogueEnabled ? p.backgroundDb : 0.0f);
}

void DialogueEnhancer::process (float* const* ch, int numSamples) noexcept
{
    background.setTarget (backgroundTarget, (int) (0.05 * fs));

    const bool voiceIdle = voice[0].isTransparent() && voice[1].isTransparent() && voice[2].isTransparent();
    if (voiceIdle && background.remaining == 0 && background.current == 1.0f && amountTarget == 0.0f)
        return;   // fully neutral - leave the signal untouched

    const bool stereo = layout.isStereo();
    const int lfeIndex = layout.indexOf (Speaker::LFE);

    for (int start = 0; start < numSamples; start += kControlBlock)
    {
        const int n = std::min (kControlBlock, numSamples - start);

        for (auto& f : voice)
            f.advance (n);

        if (stereo)
        {
            float* l = ch[0] + start;
            float* r = ch[1] + start;

            for (int i = 0; i < n; ++i)
            {
                float m = 0.5f * (l[i] + r[i]);
                const float s = 0.5f * (l[i] - r[i]) * background.next();

                for (auto& f : voice)
                    if (! f.isTransparent())
                        m = f.process (0, m);

                l[i] = m + s;
                r[i] = m - s;
            }
        }
        else if (centreIndex >= 0)
        {
            float* c = ch[centreIndex] + start;

            for (int i = 0; i < n; ++i)
                for (auto& f : voice)
                    if (! f.isTransparent())
                        c[i] = f.process (0, c[i]);

            for (int i = 0; i < n; ++i)
            {
                const float g = background.next();
                for (int k = 0; k < layout.numChannels; ++k)
                    if (k != centreIndex && k != lfeIndex)
                        ch[k][start + i] *= g;
            }
        }
        else
        {
            // Mono or a layout without centre: voice EQ on every channel's sum is not
            // separable, so apply the voice EQ to channel 0..1 as a pair.
            for (int i = 0; i < n; ++i)
            {
                background.next();
                for (auto& f : voice)
                    if (! f.isTransparent())
                    {
                        ch[0][start + i] = f.process (0, ch[0][start + i]);
                        if (layout.numChannels > 1)
                            ch[1][start + i] = f.process (1, ch[1][start + i]);
                    }
            }
        }
    }
}

} // namespace cinemax::dsp
