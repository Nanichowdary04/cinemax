#include "IntelligentEq.h"

namespace cinemax::dsp
{
namespace
{
    constexpr int kUpdateEvery = 2048;          // samples between correction updates (~43 ms)
    constexpr double kAnalysisSeconds = 2.0;    // energy follower time constant
    constexpr double kGateDb = -55.0;           // below this the measurement is frozen
    constexpr float kGlideDbPerSecond = 1.5f;   // how fast a correction may change
}

const std::array<float, IntelligentEq::kBands>& IntelligentEq::target (int profile) noexcept
{
    // Long-term octave-band balance of a well-mixed film/music programme (relative dB).
    // Balanced follows the typical professional mix; Warm adds body and softens the top;
    // Detailed adds presence and air.        31.5   63    125   250   500   1k    2k    4k    8k    16k
    static const std::array<float, kBands> balanced { -5.0f, 0.0f, 0.5f, 0.0f, -1.0f, -2.0f, -3.5f, -5.0f, -7.5f, -13.0f };
    static const std::array<float, kBands> warm     { -3.5f, 1.5f, 1.5f, 0.5f, -1.0f, -2.0f, -4.0f, -6.0f, -9.0f, -15.0f };
    static const std::array<float, kBands> detailed { -5.5f, -0.5f, 0.0f, -0.5f, -1.0f, -1.5f, -2.5f, -3.5f, -6.0f, -11.5f };
    switch (profile)
    {
        case 1:  return warm;
        case 2:  return detailed;
        default: return balanced;
    }
}

void IntelligentEq::prepare (double sampleRate) noexcept
{
    fs = sampleRate;
    for (int b = 0; b < kBands; ++b)
    {
        const double f = kDefaultEqFreqs[(size_t) b];
        // Octave-wide band-pass (Q ~1.41) for analysis; peaking filters (Q 1.0) for the correction
        // so neighbouring bands blend into a smooth curve.
        bandFilters[(size_t) b].c = design::make (FilterType::BandPass, fs, std::min (f, fs * 0.45), 1.41, 0.0);
        filters[(size_t) b].prepare (fs, FilterType::Peak, std::min (f, fs * 0.45), 1.0, 0.0);
    }
    energyCoeff = timeCoeff (fs, kAnalysisSeconds * 1000.0);
    reset();
}

void IntelligentEq::reset() noexcept
{
    for (auto& b : bandFilters) b.reset();
    for (auto& f : filters) f.reset();
    bandEnergy.fill (0.0);
    totalEnergy = 0.0;
    primed = false;
    sinceUpdate = 0;
    wantedDb.fill (0.0f);
    appliedDb.fill (0.0f);
}

void IntelligentEq::setParams (const DspParams& p) noexcept
{
    enabled = p.ieqEnabled;
    profile = std::clamp (p.ieqProfile, 0, 2);
    amount = std::clamp (p.ieqAmount, 0.0f, 1.0f);
}

void IntelligentEq::analyse (const float* left, const float* right, int n) noexcept
{
    if (! enabled)
        return;

    // Per-block energies (cheap), folded into slow followers.
    std::array<double, kBands> blockE {};
    double blockTotal = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float m = 0.5f * (left[i] + right[i]);
        blockTotal += (double) m * m;
        for (int b = 0; b < kBands; ++b)
        {
            const float y = bandFilters[(size_t) b].process (m);
            blockE[(size_t) b] += (double) y * y;
        }
    }

    const double meanTotal = blockTotal / std::max (1, n);
    const double gate = std::pow (10.0, kGateDb / 10.0);
    if (meanTotal > gate)
    {
        const double k = std::pow (energyCoeff, (double) n);
        for (int b = 0; b < kBands; ++b)
        {
            const double e = blockE[(size_t) b] / std::max (1, n);
            bandEnergy[(size_t) b] = primed ? e + (bandEnergy[(size_t) b] - e) * k : e;
        }
        totalEnergy = primed ? meanTotal + (totalEnergy - meanTotal) * k : meanTotal;
        primed = true;
    }

    sinceUpdate += n;
    if (sinceUpdate >= kUpdateEvery)
    {
        updateCorrection (sinceUpdate);
        sinceUpdate = 0;
    }
}

void IntelligentEq::updateCorrection (int samples) noexcept
{
    if (primed)
    {
        const auto& t = target (profile);
        std::array<float, kBands> measured {};
        for (int b = 0; b < kBands; ++b)
            measured[(size_t) b] = (float) (10.0 * std::log10 (bandEnergy[(size_t) b] + 1e-14));

        // Align the curves where the ear is most sensitive (250 Hz - 4 kHz).
        float offset = 0.0f;
        int cnt = 0;
        for (int b = 3; b <= 7; ++b)
        {
            offset += measured[(size_t) b] - t[(size_t) b];
            ++cnt;
        }
        offset /= (float) cnt;

        for (int b = 0; b < kBands; ++b)
        {
            const float diff = t[(size_t) b] - (measured[(size_t) b] - offset);
            const bool edge = b == 0 || b == kBands - 1;
            const float maxBoost = edge ? 3.0f : 5.0f, maxCut = edge ? 3.0f : 4.0f;
            wantedDb[(size_t) b] = std::clamp (diff * 0.8f, -maxCut, maxBoost) * amount;
        }
    }

    // Glide toward the wanted correction (or to flat when disabled).
    const float step = kGlideDbPerSecond * (float) samples / (float) fs;
    for (int b = 0; b < kBands; ++b)
    {
        const float want = enabled ? wantedDb[(size_t) b] : 0.0f;
        auto& a = appliedDb[(size_t) b];
        a += std::clamp (want - a, -step, step);
        filters[(size_t) b].setTarget (std::min ((double) kDefaultEqFreqs[(size_t) b], fs * 0.45), 1.0, a);
    }
}

void IntelligentEq::process (float* left, float* right, int n) noexcept
{
    if (! enabled)
    {
        // keep gliding to flat while disabled
        sinceUpdate += n;
        if (sinceUpdate >= kUpdateEvery)
        {
            updateCorrection (sinceUpdate);
            sinceUpdate = 0;
        }
    }

    constexpr int kControlBlock = 32;
    for (int start = 0; start < n; start += kControlBlock)
    {
        const int len = std::min (kControlBlock, n - start);
        for (auto& f : filters)
        {
            f.advance (len);
            if (f.isTransparent())
                continue;
            for (int i = start; i < start + len; ++i)
            {
                left[i] = f.process (0, left[i]);
                right[i] = f.process (1, right[i]);
            }
        }
    }
}

} // namespace cinemax::dsp
