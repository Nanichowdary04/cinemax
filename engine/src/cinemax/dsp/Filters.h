// Filter primitives used throughout the CINEMAX DSP chain.
// All processing is allocation-free; state is double precision so low-frequency
// filters (31 Hz EQ band, sub-bass, K-weighting) stay numerically clean.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace cinemax::dsp
{
constexpr double kPi = 3.14159265358979323846;

inline float dbToGain (float db) noexcept     { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g, float floorDb = -120.0f) noexcept
{
    return g > 0.0f ? std::max (floorDb, 20.0f * std::log10 (g)) : floorDb;
}

/** One-pole smoothing coefficient for a time constant in milliseconds. */
inline double timeCoeff (double sampleRate, double ms) noexcept
{
    return ms <= 0.0 ? 0.0 : std::exp (-1.0 / (0.001 * ms * sampleRate));
}

//==============================================================================
struct BiquadCoeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

enum class FilterType { Peak, LowShelf, HighShelf, LowPass, HighPass, BandPass, Identity };

namespace design
{
    inline double safeFreq (double f, double fs) noexcept { return std::clamp (f, 5.0, fs * 0.49); }

    /** RBJ audio-EQ-cookbook designs. q is the filter Q (shelves: 0.7071 = slope 1). */
    inline BiquadCoeffs make (FilterType type, double fs, double f, double q, double gainDb) noexcept
    {
        if (type == FilterType::Identity)
            return {};

        f = safeFreq (f, fs);
        q = std::max (q, 0.05);

        const double A     = std::pow (10.0, gainDb / 40.0);
        const double w0    = 2.0 * kPi * f / fs;
        const double cosw  = std::cos (w0);
        const double sinw  = std::sin (w0);
        const double alpha = sinw / (2.0 * q);
        const double sqA   = std::sqrt (A);

        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

        switch (type)
        {
            case FilterType::Peak:
                b0 = 1.0 + alpha * A;  b1 = -2.0 * cosw;  b2 = 1.0 - alpha * A;
                a0 = 1.0 + alpha / A;  a1 = -2.0 * cosw;  a2 = 1.0 - alpha / A;
                break;

            case FilterType::LowShelf:
                b0 = A * ((A + 1) - (A - 1) * cosw + 2 * sqA * alpha);
                b1 = 2 * A * ((A - 1) - (A + 1) * cosw);
                b2 = A * ((A + 1) - (A - 1) * cosw - 2 * sqA * alpha);
                a0 = (A + 1) + (A - 1) * cosw + 2 * sqA * alpha;
                a1 = -2 * ((A - 1) + (A + 1) * cosw);
                a2 = (A + 1) + (A - 1) * cosw - 2 * sqA * alpha;
                break;

            case FilterType::HighShelf:
                b0 = A * ((A + 1) + (A - 1) * cosw + 2 * sqA * alpha);
                b1 = -2 * A * ((A - 1) + (A + 1) * cosw);
                b2 = A * ((A + 1) + (A - 1) * cosw - 2 * sqA * alpha);
                a0 = (A + 1) - (A - 1) * cosw + 2 * sqA * alpha;
                a1 = 2 * ((A - 1) - (A + 1) * cosw);
                a2 = (A + 1) - (A - 1) * cosw - 2 * sqA * alpha;
                break;

            case FilterType::LowPass:
                b0 = (1.0 - cosw) * 0.5;  b1 = 1.0 - cosw;  b2 = (1.0 - cosw) * 0.5;
                a0 = 1.0 + alpha;         a1 = -2.0 * cosw; a2 = 1.0 - alpha;
                break;

            case FilterType::HighPass:
                b0 = (1.0 + cosw) * 0.5;  b1 = -(1.0 + cosw); b2 = (1.0 + cosw) * 0.5;
                a0 = 1.0 + alpha;         a1 = -2.0 * cosw;   a2 = 1.0 - alpha;
                break;

            case FilterType::BandPass:   // constant 0 dB peak gain
                b0 = alpha;       b1 = 0.0;          b2 = -alpha;
                a0 = 1.0 + alpha; a1 = -2.0 * cosw;  a2 = 1.0 - alpha;
                break;

            case FilterType::Identity:
                break;
        }

        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }
}

/** Magnitude response in dB at frequency f. */
inline double magnitudeDb (const BiquadCoeffs& c, double f, double fs) noexcept
{
    const std::complex<double> z1 = std::polar (1.0, -2.0 * kPi * f / fs);
    const std::complex<double> z2 = z1 * z1;
    const auto num = c.b0 + c.b1 * z1 + c.b2 * z2;
    const auto den = 1.0 + c.a1 * z1 + c.a2 * z2;
    return 20.0 * std::log10 (std::max (1e-12, std::abs (num / den)));
}

//==============================================================================
/** Transposed direct form II biquad, double precision state. */
struct Biquad
{
    BiquadCoeffs c;
    double z1 = 0.0, z2 = 0.0;

    inline float process (float in) noexcept
    {
        const double x = in;
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return (float) y;
    }

    void reset() noexcept { z1 = z2 = 0.0; }
};

/** First-order section y = b0 x + b1 x[n-1] - a1 y[n-1]. */
struct OnePole
{
    double b0 = 1.0, b1 = 0.0, a1 = 0.0;
    double x1 = 0.0, y1 = 0.0;

    inline float process (float in) noexcept
    {
        const double x = in;
        const double y = b0 * x + b1 * x1 - a1 * y1;
        x1 = x;
        y1 = y;
        return (float) y;
    }

    void reset() noexcept { x1 = y1 = 0.0; }

    /** First-order low-pass (bilinear, prewarped). */
    void setLowPass (double fs, double fc) noexcept
    {
        const double K = std::tan (kPi * std::clamp (fc, 5.0, fs * 0.49) / fs);
        b0 = K / (1.0 + K);  b1 = b0;  a1 = (K - 1.0) / (K + 1.0);
    }

    /** First-order high-pass (bilinear, prewarped). */
    void setHighPass (double fs, double fc) noexcept
    {
        const double K = std::tan (kPi * std::clamp (fc, 5.0, fs * 0.49) / fs);
        b0 = 1.0 / (1.0 + K);  b1 = -b0;  a1 = (K - 1.0) / (K + 1.0);
    }

    /** First-order all-pass with given break frequency (used for decorrelation). */
    void setAllPass (double fs, double fc) noexcept
    {
        const double K = std::tan (kPi * std::clamp (fc, 5.0, fs * 0.49) / fs);
        const double c = (K - 1.0) / (K + 1.0);
        b0 = c;  b1 = 1.0;  a1 = c;
    }
};

//==============================================================================
/** A biquad whose frequency / gain / Q glide smoothly to new targets. Up to two
    channels share the same coefficients. Redesigns at most once per control block. */
class SmoothedFilter
{
public:
    void prepare (double sampleRate, FilterType filterType, double f, double q, double gainDb) noexcept
    {
        fs = sampleRate;
        type = filterType;
        freq = freqT = f;
        qv = qT = q;
        gain = gainT = gainDb;
        redesign();
        reset();
    }

    void setTarget (double f, double q, double gainDb) noexcept
    {
        freqT = std::clamp (f, 10.0, 24000.0);
        qT = std::clamp (q, 0.1, 12.0);
        gainT = std::clamp (gainDb, -30.0, 30.0);

        // A band above ~0.45 fs cannot be represented at this rate: make it transparent.
        if (usesGain() && freqT > fs * 0.45)
            gainT = 0.0;
    }

    /** Advances smoothing by a control block of 'samples'. */
    void advance (int samples) noexcept
    {
        if (freq == freqT && qv == qT && gain == gainT)
            return;

        const double k = 1.0 - std::exp (-(double) samples / (0.030 * fs)); // ~30 ms glide

        gain += (gainT - gain) * k;
        if (std::abs (gainT - gain) < 0.002) gain = gainT;

        const double lf = std::log (freq), lfT = std::log (freqT);
        freq = std::exp (lf + (lfT - lf) * k);
        if (std::abs (freqT - freq) < 0.01) freq = freqT;

        qv += (qT - qv) * k;
        if (std::abs (qT - qv) < 0.0005) qv = qT;

        redesign();
    }

    /** True when this filter currently has no effect (gain-type filter at 0 dB). */
    bool isTransparent() const noexcept
    {
        return usesGain() && gain == 0.0 && gainT == 0.0;
    }

    inline float process (int ch, float x) noexcept { return state[(size_t) ch].process (x); }

    void reset() noexcept
    {
        for (auto& s : state)
            s.reset();
    }

    const BiquadCoeffs& coeffs() const noexcept { return state[0].c; }

private:
    bool usesGain() const noexcept
    {
        return type == FilterType::Peak || type == FilterType::LowShelf || type == FilterType::HighShelf;
    }

    void redesign() noexcept
    {
        const bool flat = usesGain() && gain == 0.0;
        const auto c = flat ? BiquadCoeffs {} : design::make (type, fs, freq, qv, gain);
        for (auto& s : state)
        {
            s.c = c;
            if (flat)
                s.reset();   // an identity section driven from rest keeps zero state
        }
    }

    double fs = 48000.0;
    FilterType type = FilterType::Peak;
    double freq = 1000.0, freqT = 1000.0, qv = 0.707, qT = 0.707, gain = 0.0, gainT = 0.0;
    std::array<Biquad, 2> state;
};

//==============================================================================
/** Linear parameter ramp updated per control block, read per sample. */
struct Ramp
{
    float current = 0.0f, target = 0.0f, step = 0.0f;
    int remaining = 0;

    void reset (float v) noexcept { current = target = v; step = 0.0f; remaining = 0; }

    /** Starts a ramp to v over 'samples' samples (call once per block). */
    void setTarget (float v, int samples) noexcept
    {
        if (v == target)
            return;
        target = v;
        remaining = std::max (1, samples);
        step = (target - current) / (float) remaining;
    }

    inline float next() noexcept
    {
        if (remaining > 0)
        {
            current += step;
            if (--remaining == 0)
                current = target;
        }
        return current;
    }

    bool isActive() const noexcept { return remaining > 0 || current != 0.0f; }
};

/** Smoothly approaches a target per block (exponential), for slowly varying values. */
struct Glide
{
    float value = 0.0f, target = 0.0f;
    void reset (float v) noexcept { value = target = v; }
    void advance (float coeff) noexcept
    {
        value = target + (value - target) * coeff;
        if (std::abs (value - target) < 1.0e-5f) value = target;
    }
};

} // namespace cinemax::dsp
