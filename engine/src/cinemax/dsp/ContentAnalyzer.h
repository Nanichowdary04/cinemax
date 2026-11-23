#pragma once

#include "ChannelLayout.h"
#include "Filters.h"

namespace cinemax::dsp
{
/** Real-time content analysis of the *input* (before any CINEMAX processing).

    1. Music vs. movie (heuristic, not a trained model). Evidence per 10 ms frame:
       - syllabic modulation: dialogue's voice-band envelope moves at ~2-8 Hz
       - pauses: speech and film soundtracks have frequent drops of >25 dB
       - loudness spread over 400 ms windows: films swing between whispers and
         explosions, mastered music stays within a few dB
       - rhythm: songs have a steady beat - the autocorrelation of note onsets
         (3-band positive spectral flux) over 6 s peaks at the beat period
       - active surround/LFE channels in a multichannel stream: almost always a film
       Speech evidence pushes towards film, a steady beat towards music. The
       result is a smoothed probability; the engine adds hysteresis on top.

    2. Smart tone: long-term bass (40-160 Hz) and treble (5-16 kHz) share of the
       signal compared with the typical balance of commercially mastered music.
       Half of the difference is corrected (limited, rate-limited to 1 dB/s), so a
       thin track gets more bass and a harsh one a softer top - never extreme.

    Allocation-free; everything happens in process(). */
class ContentAnalyzer
{
public:
    void prepare (double sampleRate, const ChannelLayout& layout);
    void reset() noexcept;
    void process (const float* const* in, int numSamples) noexcept;

    float movieProbability() const noexcept { return (float) pMovie; }
    /** 0..1: how much real (non-silent) audio has been analysed (1 = ~8 s). */
    float confidence() const noexcept { return (float) std::min (1.0, activeSeconds / 8.0); }
    float smartBassDb() const noexcept { return (float) bassGainDb; }
    float smartTrebleDb() const noexcept { return (float) trebleGainDb; }

    // Diagnostics (tests / logging)
    double beatStrength() const noexcept { return beat; }
    double speechScore() const noexcept { return speech; }
    double modulationDepthDb() const noexcept { return modDepthDb; }
    double pauseRatio() const noexcept { return pauseEma; }
    double loudnessSpreadDb() const noexcept { return std::sqrt (std::max (0.0, spreadVar)); }
    double bassShareDb() const noexcept { return bassShare; }
    double trebleShareDb() const noexcept { return trebleShare; }

    // Target balance of well-mastered music (long-term average spectrum).
    static constexpr double kBassTargetDb = -5.0;
    static constexpr double kTrebleTargetDb = -15.0;

private:
    void endFrame() noexcept;

    double fs = 48000.0;
    ChannelLayout layout;
    int frameLen = 480;
    int iL = 0, iR = 1, iC = -1;
    std::array<int, kMaxInputChannels> surroundIdx {};
    int numSurround = 0;

    Biquad voiceHp, voiceLp, bassHp, bassLp, trebHp, fullHp;
    double accVoice = 0, accFull = 0, accBass = 0, accTreb = 0, accSurround = 0, accFront = 0;
    int accN = 0;

    // frame-rate (100 Hz) analysis
    Biquad modBp;
    double modEnergy = 0, modDepthDb = 0;
    double runningMaxDb = -100, pauseEma = 0;
    int silentFrames = 0;
    double windowAcc = 0; int windowFrames = 0;
    double spreadMean = -30, spreadVar = 0; bool spreadInit = false;
    double surroundEma = 0;
    double bassE = 0, trebE = 0, fullE = 0;
    double bassShare = kBassTargetDb, trebleShare = kTrebleTargetDb;
    // rhythm: onset strength history (6 s at 100 Hz) and its autocorrelation peak
    static constexpr int kOnsetFrames = 600;
    std::array<float, kOnsetFrames> onset {}, linear {};
    int onsetPos = 0, onsetFilled = 0, beatCounter = 0;
    std::array<double, 3> prevBandDb { -100.0, -100.0, -100.0 };
    double beat = 0.0, speech = 0.0;

    double pMovie = 0.5, activeSeconds = 0;
    double bassGainDb = 0, trebleGainDb = 0;
};

} // namespace cinemax::dsp
