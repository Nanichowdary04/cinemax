#include "Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cinemax::dsp
{
namespace
{
    double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        const double halfX = x * 0.5;
        for (int k = 1; k < 64; ++k)
        {
            term *= (halfX / k) * (halfX / k);
            sum += term;
            if (term < sum * 1.0e-12)
                break;
        }
        return sum;
    }
}

void Resampler::prepare (int numCh, double inputRate, double outputRate, bool highQuality, double capacitySeconds)
{
    channels = std::max (1, numCh);
    inRate = inputRate;
    outRate = outputRate;
    baseStep = inRate / outRate;
    driftPpm = 0.0;
    step = baseStep;

    taps = highQuality ? 32 : 16;
    half = taps / 2;

    const double beta = highQuality ? 8.6 : 6.5;
    const double cutoff = 0.5 * std::min (1.0, outRate / inRate) * (highQuality ? 0.95 : 0.91);
    const double i0beta = besselI0 (beta);

    table.assign ((size_t) (kPhases + 1) * (size_t) taps, 0.0f);

    for (int p = 0; p <= kPhases; ++p)
    {
        const double frac = (double) p / kPhases;
        double sum = 0.0;

        for (int t = 0; t < taps; ++t)
        {
            // Tap t multiplies input sample (i - half + 1 + t); distance from the
            // fractional position i + frac:
            const double x = (double) (t - half + 1) - frac;
            const double sinc = std::abs (x) < 1.0e-12 ? 1.0
                                                       : std::sin (2.0 * 3.14159265358979323846 * cutoff * x)
                                                             / (3.14159265358979323846 * x);
            const double v = std::abs (x) < 1.0e-12 ? 2.0 * cutoff : sinc;
            const double r = x / (double) half;
            const double w = std::abs (r) >= 1.0 ? 0.0 : besselI0 (beta * std::sqrt (1.0 - r * r)) / i0beta;
            const double h = v * w;
            table[(size_t) p * (size_t) taps + (size_t) t] = (float) h;
            sum += h;
        }

        // Unity DC gain for every phase.
        for (int t = 0; t < taps; ++t)
            table[(size_t) p * (size_t) taps + (size_t) t] = (float) (table[(size_t) p * (size_t) taps + (size_t) t] / sum);
    }

    capacity = std::max (8192, (int) (inRate * capacitySeconds) + taps * 4);
    buffer.assign ((size_t) channels * (size_t) capacity, 0.0f);
    kernel.assign ((size_t) taps, 0.0f);
    reset();
}

void Resampler::reset() noexcept
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    // Pre-fill the interpolation history with silence.
    writePos = taps;
    readPos = (double) half;   // first output centred after 'half' history samples
}

void Resampler::setDriftPpm (double ppm) noexcept
{
    driftPpm = std::clamp (ppm, -20000.0, 20000.0);
    step = baseStep * (1.0 + driftPpm * 1.0e-6);
}

void Resampler::compact() noexcept
{
    // Keep 'half' samples of history before the read position.
    const int keepFrom = std::max (0, (int) std::floor (readPos) - half - 1);
    if (keepFrom <= 0)
        return;

    const int remaining = writePos - keepFrom;
    for (int c = 0; c < channels; ++c)
    {
        float* ch = buffer.data() + (size_t) c * (size_t) capacity;
        std::memmove (ch, ch + keepFrom, sizeof (float) * (size_t) remaining);
    }

    writePos -= keepFrom;
    readPos -= keepFrom;
}

int Resampler::pushInterleaved (const float* data, int frames, int srcChannels) noexcept
{
    if (writePos + frames > capacity)
        compact();

    int dropped = 0;
    if (writePos + frames > capacity)
    {
        // Overflow: drop the oldest input so the newest audio is kept (counted as an xrun).
        const int excess = writePos + frames - capacity;
        discard (excess);
        compact();
        dropped = excess;

        if (writePos + frames > capacity)
        {
            const int skip = writePos + frames - capacity;
            data += (size_t) skip * (size_t) srcChannels;
            frames -= skip;
            dropped += skip;
        }
    }

    const int copyCh = std::min (channels, srcChannels);
    for (int c = 0; c < channels; ++c)
    {
        float* dst = buffer.data() + (size_t) c * (size_t) capacity + (size_t) writePos;
        if (c < copyCh)
        {
            const float* s = data + c;
            for (int i = 0; i < frames; ++i)
                dst[i] = s[(size_t) i * (size_t) srcChannels];
        }
        else
        {
            std::memset (dst, 0, sizeof (float) * (size_t) frames);
        }
    }

    writePos += frames;
    return dropped;
}

int Resampler::pushSilence (int frames) noexcept
{
    if (writePos + frames > capacity)
        compact();
    frames = std::min (frames, capacity - writePos);
    for (int c = 0; c < channels; ++c)
        std::memset (buffer.data() + (size_t) c * (size_t) capacity + (size_t) writePos, 0, sizeof (float) * (size_t) frames);
    writePos += frames;
    return frames;
}

double Resampler::bufferedFrames() const noexcept
{
    // Frames still to be consumed before the read position hits the lookahead limit.
    return std::max (0.0, (double) writePos - (double) half - readPos);
}

void Resampler::discard (int frames) noexcept
{
    const double maxDiscard = std::max (0.0, (double) writePos - (double) half - readPos);
    readPos += std::min ((double) frames, maxDiscard);
}

bool Resampler::canProduce (int outFrames) const noexcept
{
    const double lastPos = readPos + step * (double) (outFrames - 1);
    return (int) std::floor (lastPos) + half < writePos;
}

int Resampler::produce (float* const* out, int outFrames) noexcept
{
    int produced = 0;

    for (; produced < outFrames; ++produced)
    {
        const int i = (int) std::floor (readPos);
        if (i + half >= writePos)
            break;

        const double frac = readPos - (double) i;
        const double phase = frac * kPhases;
        const int p = std::min ((int) phase, kPhases - 1);
        const float pf = (float) (phase - (double) p);

        const float* k0 = table.data() + (size_t) p * (size_t) taps;
        const float* k1 = k0 + taps;
        for (int t = 0; t < taps; ++t)
            kernel[(size_t) t] = k0[t] + pf * (k1[t] - k0[t]);

        const int first = i - half + 1;
        for (int c = 0; c < channels; ++c)
        {
            const float* x = buffer.data() + (size_t) c * (size_t) capacity + (size_t) first;
            float acc = 0.0f;
            for (int t = 0; t < taps; ++t)
                acc += kernel[(size_t) t] * x[t];
            out[c][produced] = acc;
        }

        readPos += step;
    }

    if (readPos > capacity * 0.5)
        compact();

    return produced;
}

} // namespace cinemax::dsp
