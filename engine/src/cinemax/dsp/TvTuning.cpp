#include "TvTuning.h"

namespace cinemax::dsp
{
float tvSpeakerHalfAngle (const TvSetup& tv) noexcept
{
    const double dist = std::clamp ((double) tv.distanceM, 0.8, 8.0);
    double span = 0.0;   // distance between the left and right speakers (m)
    switch (tv.speakers)
    {
        case 2:  span = 0.85; break;                                                  // typical soundbar
        case 3:  return 30.0f;                                                        // stereo speakers, standard placement
        default: span = 0.80 * std::clamp ((double) tv.sizeInch, 24.0, 120.0) * 0.0254 * 0.8716; break;   // ~80% of a 16:9 screen width
    }
    return (float) (std::atan ((span * 0.5) / dist) * 180.0 / kPi);
}

void TvSpeakerTuning::prepare (double sampleRate) noexcept
{
    fs = sampleRate;
    highPass.prepare (fs, FilterType::HighPass, 55.0, 0.7071, 0.0);
    body.prepare (fs, FilterType::LowShelf, 160.0, 0.7071, 0.0);
    presence.prepare (fs, FilterType::Peak, 2800.0, 0.9, 0.0);
    air.prepare (fs, FilterType::HighShelf, std::min (10000.0, fs * 0.42), 0.7071, 0.0);
    hpOn = false;
}

void TvSpeakerTuning::reset() noexcept
{
    for (auto* f : { &highPass, &body, &presence, &air })
        f->reset();
}

void TvSpeakerTuning::setSetup (const TvSetup& tv) noexcept
{
    float bodyDb = 0, presenceDb = 0, airDb = 0;
    hpOn = false;
    if (tv.isTv)
    {
        switch (tv.speakers)
        {
            case 1: bodyDb = 3.0f; presenceDb = 2.5f; airDb = 1.5f; hpOn = true; break;   // TV speakers
            case 2: bodyDb = 1.0f; presenceDb = 1.0f; break;                             // soundbar
            default: break;                                                              // AV receiver
        }
    }
    body.setTarget (160.0, 0.7071, bodyDb);
    presence.setTarget (2800.0, 0.9, presenceDb);
    air.setTarget (std::min (10000.0, fs * 0.42), 0.7071, airDb);
}

void TvSpeakerTuning::process (float* left, float* right, int n) noexcept
{
    constexpr int kControlBlock = 32;
    for (int start = 0; start < n; start += kControlBlock)
    {
        const int len = std::min (kControlBlock, n - start);
        if (hpOn)
            for (int i = start; i < start + len; ++i)
            {
                left[i] = highPass.process (0, left[i]);
                right[i] = highPass.process (1, right[i]);
            }
        for (auto* f : { &body, &presence, &air })
        {
            f->advance (len);
            if (f->isTransparent())
                continue;
            for (int i = start; i < start + len; ++i)
            {
                left[i] = f->process (0, left[i]);
                right[i] = f->process (1, right[i]);
            }
        }
    }
}

//==============================================================================
void LipSyncDelay::prepare (double sampleRate)
{
    fs = sampleRate;
    const int need = (int) std::ceil (kMaxMs * 0.001 * fs) + 4;
    int size = 1;
    while (size < need) size <<= 1;
    bufL.assign ((size_t) size, 0.0f);
    bufR.assign ((size_t) size, 0.0f);
    mask = size - 1;
    reset();
}

void LipSyncDelay::reset() noexcept
{
    std::fill (bufL.begin(), bufL.end(), 0.0f);
    std::fill (bufR.begin(), bufR.end(), 0.0f);
    write = 0;
    current = target;
    gain = 1.0f;
    phase = 0;
}

void LipSyncDelay::setDelayMs (float ms) noexcept
{
    target = (int) std::lround (std::clamp ((double) ms, 0.0, kMaxMs) * 0.001 * fs);
}

void LipSyncDelay::process (float* left, float* right, int n) noexcept
{
    if (current == 0 && target == 0 && phase == 0)
    {
        stale = true;   // nothing to do; the buffers no longer hold recent audio
        return;
    }
    if (stale)
    {
        std::fill (bufL.begin(), bufL.end(), 0.0f);
        std::fill (bufR.begin(), bufR.end(), 0.0f);
        stale = false;
    }

    const int fade = std::max (1, (int) (0.005 * fs));   // 5 ms out, 5 ms in
    for (int i = 0; i < n; ++i)
    {
        bufL[(size_t) write] = left[i];
        bufR[(size_t) write] = right[i];

        if (phase == 0 && target != current)
        {
            phase = 1;
            gainStep = -1.0f / (float) fade;
        }
        if (phase != 0)
        {
            gain += gainStep;
            if (phase == 1 && gain <= 0.0f)
            {
                gain = 0.0f;
                current = target;           // jump while silent
                phase = 2;
                gainStep = 1.0f / (float) fade;
            }
            else if (phase == 2 && gain >= 1.0f)
            {
                gain = 1.0f;
                phase = 0;
            }
        }

        const int r = (write - current) & mask;
        left[i] = bufL[(size_t) r] * gain;
        right[i] = bufR[(size_t) r] * gain;
        write = (write + 1) & mask;
    }
}

} // namespace cinemax::dsp
