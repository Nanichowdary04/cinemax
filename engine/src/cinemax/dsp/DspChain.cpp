#include "DspChain.h"

#include <cstring>

namespace cinemax::dsp
{
void DspChain::prepare (double sampleRate, const ChannelLayout& layout)
{
    fs = sampleRate;
    inLayout = layout;

    analyzer.prepare (fs, inLayout);
    smartLow.prepare (fs, FilterType::LowShelf, 110.0, 0.7071, 0.0);
    smartHigh.prepare (fs, FilterType::HighShelf, 6000.0, 0.7071, 0.0);
    dialogue.prepare (fs, inLayout);
    spatial.prepare (fs, inLayout, kMaxBlock);
    eq.prepare (fs);
    ieq.prepare (fs);
    volumeModeler.prepare (fs);
    tvTuning.prepare (fs);
    lipSync.prepare (fs);
    bass.prepare (fs);
    loudness.prepare (fs);
    compressor.prepare (fs);
    limiter.prepare (fs, 1.5, 80.0);
    outputMeter.prepare (fs);

    dryL.assign (kMaxBlock, 0.0f);
    dryR.assign (kMaxBlock, 0.0f);
    wetL.assign (kMaxBlock, 0.0f);
    wetR.assign (kMaxBlock, 0.0f);
    {
        int size = 1;
        while (size < spatial.upmixer().latencySamples() + 2) size <<= 1;
        dryRingL.assign ((size_t) size, 0.0f);
        dryRingR.assign ((size_t) size, 0.0f);
        dryRingMask = size - 1;
        dryRingWrite = 0;
    }

    bassMeterLp.c = design::make (FilterType::LowPass, fs, 150.0, 0.7071, 0.0);
    meterCoeff = timeCoeff (fs / 64.0, 120.0);

    inputGainRamp.reset (1.0f);
    wetMix.reset (params.bypass ? 0.0f : 1.0f);

    setParams (params);
    reset();
}

void DspChain::reset() noexcept
{
    analyzer.reset();
    smartLow.reset();
    smartHigh.reset();
    dialogue.reset();
    spatial.reset();
    eq.reset();
    ieq.reset();
    volumeModeler.reset();
    tvTuning.reset();
    std::fill (dryRingL.begin(), dryRingL.end(), 0.0f);
    std::fill (dryRingR.begin(), dryRingR.end(), 0.0f);
    lipSync.reset();
    bass.reset();
    loudness.reset();
    compressor.reset();
    limiter.reset();
    outputMeter.reset();
    bassMeterLp.reset();
    bassEnergy = rmsL = rmsR = 0.0;
}

void DspChain::setParams (const ProcessingParams& p) noexcept
{
    params = p;
    const auto& u = p.user;

    // With the immersive up-mix the dialogue enhancer works on the extracted centre
    // (speech-gated, inside the up-mixer) instead of the whole mid signal.
    {
        DspParams du = u;
        if (u.immersive && u.surroundEnabled && (inLayout.isStereo() || inLayout.numChannels == 1))
            du.dialogueEnabled = false;
        dialogue.setParams (du);
    }
    spatial.setParams (u, p.headphones);
    spatial.setHrtf (p.hrtfSet);
    if (p.tv.isTv)
        spatial.setSpeakerGeometry (tvSpeakerHalfAngle (p.tv), p.tv.speakers == 3 ? 0.25f : p.tv.speakers == 2 ? 0.35f : 0.45f);
    else
        spatial.setSpeakerGeometry (10.1f, 0.45f);   // laptop speakers (the original tuning)
    tvTuning.setSetup (p.tv);
    lipSync.setDelayMs (p.tv.isTv ? p.tv.lipSyncMs : 0.0f);
    spatial.setAudition (p.auditionSet, p.auditionId);
    eq.setParams (u);
    ieq.setParams (u);
    bass.setParams (u, p.speakerHighPassHz);

    const bool night = u.preset == PresetId::Night;
    const bool leveling = u.dynamicsEnabled && u.loudnessEnabled;
    loudness.setParams (leveling, u.loudnessTargetLufs, night ? 12.0f : 9.0f);
    compressor.setAmount (u.dynamicsEnabled ? u.compressorAmount : 0.0f);
    // The normaliser after the compressor sets the final level; stacking make-up gain
    // on top of it made presets ~10 dB louder than their loudness target.
    compressor.setMakeupScale (leveling ? 0.0f : 1.0f);
    limiter.setCeilingDb (u.limiterCeilingDb);
}

void DspChain::process (float* const* in, int numSamples, float inputGain,
                        float* outL, float* outR, MeterBus* meters) noexcept
{
    const juce::ScopedNoDenormals noDenormals;   // decaying IIR tails must never go denormal

    inputGainRamp.setTarget (inputGain, (int) (0.02 * fs));
    wetMix.setTarget (params.bypass ? 0.0f : 1.0f, (int) (0.05 * fs));

    float* chunk[kMaxInputChannels] {};

    for (int start = 0; start < numSamples; start += kMaxBlock)
    {
        const int n = std::min (kMaxBlock, numSamples - start);
        for (int c = 0; c < inLayout.numChannels; ++c)
            chunk[c] = in[c] + start;

        processBlock (chunk, n, outL + start, outR + start, meters);
    }
}

void DspChain::processBlock (float* const* in, int n, float* outL, float* outR, MeterBus* meters) noexcept
{
    const int numCh = inLayout.numChannels;

    // --- input gain (Windows volume compensation) + input metering -----------------
    float inPeak = 0.0f;
    {
        const bool ramping = inputGainRamp.remaining > 0;
        const float g0 = inputGainRamp.current;
        float gains[kMaxBlock];
        for (int i = 0; i < n; ++i)
            gains[i] = ramping ? inputGainRamp.next() : g0;

        for (int c = 0; c < numCh; ++c)
        {
            float* x = in[c];
            for (int i = 0; i < n; ++i)
            {
                const float raw = std::abs (x[i]);
                if (raw >= 0.9999f && meters != nullptr)
                    meters->clip.store (true, std::memory_order_relaxed);   // source audio itself is clipping
                x[i] *= gains[i];
                inPeak = std::max (inPeak, std::abs (x[i]));
            }
        }
    }

    // --- content analysis of the untouched input (music/movie, tonal balance) -------
    analyzer.process (in, n);

    // --- dry path: plain stereo downmix (used for bypass) ---------------------------
    if (inLayout.isStereo())
    {
        std::memcpy (dryL.data(), in[0], sizeof (float) * (size_t) n);
        std::memcpy (dryR.data(), in[1], sizeof (float) * (size_t) n);
    }
    else if (numCh == 1)
    {
        std::memcpy (dryL.data(), in[0], sizeof (float) * (size_t) n);
        std::memcpy (dryR.data(), in[0], sizeof (float) * (size_t) n);
    }
    else
    {
        std::fill (dryL.begin(), dryL.begin() + n, 0.0f);
        std::fill (dryR.begin(), dryR.begin() + n, 0.0f);
        for (int c = 0; c < numCh; ++c)
        {
            float gl = 0.0f, gr = 0.0f;
            switch (inLayout.speakers[(size_t) c])
            {
                case Speaker::FL:  gl = 1.0f; break;
                case Speaker::FR:  gr = 1.0f; break;
                case Speaker::FC:  gl = gr = 0.7071f; break;
                case Speaker::LFE: gl = gr = 0.5f; break;
                case Speaker::BL:  case Speaker::SL: gl = 0.7071f; break;
                case Speaker::BR:  case Speaker::SR: gr = 0.7071f; break;
                case Speaker::FLC: gl = 0.92f; gr = 0.38f; break;
                case Speaker::FRC: gl = 0.38f; gr = 0.92f; break;
                case Speaker::BC:  gl = gr = 0.5f; break;
                case Speaker::TFL: case Speaker::TBL: gl = 0.7071f; break;
                case Speaker::TFR: case Speaker::TBR: gr = 0.7071f; break;
                case Speaker::TC:  case Speaker::TFC: case Speaker::TBC: gl = gr = 0.5f; break;
                case Speaker::Other: break;
            }
            for (int i = 0; i < n; ++i)
            {
                dryL[(size_t) i] += gl * in[c][i];
                dryR[(size_t) i] += gr * in[c][i];
            }
        }
    }

    // Align the dry path with the spatial stage (the immersive up-mix runs one frame late).
    {
        const int d = spatial.latencySamples();
        for (int i = 0; i < n; ++i)
        {
            dryRingL[(size_t) dryRingWrite] = dryL[(size_t) i];
            dryRingR[(size_t) dryRingWrite] = dryR[(size_t) i];
            const auto r = (size_t) ((dryRingWrite - d) & dryRingMask);
            dryL[(size_t) i] = dryRingL[r];
            dryR[(size_t) i] = dryRingR[r];
            dryRingWrite = (dryRingWrite + 1) & dryRingMask;
        }
    }

    // --- wet path ------------------------------------------------------------------
    dialogue.process (in, n);
    spatial.setSpeech ((float) analyzer.speechScore());
    if (headPose != nullptr && headPose->active.load (std::memory_order_relaxed))
        spatial.setHeadPose (headPose->yaw.load (std::memory_order_relaxed),
                             headPose->pitch.load (std::memory_order_relaxed));
    else
        spatial.setHeadPose (0.0f, 0.0f);
    spatial.process (in, wetL.data(), wetR.data(), n);
    ieq.analyse (wetL.data(), wetR.data(), n);      // the material itself, before the user's EQ
    eq.process (wetL.data(), wetR.data(), n);
    ieq.process (wetL.data(), wetR.data(), n);

    // --- smart tone: automatic bass / treble from the analyser (glides) -------------
    {
        const float sb = params.smartTone ? analyzer.smartBassDb() : 0.0f;
        const float st = params.smartTone ? analyzer.smartTrebleDb() : 0.0f;
        smartLow.setTarget (110.0, 0.7071, sb);
        smartHigh.setTarget (6000.0, 0.7071, st);

        for (int start = 0; start < n; start += 32)
        {
            const int len = std::min (32, n - start);
            smartLow.advance (len);
            smartHigh.advance (len);
            for (auto* f : { &smartLow, &smartHigh })
            {
                if (f->isTransparent())
                    continue;
                for (int i = start; i < start + len; ++i)
                {
                    wetL[(size_t) i] = f->process (0, wetL[(size_t) i]);
                    wetR[(size_t) i] = f->process (1, wetR[(size_t) i]);
                }
            }
        }
    }

    // --- volume-aware tone: how far the playback volume is turned down -----------------
    // Windows: the real device's volume (mirrored from CABLE Input; with a software-volume
    // cable the input gain undoes the same attenuation). Phone: the media volume, in the
    // parameters. Whichever says the level is lower.
    {
        const float att = std::max ({ gainToDb (inputGainRamp.current, 0.0f), -params.listenVolumeDb, -deviceVolumeDb });
        volumeModeler.setTarget (params.user.volumeModeler, att);
        volumeModeler.process (wetL.data(), wetR.data(), n);
    }

    bass.process (wetL.data(), wetR.data(), n);
    tvTuning.process (wetL.data(), wetR.data(), n);
    compressor.process (wetL.data(), wetR.data(), n);   // shape dynamics first ...
    loudness.process (wetL.data(), wetR.data(), n);     // ... then set the final loudness

    // --- bypass crossfade ------------------------------------------------------------
    for (int i = 0; i < n; ++i)
    {
        const float m = wetMix.next();
        outL[i] = dryL[(size_t) i] + m * (wetL[(size_t) i] - dryL[(size_t) i]);
        outR[i] = dryR[(size_t) i] + m * (wetR[(size_t) i] - dryR[(size_t) i]);
    }

    // --- ear-fit test: the test sound replaces the program while it plays -------------
    if (spatial.auditionActive())
        spatial.renderAudition (outL, outR, n);

    // --- peak protection -------------------------------------------------------------
    limiter.process (outL, outR, n);

    // --- lip-sync (TV picture runs late) ---------------------------------------------
    lipSync.process (outL, outR, n);

    // --- meters ----------------------------------------------------------------------
    if (meters == nullptr)
        return;

    float pkL = 0.0f, pkR = 0.0f;
    double sumL = 0.0, sumR = 0.0, sumB = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const float l = outL[i], r = outR[i];
        pkL = std::max (pkL, std::abs (l));
        pkR = std::max (pkR, std::abs (r));
        sumL += (double) l * l;
        sumR += (double) r * r;
        const float b = bassMeterLp.process (0.5f * (l + r));
        sumB += (double) b * b;
    }

    const double k = std::pow (meterCoeff, (double) n / 64.0);
    rmsL = sumL / n + (rmsL - sumL / n) * k;
    rmsR = sumR / n + (rmsR - sumR / n) * k;
    bassEnergy = sumB / n + (bassEnergy - sumB / n) * k;

    outputMeter.process (outL, outR, n);

    MeterBus::storeMax (meters->peakL, pkL);
    MeterBus::storeMax (meters->peakR, pkR);
    MeterBus::storeMax (meters->inputPeak, inPeak);
    meters->rmsDbL.store (gainToDb ((float) std::sqrt (rmsL), -100.0f), std::memory_order_relaxed);
    meters->rmsDbR.store (gainToDb ((float) std::sqrt (rmsR), -100.0f), std::memory_order_relaxed);
    meters->bassRms.store ((float) std::sqrt (bassEnergy), std::memory_order_relaxed);

    const float limGr = limiter.gainReductionDb();
    MeterBus::storeMax (meters->limiterGrDb, limGr);
    if (limGr > 0.1f)
        meters->limiting.store (true, std::memory_order_relaxed);
    if (pkL >= 0.9999f || pkR >= 0.9999f)
        meters->clip.store (true, std::memory_order_relaxed);

    meters->compressorGrDb.store (std::max (compressor.gainReductionDb(), bass.getGainReductionDb()), std::memory_order_relaxed);
    meters->loudnessLufs.store (outputMeter.momentaryLufs(), std::memory_order_relaxed);
    meters->movieProbability.store (analyzer.movieProbability(), std::memory_order_relaxed);
    meters->contentConfidence.store (analyzer.confidence(), std::memory_order_relaxed);
    meters->analysisSpeech.store ((float) analyzer.speechScore(), std::memory_order_relaxed);
    meters->analysisBeat.store ((float) analyzer.beatStrength(), std::memory_order_relaxed);
    meters->smartBassDb.store (params.smartTone ? analyzer.smartBassDb() : 0.0f, std::memory_order_relaxed);
    meters->smartTrebleDb.store (params.smartTone ? analyzer.smartTrebleDb() : 0.0f, std::memory_order_relaxed);

    meters->pushAnalysis (outL, outR, n);
}

} // namespace cinemax::dsp
