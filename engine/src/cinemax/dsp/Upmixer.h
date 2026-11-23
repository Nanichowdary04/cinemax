// Immersive up-mixer: stereo -> a 7.1.4 speaker bed, the heart of DOLBY-STYLE.
#pragma once

#include "Fft.h"
#include "Filters.h"

#include <array>
#include <complex>
#include <vector>

namespace cinemax::dsp
{
/** Splits a stereo signal into front, centre, surround and height feeds, frequency by
    frequency (short-time Fourier transform, ~11 ms frames at 48 kHz, 50 % overlap,
    sqrt-Hann windows: with every mask at 1 the output is the input, delayed).

    For every frequency bin the smoothed 2x2 covariance of L/R is decomposed
    (principal components, as in Goodwin & Jot's primary-ambient extraction):
      - primary (directional) sound: the coherent part. Its pan position comes from the
        principal direction and it is re-panned over three front speakers, so voices
        mixed in the middle play from a real centre speaker instead of a phantom image;
      - ambience (diffuse) sound: the uncorrelated part, sent to the surround and
        (above ~1 kHz, scaled by the height amount) the overhead feeds;
      - anti-phase primary sound (matrix-encoded surround in Lt/Rt film mixes, as decoded
        by Pro Logic II-style upmixers) goes to the surrounds.
    Below ~120 Hz nothing is split: bass stays in the front pair, solid and in phase.

    Dialogue: while there is speech (content analyser evidence) and the voice band is
    concentrated in the centre, the centre gets a presence lift and the ambience can be
    turned down - so it acts on voices, not on the music around them.

    Real-time safe after prepare(); output is independent of the block size. */
class ImmersiveUpmixer
{
public:
    enum Feed { FrontL, FrontR, Centre, SurL, SurR, TopL, TopR, kFeeds };

    void prepare (double sampleRate);
    void reset() noexcept;

    /** Latency of every feed relative to the input, in samples (one STFT frame). */
    int latencySamples() const noexcept { return frame; }

    /** height 0..1 share of ambience for the ceiling speakers; dialogue 0..1 centre voice
        lift while speech plays; backgroundDb <= 0 ambience level while speech plays. */
    void setParams (float height, float dialogue, float backgroundDb) noexcept;

    /** Content analyser speech evidence 0..1 (called once per block). */
    void setSpeech (float speech01) noexcept { speechEvidence = std::clamp (speech01, 0.0f, 1.0f); }

    /** out: kFeeds buffers of n samples. */
    void process (const float* inL, const float* inR, float* const* out, int n) noexcept;

    // Diagnostics (tests)
    float centreShare() const noexcept { return (float) centreShareSmoothed; }
    float speechGate() const noexcept { return (float) gate; }

private:
    using Complex = std::complex<float>;
    void processFrame() noexcept;

    double fs = 48000.0;
    int frame = 512, hop = 256, bins = 257, pos = 0;
    Fft fft;
    std::vector<float> window, bufL, bufR;
    std::array<std::vector<float>, kFeeds> acc, ready;
    std::vector<Complex> spec, work;
    std::array<std::vector<Complex>, kFeeds> feed;

    // smoothed per-bin statistics
    std::vector<float> sll, srr, pll, prr;
    std::vector<Complex> slr, plr, xl, xr;
    float alpha = 0.9f;

    // per-bin weights
    std::vector<float> splitW, topW, voiceW, voiceBand;

    // parameters
    float heightAmount = 0.5f, dialogueAmount = 0.0f, backgroundDb = 0.0f;
    float speechEvidence = 0.0f;
    double centreShareSmoothed = 0.0, gate = 0.0;
    double gateAttack = 0.5, gateRelease = 0.05, shareCoeff = 0.2;
};

} // namespace cinemax::dsp
