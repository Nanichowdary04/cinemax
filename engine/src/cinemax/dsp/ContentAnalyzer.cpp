#include "ContentAnalyzer.h"

namespace cinemax::dsp
{
namespace
{
    constexpr double kFrameRate = 100.0;   // 10 ms frames

    double ema (double frameSeconds) { return 1.0 / (frameSeconds * kFrameRate); }
    double toDb (double e) { return 10.0 * std::log10 (e + 1.0e-12); }
}

void ContentAnalyzer::prepare (double sampleRate, const ChannelLayout& l)
{
    fs = sampleRate;
    layout = l;
    frameLen = std::max (16, (int) std::lround (fs / kFrameRate));

    iL = layout.indexOf (Speaker::FL);
    iR = layout.indexOf (Speaker::FR);
    iC = layout.indexOf (Speaker::FC);
    if (layout.numChannels == 1) { iL = iR = 0; iC = -1; }
    if (iL < 0) iL = 0;
    if (iR < 0) iR = std::min (1, layout.numChannels - 1);

    numSurround = 0;
    for (int ch = 0; ch < layout.numChannels; ++ch)
    {
        const auto s = layout.speakers[(size_t) ch];
        if (s != Speaker::FL && s != Speaker::FR && s != Speaker::FC && s != Speaker::Other)
            surroundIdx[(size_t) numSurround++] = ch;
    }

    voiceHp.c = design::make (FilterType::HighPass, fs, 300.0, 0.7071, 0.0);
    voiceLp.c = design::make (FilterType::LowPass, fs, 3400.0, 0.7071, 0.0);
    bassHp.c  = design::make (FilterType::HighPass, fs, 40.0, 0.7071, 0.0);
    bassLp.c  = design::make (FilterType::LowPass, fs, 160.0, 0.7071, 0.0);
    trebHp.c  = design::make (FilterType::HighPass, fs, 5000.0, 0.7071, 0.0);
    fullHp.c  = design::make (FilterType::HighPass, fs, 30.0, 0.7071, 0.0);
    // Syllabic band (~2-12 Hz, centred at 5 Hz) of the 100 Hz frame-rate envelope.
    modBp.c   = design::make (FilterType::BandPass, kFrameRate, 5.0, 0.7, 0.0);

    reset();
}

void ContentAnalyzer::reset() noexcept
{
    for (auto* b : { &voiceHp, &voiceLp, &bassHp, &bassLp, &trebHp, &fullHp, &modBp })
        b->reset();
    accVoice = accFull = accBass = accTreb = accSurround = accFront = 0.0;
    accN = 0;
    modEnergy = modDepthDb = 0.0;
    runningMaxDb = -100.0;
    pauseEma = 0.0;
    silentFrames = 0;
    windowAcc = 0.0; windowFrames = 0;
    spreadMean = -30.0; spreadVar = 0.0; spreadInit = false;
    surroundEma = 0.0;
    bassE = trebE = fullE = 0.0;
    bassShare = kBassTargetDb; trebleShare = kTrebleTargetDb;
    onset.fill (0.0f);
    onsetPos = onsetFilled = beatCounter = 0;
    prevBandDb = { -100.0, -100.0, -100.0 };
    beat = speech = 0.0;
    pMovie = 0.5; activeSeconds = 0.0;
    bassGainDb = trebleGainDb = 0.0;
}

void ContentAnalyzer::process (const float* const* in, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float l = in[iL][i], r = in[iR][i];
        const float m = 0.5f * (l + r);
        const float dialog = iC >= 0 ? in[iC][i] : m;   // centre channel carries film dialogue

        const double v = voiceLp.process (voiceHp.process (dialog));
        const double f = fullHp.process (m);
        const double b = bassLp.process (bassHp.process (m));
        const double t = trebHp.process (m);

        accVoice += v * v;
        accFull += f * f;
        accBass += b * b;
        accTreb += t * t;
        accFront += (double) l * l + (double) r * r + (iC >= 0 ? (double) in[iC][i] * in[iC][i] : 0.0);

        for (int k = 0; k < numSurround; ++k)
        {
            const double s = in[surroundIdx[(size_t) k]][i];
            accSurround += s * s;
        }

        if (++accN >= frameLen)
            endFrame();
    }
}

void ContentAnalyzer::endFrame() noexcept
{
    const double inv = 1.0 / accN;
    const double eFull = accFull * inv, eVoice = accVoice * inv, eBass = accBass * inv, eTreb = accTreb * inv;
    const double eSurr = accSurround * inv, eFront = accFront * inv;
    accVoice = accFull = accBass = accTreb = accSurround = accFront = 0.0;
    accN = 0;

    const double frameDb = toDb (eFull);

    // Digital silence for > 0.5 s = nothing playing: hold all statistics.
    if (frameDb < -70.0)
    {
        if (++silentFrames > 50)
            return;
    }
    else
    {
        silentFrames = 0;
        activeSeconds += 1.0 / kFrameRate;
    }

    // --- pauses ------------------------------------------------------------------------
    runningMaxDb = std::max (frameDb, runningMaxDb - 0.05);   // 5 dB/s release
    const double isPause = frameDb < runningMaxDb - 25.0 ? 1.0 : 0.0;
    pauseEma += (isPause - pauseEma) * ema (4.0);

    // --- syllabic modulation of the voice band -----------------------------------------
    const double voiceDb = std::max (-80.0, toDb (eVoice));
    const double y = modBp.process ((float) voiceDb);
    modEnergy += (y * y - modEnergy) * ema (2.5);
    modDepthDb = std::sqrt (2.0 * modEnergy);

    // --- loudness spread over 400 ms windows -------------------------------------------
    windowAcc += eFull;
    if (++windowFrames >= 40)
    {
        const double wDb = toDb (windowAcc / windowFrames);
        windowAcc = 0.0;
        windowFrames = 0;

        if (wDb > -60.0)
        {
            if (! spreadInit) { spreadMean = wDb; spreadInit = true; }
            const double a = 1.0 / 15.0;   // ~6 s at 2.5 windows/s
            const double d = wDb - spreadMean;
            spreadMean += a * d;
            spreadVar += a * (d * d - spreadVar);
        }
    }

    // --- surround / LFE activity --------------------------------------------------------
    if (numSurround > 0 && frameDb > -60.0)
        surroundEma += (eSurr / (eSurr + eFront + 1.0e-12) - surroundEma) * ema (4.0);

    // --- long-term spectral balance (non-silent frames only) ----------------------------
    if (frameDb > -60.0)
    {
        const double a = ema (6.0);
        bassE += (eBass - bassE) * a;
        trebE += (eTreb - trebE) * a;
        fullE += (eFull - fullE) * a;
        bassShare = toDb (bassE) - toDb (fullE);
        trebleShare = toDb (trebE) - toDb (fullE);
    }

    // --- rhythm: onset strength (positive flux in 3 bands) and its periodicity ----------
    {
        const double bands[3] { toDb (eBass), toDb (eVoice), toDb (eTreb) };
        double o = 0.0;
        for (size_t b = 0; b < 3; ++b)
        {
            o += std::clamp (bands[b] - prevBandDb[b], 0.0, 20.0);
            prevBandDb[b] = bands[b];
        }
        onset[(size_t) onsetPos] = frameDb > -60.0 ? (float) o : 0.0f;
        onsetPos = (onsetPos + 1) % kOnsetFrames;
        onsetFilled = std::min (onsetFilled + 1, kOnsetFrames);

        // Autocorrelation every 250 ms over lags 0.3 - 1.5 s (40 - 200 BPM).
        if (++beatCounter >= 25 && onsetFilled >= 300)
        {
            beatCounter = 0;
            const int n = onsetFilled;

            // Unroll the ring (oldest -> newest) into a mean-removed linear buffer.
            double mean = 0.0;
            for (int j = 0; j < n; ++j)
            {
                linear[(size_t) j] = onset[(size_t) ((onsetPos - n + j + kOnsetFrames) % kOnsetFrames)];
                mean += linear[(size_t) j];
            }
            mean /= n;
            double var = 0.0;
            for (int j = 0; j < n; ++j)
            {
                linear[(size_t) j] -= (float) mean;
                var += (double) linear[(size_t) j] * linear[(size_t) j];
            }

            double best = 0.0;
            if (var > 1.0e-9)
            {
                for (int lag = 30; lag <= 150; ++lag)
                {
                    double acc = 0.0;
                    for (int j = lag; j < n; ++j)
                        acc += (double) linear[(size_t) j] * linear[(size_t) (j - lag)];
                    best = std::max (best, acc / var * (double) n / (double) (n - lag));
                }
            }
            beat += (std::clamp (best, 0.0, 1.0) - beat) * 0.12;   // ~2 s smoothing
        }
    }

    // --- music vs movie probability ------------------------------------------------------
    // Speech-like evidence (dialogue) pushes towards film; a steady beat towards music.
    const double sMod = std::clamp ((modDepthDb - 2.5) / 6.0, 0.0, 1.0);
    const double sPause = std::clamp ((pauseEma - 0.03) / 0.15, 0.0, 1.0);
    const double sSpread = std::clamp ((std::sqrt (std::max (0.0, spreadVar)) - 2.5) / 4.5, 0.0, 1.0);
    speech = 0.45 * sMod + 0.3 * sPause + 0.25 * sSpread;
    const double sBeat = std::clamp ((beat - 0.12) / 0.25, 0.0, 1.0);

    double raw = std::clamp (0.48 + 0.8 * speech - 0.65 * sBeat, 0.0, 1.0);
    if (surroundEma > 0.08)
        raw = std::max (raw, 0.85);   // a stream with active surround/LFE channels is a film
    if (frameDb > -60.0)
        pMovie += (raw - pMovie) * ema (2.0);

    // --- smart tone targets (half correction, limited, 1 dB/s) --------------------------
    if (activeSeconds > 2.0 && frameDb > -60.0)
    {
        // Films: barely add bass (under dialogue it just sounds boomy).
        const double maxBassBoost = 5.0 - 4.0 * std::clamp (pMovie, 0.0, 1.0);
        // Shares compress near 0 dB (bass can't exceed 100 % of the power), so an excess
        // reads small: cut more strongly than we boost. Boosts stay gentle (half).
        const double bassErr = kBassTargetDb - bassShare;
        const double trebErr = kTrebleTargetDb - trebleShare;
        const double wantBass = std::clamp (bassErr > 0.0 ? 0.5 * bassErr : 1.5 * bassErr, -4.0, maxBassBoost);
        const double wantTreb = std::clamp (trebErr > 0.0 ? 0.5 * trebErr : 0.8 * trebErr, -4.0, 3.0);
        constexpr double step = 1.0 / kFrameRate;   // 1 dB per second
        bassGainDb += std::clamp (wantBass - bassGainDb, -step, step);
        trebleGainDb += std::clamp (wantTreb - trebleGainDb, -step, step);
    }
}

} // namespace cinemax::dsp
