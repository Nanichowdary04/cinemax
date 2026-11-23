#include "Dynamics.h"

namespace cinemax::dsp
{
//==============================================================================
// LoudnessMeter
//==============================================================================
void LoudnessMeter::prepare (double sampleRate)
{
    fs = sampleRate;

    // K-weighting coefficients for arbitrary sample rates (same derivation as libebur128).
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan (kPi * f0 / fs);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        BiquadCoeffs c;
        c.b0 = (Vh + Vb * K / Q + K * K) / a0;
        c.b1 = 2.0 * (K * K - Vh) / a0;
        c.b2 = (Vh - Vb * K / Q + K * K) / a0;
        c.a1 = 2.0 * (K * K - 1.0) / a0;
        c.a2 = (1.0 - K / Q + K * K) / a0;
        preL.c = preR.c = c;
    }
    {
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan (kPi * f0 / fs);
        const double den = 1.0 + K / Q + K * K;
        BiquadCoeffs c;
        c.b0 = 1.0; c.b1 = -2.0; c.b2 = 1.0;
        c.a1 = 2.0 * (K * K - 1.0) / den;
        c.a2 = (1.0 - K / Q + K * K) / den;
        rlbL.c = rlbR.c = c;
    }

    blockSize = std::max (1, (int) std::lround (fs * 0.1));
    reset();
}

void LoudnessMeter::reset() noexcept
{
    preL.reset(); preR.reset(); rlbL.reset(); rlbR.reset();
    acc = 0.0;
    accCount = 0;
    history.fill (0.0);
    historyPos = historyFilled = 0;
    shortTerm = momentary = -70.0f;
    integratedSum = 0.0;
    integratedBlocks = 0;
}

void LoudnessMeter::process (const float* left, const float* right, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const double l = rlbL.process (preL.process (left[i]));
        const double r = rlbR.process (preR.process (right[i]));
        acc += l * l + r * r;

        if (++accCount >= blockSize)
        {
            history[(size_t) historyPos] = acc / (double) accCount;
            if (acc / (double) accCount > 1.17e-7)   // absolute gate at -70 LUFS
            {
                integratedSum += acc / (double) accCount;
                ++integratedBlocks;
            }
            historyPos = (historyPos + 1) % (int) history.size();
            historyFilled = std::min (historyFilled + 1, (int) history.size());
            acc = 0.0;
            accCount = 0;
            ++blocks;

            auto meanOfLast = [this] (int count)
            {
                count = std::min (count, historyFilled);
                if (count == 0) return 0.0;
                double s = 0.0;
                for (int k = 1; k <= count; ++k)
                    s += history[(size_t) ((historyPos - k + (int) history.size()) % (int) history.size())];
                return s / count;
            };

            auto toLufs = [] (double ms) { return ms > 1.0e-12 ? (float) (-0.691 + 10.0 * std::log10 (ms)) : -70.0f; };
            momentary = std::max (-70.0f, toLufs (meanOfLast (4)));
            shortTerm = std::max (-70.0f, toLufs (meanOfLast (30)));
        }
    }
}

//==============================================================================
// LoudnessNormalizer
//==============================================================================
void LoudnessNormalizer::prepare (double sampleRate)
{
    fs = sampleRate;
    meter.prepare (fs);
    reset();
}

void LoudnessNormalizer::reset() noexcept
{
    meter.reset();
    gainDb = desiredDb = 0.0f;
    gain.reset (1.0f);
    lastBlock = meter.blockCounter();
}

void LoudnessNormalizer::setParams (bool on, float targetLufs, float maxBoostDb) noexcept
{
    enabled = on;
    target = targetLufs;
    maxBoost = maxBoostDb;
}

void LoudnessNormalizer::process (float* left, float* right, int n) noexcept
{
    meter.process (left, right, n);   // measure the *input* (open loop, cannot oscillate)

    if (meter.blockCounter() != lastBlock)
    {
        const unsigned newBlocks = meter.blockCounter() - lastBlock;
        lastBlock = meter.blockCounter();

        const float loud = meter.shortTermLufs();
        if (enabled)
        {
            // Gate: below -50 LUFS hold the gain (don't amplify silence / noise floor).
            if (loud > -50.0f)
                desiredDb = std::clamp (target - loud, -12.0f, maxBoost);
        }
        else
        {
            desiredDb = 0.0f;
        }

        // Rate limits per 100 ms block: fall fast (6 dB/s), rise slowly (1.5 dB/s).
        const float dt = 0.1f * (float) newBlocks;
        const float maxDown = 6.0f * dt, maxUp = 1.5f * dt;
        const float delta = std::clamp (desiredDb - gainDb, -maxDown, maxUp);
        gainDb += delta;
    }

    gain.setTarget (dbToGain (gainDb), (int) (0.1 * fs));

    if (gain.remaining == 0 && gain.current == 1.0f)
        return;

    for (int i = 0; i < n; ++i)
    {
        const float g = gain.next();
        left[i] *= g;
        right[i] *= g;
    }
}

//==============================================================================
// Compressor
//==============================================================================
void Compressor::prepare (double sampleRate)
{
    fs = sampleRate;
    scL.c = scR.c = design::make (FilterType::HighPass, fs, 90.0, 0.7071, 0.0);
    envCoeff = timeCoeff (fs, 5.0);
    attack = timeCoeff (fs, 12.0);
    release = timeCoeff (fs, 180.0);
    amountCoeff = (float) timeCoeff (fs / 32.0, 50.0);
    amount.reset (0.0f);
    reset();
}

void Compressor::reset() noexcept
{
    scL.reset();
    scR.reset();
    env = 0.0;
    grDb = 0.0;
    grReport = 0.0f;
}

void Compressor::process (float* left, float* right, int n) noexcept
{
    amount.target = amountTarget;

    if (amount.value == 0.0f && amount.target == 0.0f && grDb < 0.01)
    {
        grDb = 0.0;
        grReport = 0.0f;
        return;
    }

    float maxGr = 0.0f;

    for (int start = 0; start < n; start += 32)
    {
        const int len = std::min (32, n - start);
        amount.advance (amountCoeff);

        const double a = amount.value;
        const double threshold = -12.0 - 18.0 * a;        // dBFS
        const double ratio = 1.0 + 5.0 * a;               // 1:1 .. 6:1
        const double knee = 8.0;
        const double makeup = (-threshold) * (1.0 - 1.0 / ratio) * 0.45 * makeupScale;

        for (int i = start; i < start + len; ++i)
        {
            const double sl = scL.process (left[i]);
            const double sr = scR.process (right[i]);
            const double power = std::max (sl * sl, sr * sr);
            env = power + (env - power) * envCoeff;

            const double levelDb = 10.0 * std::log10 (env + 1.0e-12);
            const double over = levelDb - threshold;

            double outDb;
            if (2.0 * over < -knee)              outDb = levelDb;
            else if (2.0 * std::abs (over) <= knee)
            {
                const double t = over + knee / 2.0;
                outDb = levelDb + (1.0 / ratio - 1.0) * t * t / (2.0 * knee);
            }
            else                                 outDb = threshold + over / ratio;

            const double wantGr = levelDb - outDb;   // >= 0
            grDb = wantGr > grDb ? wantGr + (grDb - wantGr) * attack
                                 : wantGr + (grDb - wantGr) * release;

            const float g = (float) std::pow (10.0, (makeup - grDb) / 20.0);
            left[i] *= g;
            right[i] *= g;
        }

        maxGr = std::max (maxGr, (float) grDb);
    }

    grReport = maxGr;
}

//==============================================================================
// Limiter
//==============================================================================
void Limiter::prepare (double sampleRate, double lookaheadMs, double releaseMs)
{
    fs = sampleRate;
    lookahead = std::max (1, (int) std::lround (lookaheadMs * 0.001 * fs));
    window = lookahead + 1;

    // Output sample p = n - lookahead; the gains averaged at time n (window = lookahead + 1
    // values) each come from a minimum whose window contains p, so g <= required[p].
    delayL.assign ((size_t) lookahead, 0.0f);
    delayR.assign ((size_t) lookahead, 0.0f);

    int dqSize = 1;
    while (dqSize < window + 2) dqSize <<= 1;
    dqValue.assign ((size_t) dqSize, 1.0f);
    dqIndex.assign ((size_t) dqSize, 0);
    dqMask = dqSize - 1;

    avgRing.assign ((size_t) window, 1.0f);
    releaseCoeff = timeCoeff (fs, releaseMs);
    ceiling = ceilingTarget;
    reset();
}

void Limiter::reset() noexcept
{
    std::fill (delayL.begin(), delayL.end(), 0.0f);
    std::fill (delayR.begin(), delayR.end(), 0.0f);
    delayPos = 0;
    dqHead = dqTail = 0;
    sampleIndex = 0;
    std::fill (avgRing.begin(), avgRing.end(), 1.0f);
    avgPos = 0;
    avgSum = (double) window;
    avgRecalc = 0;
    released = 1.0;
    grReport = 0.0f;
}

void Limiter::process (float* left, float* right, int n) noexcept
{
    // A lower ceiling applies immediately (the guarantee must hold at once); a higher
    // ceiling is approached over ~50 ms so the gain is released smoothly.
    if (ceilingTarget <= ceiling)
    {
        ceiling = ceilingTarget;
    }
    else
    {
        ceiling += (ceilingTarget - ceiling) * std::min (1.0f, (float) n / (float) (0.05 * fs));
        if (ceilingTarget - ceiling < 1.0e-6f)
            ceiling = ceilingTarget;
    }

    float minGain = 1.0f;

    for (int i = 0; i < n; ++i)
    {
        const float xl = left[i], xr = right[i];
        const float peak = std::max (std::abs (xl), std::abs (xr));
        const float required = peak > ceiling ? ceiling / peak : 1.0f;

        // --- sliding minimum over the last 'window' required gains ---------------
        while (dqTail != dqHead && dqValue[(size_t) ((dqTail - 1) & dqMask)] >= required)
            dqTail = (dqTail - 1) & dqMask;
        dqValue[(size_t) dqTail] = required;
        dqIndex[(size_t) dqTail] = sampleIndex;
        dqTail = (dqTail + 1) & dqMask;
        while (dqIndex[(size_t) dqHead] <= sampleIndex - window)
            dqHead = (dqHead + 1) & dqMask;
        const float windowMin = dqValue[(size_t) dqHead];
        ++sampleIndex;
        if (sampleIndex > (1 << 30))
        {
            // Rebase indices to avoid overflow (rare; keeps the deque consistent).
            for (int k = dqHead; k != dqTail; k = (k + 1) & dqMask)
                dqIndex[(size_t) k] -= (1 << 29);
            sampleIndex -= (1 << 29);
        }

        // --- instant attack, smooth release ---------------------------------------
        if (windowMin < released) released = windowMin;
        else                      released = windowMin + (released - windowMin) * releaseCoeff;

        // --- moving average over the window ---------------------------------------
        avgSum += released - avgRing[(size_t) avgPos];
        avgRing[(size_t) avgPos] = (float) released;
        avgPos = (avgPos + 1) % window;
        if (++avgRecalc >= 8192)
        {
            avgRecalc = 0;
            double s = 0.0;
            for (float v : avgRing) s += v;
            avgSum = s;
        }
        const float g = std::min ((float) (avgSum / (double) window), 1.0f);

        // --- delay the audio by the look-ahead and apply --------------------------
        const float dl = delayL[(size_t) delayPos];
        const float dr = delayR[(size_t) delayPos];
        delayL[(size_t) delayPos] = xl;
        delayR[(size_t) delayPos] = xr;
        delayPos = (delayPos + 1) % lookahead;

        float yl = dl * g, yr = dr * g;

        // Final guard against float rounding at the ceiling.
        yl = std::clamp (yl, -ceiling, ceiling);
        yr = std::clamp (yr, -ceiling, ceiling);

        left[i] = yl;
        right[i] = yr;
        minGain = std::min (minGain, g);
    }

    grReport = -gainToDb (minGain);
}

} // namespace cinemax::dsp
