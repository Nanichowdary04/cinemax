// CINEMAX DSP test-suite. Pure offline tests: no audio devices involved.
// Verifies filter maths, transparency, limiter guarantee, stability of every preset
// in every channel layout / sample rate, resampler accuracy, loudness metering and
// that the real-time path performs zero heap allocations.
#include <juce_dsp/juce_dsp.h>

#include "cinemax/core/Params.h"
#include "cinemax/dsp/DspChain.h"
#include "cinemax/dsp/Hrtf.h"
#include "cinemax/dsp/IntelligentEq.h"
#include "cinemax/dsp/TvTuning.h"
#include "cinemax/dsp/Upmixer.h"
#include "cinemax/core/Settings.h"
#include "cinemax/dsp/Resampler.h"
#include "cinemax/win/AudioSessions.h"
#include "cinemax/win/Endpoints.h"
#include "cinemax/vision/Picture.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

using namespace cinemax;
using namespace cinemax::dsp;

//==============================================================================
// Allocation tracking: counts heap allocations made by the current thread while armed.
namespace
{
    thread_local bool trackAllocations = false;
    thread_local long allocationCount = 0;
}

void* operator new (std::size_t n)
{
    if (trackAllocations) ++allocationCount;
    if (void* p = std::malloc (n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n)
{
    if (trackAllocations) ++allocationCount;
    if (void* p = std::malloc (n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept                 { std::free (p); }
void operator delete[] (void* p) noexcept               { std::free (p); }
void operator delete (void* p, std::size_t) noexcept    { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept  { std::free (p); }

//==============================================================================
namespace
{
    int failures = 0, checks = 0;

    void check (bool ok, const char* what, double value = 0.0)
    {
        ++checks;
        if (! ok)
        {
            ++failures;
            std::printf ("  FAIL: %s (value %.6g)\n", what, value);
        }
    }

    struct Signal
    {
        std::vector<std::vector<float>> ch;
        int frames = 0;
        float* ptrs[kMaxInputChannels] {};

        Signal (int channels, int n) : ch ((size_t) channels, std::vector<float> ((size_t) n, 0.0f)), frames (n)
        {
            for (int c = 0; c < channels; ++c)
                ptrs[c] = ch[(size_t) c].data();
        }
    };

    void fillNoise (Signal& s, float rmsDb, unsigned seed)
    {
        std::mt19937 rng (seed);
        std::normal_distribution<float> nd (0.0f, std::pow (10.0f, rmsDb / 20.0f));
        for (auto& c : s.ch)
            for (auto& v : c)
                v = nd (rng);
    }

    void fillSine (Signal& s, double freq, double fs, float peak, double phaseOffsetPerChannel = 0.0)
    {
        for (size_t c = 0; c < s.ch.size(); ++c)
            for (int i = 0; i < s.frames; ++i)
                s.ch[c][(size_t) i] = peak * (float) std::sin (2.0 * kPi * freq * i / fs + phaseOffsetPerChannel * (double) c);
    }

    /** Processes a whole signal through a chain in blocks of 'block'. */
    void runChain (DspChain& chain, Signal& in, std::vector<float>& outL, std::vector<float>& outR, int block, MeterBus* meters = nullptr)
    {
        outL.assign ((size_t) in.frames, 0.0f);
        outR.assign ((size_t) in.frames, 0.0f);
        float* ptrs[kMaxInputChannels] {};

        for (int pos = 0; pos < in.frames; pos += block)
        {
            const int n = std::min (block, in.frames - pos);
            for (size_t c = 0; c < in.ch.size(); ++c)
                ptrs[c] = in.ch[c].data() + pos;
            chain.process (ptrs, n, 1.0f, outL.data() + pos, outR.data() + pos, meters);
        }
    }

    double rmsDb (const std::vector<float>& x, size_t from)
    {
        double s = 0.0;
        for (size_t i = from; i < x.size(); ++i) s += (double) x[i] * x[i];
        return 10.0 * std::log10 (s / (double) (x.size() - from) + 1e-30);
    }

    float peakAbs (const std::vector<float>& x)
    {
        float p = 0.0f;
        for (float v : x) p = std::max (p, std::abs (v));
        return p;
    }

    bool allFinite (const std::vector<float>& x)
    {
        for (float v : x)
            if (! std::isfinite (v)) return false;
        return true;
    }
}

//==============================================================================
static void testFilterMaths()
{
    std::printf ("[filters] RBJ designs\n");
    const double fs = 48000.0;

    auto peak = design::make (FilterType::Peak, fs, 1000.0, 1.41, 6.0);
    check (std::abs (magnitudeDb (peak, 1000.0, fs) - 6.0) < 0.01, "peak +6 dB at centre", magnitudeDb (peak, 1000.0, fs));
    check (std::abs (magnitudeDb (peak, 20.0, fs)) < 0.05, "peak flat far below", magnitudeDb (peak, 20.0, fs));

    auto ls = design::make (FilterType::LowShelf, fs, 100.0, 0.7071, 9.0);
    check (std::abs (magnitudeDb (ls, 10.0, fs) - 9.0) < 0.1, "low shelf +9 dB at DC side", magnitudeDb (ls, 10.0, fs));
    check (std::abs (magnitudeDb (ls, 5000.0, fs)) < 0.05, "low shelf flat above", magnitudeDb (ls, 5000.0, fs));

    auto hp = design::make (FilterType::HighPass, fs, 80.0, 0.7071, 0.0);
    check (std::abs (magnitudeDb (hp, 80.0, fs) + 3.01) < 0.05, "Butterworth HP -3 dB at fc", magnitudeDb (hp, 80.0, fs));
}

static void testTripleBufferAndLayout()
{
    std::printf ("[core] triple buffer, channel layouts\n");
    TripleBuffer<ProcessingParams> tb;
    ProcessingParams out;
    check (! tb.read (out), "empty triple buffer has nothing to read");
    ProcessingParams a; a.user.preampDb = -3.0f;
    ProcessingParams b; b.user.preampDb = -5.0f;
    tb.write (a);
    tb.write (b);
    check (tb.read (out) && out.user.preampDb == -5.0f, "reader gets latest value", out.user.preampDb);
    check (! tb.read (out), "no new value after read");

    const auto l51 = ChannelLayout::fromMask (0x3F, 6);
    check (l51.speakers[2] == Speaker::FC && l51.speakers[3] == Speaker::LFE && l51.speakers[5] == Speaker::BR, "5.1 mask mapping");
    const auto l71 = ChannelLayout::fromMask (0x63F, 8);
    check (l71.speakers[6] == Speaker::SL && l71.speakers[7] == Speaker::SR, "7.1 mask mapping");
    check (ChannelLayout::fromMask (0, 2).isStereo(), "default stereo layout");
}

static void testNeutralTransparency()
{
    std::printf ("[chain] neutral preset is transparent (limiter latency only)\n");
    for (double fs : { 44100.0, 48000.0 })
    {
        DspChain chain;
        chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
        ProcessingParams pp;
        pp.user = makePreset (PresetId::Neutral);
        chain.setParams (pp);

        Signal in (2, (int) fs);
        fillNoise (in, -24.0f, 7);
        const Signal copy = in;
        std::vector<float> l, r;
        runChain (chain, in, l, r, 480);

        const int lat = chain.latencySamples();
        double maxErr = 0.0;
        for (int i = lat; i < in.frames; ++i)
        {
            maxErr = std::max (maxErr, (double) std::abs (l[(size_t) i] - copy.ch[0][(size_t) (i - lat)]));
            maxErr = std::max (maxErr, (double) std::abs (r[(size_t) i] - copy.ch[1][(size_t) (i - lat)]));
        }
        check (maxErr < 1.0e-5, "neutral output == delayed input", maxErr);
    }
}

static void testEqAccuracy()
{
    std::printf ("[eq] band gain at centre frequency\n");
    const double fs = 48000.0;
    DspChain chain;
    chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
    ProcessingParams pp;
    pp.user = makePreset (PresetId::Neutral);
    pp.user.eqBands[5].gainDb = 6.0f;     // 1 kHz
    chain.setParams (pp);

    Signal in (2, (int) fs);
    fillSine (in, 1000.0, fs, 0.1f);
    std::vector<float> l, r;
    runChain (chain, in, l, r, 256);

    const double gain = rmsDb (l, (size_t) (fs * 0.5)) - (20.0 * std::log10 (0.1 / std::sqrt (2.0)));
    check (std::abs (gain - 6.0) < 0.15, "1 kHz band +6 dB", gain);
}

static void testLimiterGuarantee()
{
    std::printf ("[limiter] output never exceeds the ceiling\n");
    const double fs = 48000.0;
    for (float ceilingDb : { -1.0f, -3.0f, -0.1f })
    {
        Limiter lim;
        lim.prepare (fs, 1.5, 80.0);
        lim.setCeilingDb (ceilingDb);
        // let the ceiling glide settle
        std::vector<float> warm (4800, 0.0f), warm2 (4800, 0.0f);
        for (int k = 0; k < 40; ++k) lim.process (warm.data(), warm2.data(), 120);

        Signal in (2, (int) fs * 2);
        fillNoise (in, 6.0f, 3);                     // ~ +6 dBFS RMS: brutal overload
        for (int i = 20000; i < 20040; ++i) in.ch[0][(size_t) i] = 30.0f;   // isolated spikes
        lim.process (in.ch[0].data(), in.ch[1].data(), in.frames);

        const float ceiling = std::pow (10.0f, ceilingDb / 20.0f);
        const float pk = std::max (peakAbs (in.ch[0]), peakAbs (in.ch[1]));
        check (pk <= ceiling + 1.0e-6f, "peak <= ceiling", pk);
    }
}

static void testAllPresetsStable()
{
    std::printf ("[chain] every preset x layout x rate: finite, within ceiling, silence stays silent\n");
    const std::vector<std::pair<uint32_t, int>> layouts { { 0x4, 1 }, { 0x3, 2 }, { 0x3F, 6 }, { 0x63F, 8 }, { 0x2D63F, 12 } };

    for (double fs : { 16000.0, 44100.0, 48000.0, 96000.0 })
        for (auto [mask, chans] : layouts)
            for (auto& info : presetCatalog())
                for (bool headphones : { false, true })
                {
                    DspChain chain;
                    chain.prepare (fs, ChannelLayout::fromMask (mask, chans));
                    ProcessingParams pp;
                    pp.user = makePreset (info.id == PresetId::Custom ? PresetId::Cinema : info.id);
                    pp.headphones = headphones;
                    pp.speakerHighPassHz = headphones ? 0.0f : 80.0f;
                    chain.setParams (pp);

                    Signal in (chans, (int) (fs * 0.75));
                    fillNoise (in, -12.0f, (unsigned) (mask + chans));
                    // loud transient burst
                    for (int c = 0; c < chans; ++c)
                        for (int i = 2000; i < 2400; ++i)
                            in.ch[(size_t) c][(size_t) i] *= 8.0f;

                    std::vector<float> l, r;
                    runChain (chain, in, l, r, 333);

                    const float ceiling = std::pow (10.0f, pp.user.limiterCeilingDb / 20.0f);
                    if (! allFinite (l) || ! allFinite (r))
                    {
                        check (false, "non-finite output");
                        std::printf ("    preset=%s fs=%.0f ch=%d hp=%d\n", info.key, fs, chans, headphones ? 1 : 0);
                        continue;
                    }
                    const float pk = std::max (peakAbs (l), peakAbs (r));
                    check (pk <= ceiling + 1.0e-5f, "within limiter ceiling", pk);

                    // Silence afterwards must decay to (near) silence: no self-oscillation / DC.
                    Signal quiet (chans, (int) (fs * 3.0));
                    runChain (chain, quiet, l, r, 333);
                    float tail = 0.0f;
                    for (size_t i = l.size() - (size_t) (fs * 0.25); i < l.size(); ++i)
                        tail = std::max (tail, std::max (std::abs (l[i]), std::abs (r[i])));
                    if (tail >= 1.0e-4f)
                        std::printf ("    tail preset=%s fs=%.0f ch=%d hp=%d tail=%g\n", info.key, fs, chans, headphones ? 1 : 0, tail);
                    check (tail < 1.0e-4f, "silence decays to silence", tail);
                }
}

static void testBypassAndSurroundOff()
{
    std::printf ("[chain] bypass passes the dry signal\n");
    const double fs = 48000.0;
    DspChain chain;
    chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
    ProcessingParams pp;
    pp.user = makePreset (PresetId::Imax);
    pp.bypass = true;
    chain.setParams (pp);

    Signal in (2, (int) fs);
    fillNoise (in, -26.0f, 11);
    const Signal copy = in;
    std::vector<float> l, r;
    runChain (chain, in, l, r, 512);

    const int lat = chain.latencySamples();
    double maxErr = 0.0;
    for (int i = (int) (fs * 0.2); i < in.frames; ++i)
        maxErr = std::max (maxErr, (double) std::abs (l[(size_t) i] - copy.ch[0][(size_t) (i - lat)]));
    check (maxErr < 1.0e-5, "bypassed output == dry", maxErr);
}

static void testSpatialEnergy()
{
    std::printf ("[spatial] binaural render keeps level sane and is not mono\n");
    const double fs = 48000.0;
    DspChain chain;
    chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
    ProcessingParams pp;
    pp.user = makePreset (PresetId::Neutral);
    pp.user.surroundEnabled = true;
    pp.user.surroundIntensity = 1.0f;
    pp.user.stereoWidth = 0.5f;
    pp.headphones = true;
    chain.setParams (pp);

    Signal in (2, (int) fs);
    fillNoise (in, -24.0f, 5);   // uncorrelated L/R
    std::vector<float> l, r;
    runChain (chain, in, l, r, 480);

    const double inDb = rmsDb (in.ch[0], 0);
    const double outDb = rmsDb (l, (size_t) (fs * 0.3));
    check (std::abs (outDb - inDb) < 6.0, "binaural level within 6 dB", outDb - inDb);

    double diff = 0.0;
    for (size_t i = (size_t) (fs * 0.3); i < l.size(); ++i)
        diff += std::abs (l[i] - r[i]);
    check (diff > 1.0, "binaural output is not mono", diff);
}

static void testHeightLayer()
{
    std::printf ("[spatial] 3D height: overhead layer adds elevation cues, real 7.1.4 tops are placed\n");
    const double fs = 48000.0;

    auto renderStereo = [&] (float heightAmount, std::vector<float>& l, std::vector<float>& r)
    {
        DspChain chain;
        chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
        ProcessingParams pp;
        pp.user = makePreset (PresetId::Neutral);
        pp.user.surroundEnabled = true;
        pp.user.surroundIntensity = 1.0f;
        pp.user.heightAmount = heightAmount;
        pp.headphones = true;
        chain.setParams (pp);
        Signal in (2, (int) fs * 2);
        fillNoise (in, -24.0f, 13);
        runChain (chain, in, l, r, 480);
    };

    auto bandRatioDb = [&] (const std::vector<float>& x)
    {
        Biquad bp;
        bp.c = design::make (FilterType::BandPass, fs, 8000.0, 1.4, 0.0);
        double band = 0.0, total = 0.0;
        for (size_t i = (size_t) (fs * 0.5); i < x.size(); ++i)
        {
            const float b = bp.process (x[i]);
            band += (double) b * b;
            total += (double) x[i] * x[i];
        }
        return 10.0 * std::log10 (band / total);
    };

    std::vector<float> l0, r0, l1, r1;
    renderStereo (0.0f, l0, r0);
    renderStereo (1.0f, l1, r1);

    double diff = 0.0;
    for (size_t i = (size_t) (fs * 0.5); i < l0.size(); ++i)
        diff += std::abs (l1[i] - l0[i]);
    check (diff > 1.0, "height changes the output", diff);

    const double lift = bandRatioDb (l1) - bandRatioDb (l0);
    std::printf ("    8 kHz 'above' band share: %+.2f dB with height\n", lift);
    check (lift > 0.3, "height raises the 8 kHz elevation band", lift);

    const double levelChange = rmsDb (l1, (size_t) (fs * 0.5)) - rmsDb (l0, (size_t) (fs * 0.5));
    check (std::abs (levelChange) < 4.0, "height keeps level sane", levelChange);

    // Real 7.1.4: a signal only in the top-front-left channel lands left, from the effect path.
    {
        DspChain chain;
        chain.prepare (fs, ChannelLayout::fromMask (0x2D63F, 12));
        ProcessingParams pp;
        pp.user = makePreset (PresetId::Neutral);
        pp.user.surroundEnabled = true;
        pp.user.surroundIntensity = 1.0f;
        pp.headphones = true;
        chain.setParams (pp);

        Signal in (12, (int) fs);
        const int tfl = ChannelLayout::fromMask (0x2D63F, 12).indexOf (Speaker::TFL);
        check (tfl == 8, "7.1.4 TFL channel index", tfl);
        std::mt19937 rng (3);
        std::normal_distribution<float> nd (0.0f, 0.05f);
        for (auto& v : in.ch[(size_t) tfl]) v = nd (rng);

        std::vector<float> l, r;
        runChain (chain, in, l, r, 480);
        const double ld = rmsDb (l, (size_t) (fs * 0.3)), rd = rmsDb (r, (size_t) (fs * 0.3));
        check (allFinite (l) && allFinite (r), "7.1.4 output finite");
        check (ld > -60.0, "top channel is audible", ld);
        check (ld - rd > 2.0, "top-front-left is rendered on the left", ld - rd);
    }
}

static void testVenueReverb()
{
    std::printf ("[room] venue reverb time matches the venue size (Schroeder T30)\n");
    const double fs = 48000.0;

    for (float size : { 0.1f, 0.5f, 0.9f })
    {
        RoomAmbience room;
        room.setParams (1.0f, 0.0f, size);
        room.prepare (fs);
        room.setParams (1.0f, 0.0f, size);

        const int n = (int) (fs * 5.0);
        std::vector<float> l ((size_t) n, 0.0f), r ((size_t) n, 0.0f);
        // Let the wet ramp reach full level before the impulse.
        std::vector<float> warmL (9600, 0.0f), warmR (9600, 0.0f);
        room.process (warmL.data(), warmR.data(), 9600);
        l[0] = r[0] = 1.0f;
        for (int pos = 0; pos < n; pos += 480)
            room.process (l.data() + pos, r.data() + pos, std::min (480, n - pos));

        // Energy decay curve from 5 ms (skip the dry impulse).
        const size_t start = (size_t) (0.005 * fs);
        std::vector<double> edc ((size_t) n, 0.0);
        double acc = 0.0;
        for (size_t i = (size_t) n; i-- > start;)
        {
            acc += (double) l[i] * l[i] + (double) r[i] * r[i];
            edc[i] = acc;
        }
        auto crossing = [&] (double db)
        {
            const double thr = edc[start] * std::pow (10.0, db / 10.0);
            for (size_t i = start; i < (size_t) n; ++i)
                if (edc[i] <= thr) return (double) (i - start) / fs;
            return -1.0;
        };

        const double t30 = 2.0 * (crossing (-35.0) - crossing (-5.0));
        const double expected = RoomAmbience::rt60ForSize (size);
        std::printf ("    size %.1f: measured RT60 %.2f s (design %.2f s)\n", size, t30, expected);
        check (t30 > 0.0 && std::abs (t30 - expected) < expected * 0.35, "reverb time within 35 % of design", t30);
        check (allFinite (l) && allFinite (r), "reverb output finite");
    }
}

/** Clarity index C80 (dB) of the chain's impulse response: energy in the first 80 ms
    after the direct sound vs. everything later. Concert-hall acoustics treat roughly
    -2..+2 dB as "reverberant", higher values as increasingly clear. */
static double measureC80 (const ProcessingParams& pp, double fs)
{
    DspChain chain;
    chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
    chain.setParams (pp);

    Signal warm (2, (int) (fs * 0.3));
    std::vector<float> l, r;
    runChain (chain, warm, l, r, 480);   // let parameter ramps settle

    Signal imp (2, (int) (fs * 3.0));
    imp.ch[0][0] = imp.ch[1][0] = 0.5f;
    runChain (chain, imp, l, r, 480);

    const size_t t0 = (size_t) chain.latencySamples();
    const size_t split = t0 + (size_t) (0.08 * fs);
    double early = 0.0, late = 0.0;
    for (size_t i = t0; i < l.size(); ++i)
    {
        const double e = (double) l[i] * l[i] + (double) r[i] * r[i];
        (i < split ? early : late) += e;
    }
    return 10.0 * std::log10 (early / std::max (late, 1e-20));
}

/** Energy of the impulse response per time window relative to the direct sound (dB). */
static void printEchoProfile (const char* label, const ProcessingParams& pp, double fs)
{
    DspChain chain;
    chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
    chain.setParams (pp);
    Signal warm (2, (int) (fs * 0.3));
    std::vector<float> l, r;
    runChain (chain, warm, l, r, 480);
    Signal imp (2, (int) (fs * 3.0));
    imp.ch[0][0] = 0.5f;                       // hard-left impulse: shows crossfeed/echo paths too
    runChain (chain, imp, l, r, 480);

    const size_t t0 = (size_t) chain.latencySamples();
    const double edges[] { 0.0, 0.002, 0.020, 0.080, 0.300, 3.0 };
    double e[5] {};
    for (size_t i = t0; i < l.size(); ++i)
    {
        const double t = (double) (i - t0) / fs;
        for (int w = 0; w < 5; ++w)
            if (t >= edges[w] && t < edges[w + 1])
                e[w] += (double) l[i] * l[i] + (double) r[i] * r[i];
    }
    auto db = [&] (double x) { return 10.0 * std::log10 (std::max (x, 1e-20) / e[0]); };
    std::printf ("    %-28s 2-20ms %+6.1f | 20-80ms %+6.1f | 80-300ms %+6.1f | >300ms %+6.1f dB re direct\n",
                 label, db (e[1]), db (e[2]), db (e[3]), db (e[4]));
}

static void testConcertHallClarity()
{
    {
        const double fs = 48000.0;
        for (bool hp : { false, true })
        {
            ProcessingParams a; a.user = makePreset (PresetId::Music); a.headphones = hp;
            ProcessingParams b; b.user = makePreset (PresetId::ConcertHall); b.headphones = hp;
            std::printf ("  echo profile (%s):\n", hp ? "headphones" : "speakers");
            printEchoProfile ("MUSIC", a, fs);
            printEchoProfile ("CONCERT HALL", b, fs);
        }
    }
    std::printf ("[room] CONCERT HALL clarity (C80) - music must stay clear, not echoey\n");
    const double fs = 48000.0;
    for (bool headphones : { true, false })
    {
        ProcessingParams pp;
        pp.user = makePreset (PresetId::ConcertHall);
        pp.headphones = headphones;
        const double c80 = measureC80 (pp, fs);
        std::printf ("    %s: C80 %+.1f dB (venue %.2f, room %.2f)\n", headphones ? "headphones" : "speakers  ", c80,
                     pp.user.roomSize, pp.user.roomAmount);
        check (c80 > 4.0, "concert hall C80 > +4 dB (clear, not muddy)", c80);
    }
}

//==============================================================================
// Synthetic programme material for the content analyser (stand-ins, not real recordings)
namespace synth
{
    /** Dialogue-like: voice-band noise syllables (120-250 ms) with gaps and phrase pauses, centred. */
    Signal speech (double fs, double seconds, unsigned seed)
    {
        Signal s (2, (int) (fs * seconds));
        std::mt19937 rng (seed);
        std::uniform_real_distribution<double> u (0.0, 1.0);
        std::normal_distribution<float> nd (0.0f, 1.0f);
        Biquad hp, lp;
        hp.c = design::make (FilterType::HighPass, fs, 300.0, 0.7071, 0.0);
        lp.c = design::make (FilterType::LowPass, fs, 3000.0, 0.7071, 0.0);

        size_t pos = 0;
        while (pos < (size_t) s.frames)
        {
            const int syllables = 6 + (int) (u (rng) * 7);
            const float phraseGain = 0.2f * std::pow (10.0f, (float) (-6.0 * u (rng)) / 20.0f);
            for (int k = 0; k < syllables && pos < (size_t) s.frames; ++k)
            {
                const size_t len = (size_t) ((0.12 + 0.13 * u (rng)) * fs);
                for (size_t i = 0; i < len && pos < (size_t) s.frames; ++i, ++pos)
                {
                    const float env = 0.5f - 0.5f * (float) std::cos (2.0 * kPi * (double) i / (double) len);
                    const float x = lp.process (hp.process (nd (rng))) * env * phraseGain + 0.001f * nd (rng);
                    s.ch[0][pos] = s.ch[1][pos] = x;
                }
                const size_t gap = (size_t) ((0.04 + 0.08 * u (rng)) * fs);
                for (size_t i = 0; i < gap && pos < (size_t) s.frames; ++i, ++pos)
                    s.ch[0][pos] = s.ch[1][pos] = 0.001f * nd (rng);
            }
            const size_t pause = (size_t) ((0.4 + 0.5 * u (rng)) * fs);
            for (size_t i = 0; i < pause && pos < (size_t) s.frames; ++i, ++pos)
                s.ch[0][pos] = s.ch[1][pos] = 0.001f * nd (rng);
        }
        return s;
    }

    /** Music-like: sustained chords, kick every 0.5 s, hi-hats every 0.25 s, steady level, wide. */
    Signal music (double fs, double seconds, unsigned seed)
    {
        Signal s (2, (int) (fs * seconds));
        std::mt19937 rng (seed);
        std::normal_distribution<float> nd (0.0f, 1.0f);
        Biquad hatHp;
        hatHp.c = design::make (FilterType::HighPass, fs, 6000.0, 0.7071, 0.0);
        const double chords[4][3] { { 220.0, 261.6, 329.6 }, { 196.0, 246.9, 293.7 }, { 174.6, 220.0, 261.6 }, { 196.0, 246.9, 329.6 } };

        for (int i = 0; i < s.frames; ++i)
        {
            const double t = i / fs;
            const auto& c = chords[(int) (t / 2.0) % 4];
            double padL = 0.0, padR = 0.0;
            for (double f0 : c)
                for (int h = 1; h <= 5; ++h)
                {
                    padL += std::sin (2.0 * kPi * f0 * h * t) / h;
                    padR += std::sin (2.0 * kPi * f0 * 1.003 * h * t + 0.7 * h) / h;
                }
            const double tk = std::fmod (t, 0.5), th = std::fmod (t, 0.25);
            const double kick = 0.5 * std::sin (2.0 * kPi * 55.0 * tk) * std::exp (-tk / 0.12);
            const double hat = 0.08 * hatHp.process (nd (rng)) * std::exp (-th / 0.03);
            s.ch[0][(size_t) i] = (float) (0.05 * padL + kick + hat);
            s.ch[1][(size_t) i] = (float) (0.05 * padR + kick + hat);
        }
        return s;
    }

    /** Pink noise (Paul Kellet's filter), optionally reshaped. */
    Signal pink (double fs, double seconds, unsigned seed)
    {
        Signal s (2, (int) (fs * seconds));
        std::mt19937 rng (seed);
        std::normal_distribution<float> nd (0.0f, 1.0f);
        for (auto& ch : s.ch)
        {
            double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
            for (auto& v : ch)
            {
                const double w = nd (rng);
                b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759;
                b2 = 0.96900 * b2 + w * 0.1538520; b3 = 0.86650 * b3 + w * 0.3104856;
                b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
                v = (float) ((b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * 0.03);
                b6 = w * 0.115926;
            }
        }
        return s;
    }

    void runAnalyzer (ContentAnalyzer& a, Signal& s)
    {
        float* p[kMaxInputChannels] {};
        for (int pos = 0; pos < s.frames; pos += 480)
        {
            for (size_t c = 0; c < s.ch.size(); ++c)
                p[c] = s.ch[c].data() + pos;
            a.process (p, std::min (480, s.frames - pos));
        }
    }
}

static void testContentAnalyzer()
{
    std::printf ("[auto] music vs. film detection on synthetic material\n");
    const double fs = 48000.0;

    auto analyse = [&] (Signal s, const char* label, uint32_t mask)
    {
        ContentAnalyzer a;
        a.prepare (fs, ChannelLayout::fromMask (mask, (int) s.ch.size()));
        synth::runAnalyzer (a, s);
        std::printf ("    %-28s p(movie) %.2f | speech %.2f beat %.2f | modulation %.1f dB | pauses %.2f | spread %.1f dB\n",
                     label, a.movieProbability(), a.speechScore(), a.beatStrength(), a.modulationDepthDb(), a.pauseRatio(), a.loudnessSpreadDb());
        return a.movieProbability();
    };

    const float pSpeech = analyse (synth::speech (fs, 25.0, 4), "dialogue-like", 0x3);
    const float pMusic = analyse (synth::music (fs, 25.0, 5), "music-like", 0x3);
    check (pSpeech > 0.62f, "dialogue-like material is classed as film (> 0.62)", pSpeech);
    check (pMusic < 0.38f, "music-like material is classed as music (< 0.38)", pMusic);

    // Harder, realistic mixes.
    // Film scene: dialogue over a sustained score (no drums) and room tone, wide stereo.
    {
        Signal dia = synth::speech (fs, 25.0, 8);
        Signal amb = synth::pink (fs, 25.0, 9);
        const double chord[3] { 146.8, 220.0, 293.7 };
        for (int i = 0; i < dia.frames; ++i)
        {
            const double t = i / fs;
            double padL = 0.0, padR = 0.0;
            for (double f0 : chord)
                for (int h = 1; h <= 4; ++h)
                {
                    padL += std::sin (2.0 * kPi * f0 * h * t) / h;
                    padR += std::sin (2.0 * kPi * f0 * 1.004 * h * t + h) / h;
                }
            dia.ch[0][(size_t) i] += (float) (0.012 * padL) + 0.15f * amb.ch[0][(size_t) i];
            dia.ch[1][(size_t) i] += (float) (0.012 * padR) + 0.15f * amb.ch[1][(size_t) i];
        }
        const float pFilm = analyse (std::move (dia), "film scene (dialogue+score)", 0x3);
        check (pFilm > 0.62f, "dialogue over music bed is classed as film", pFilm);
    }
    // Song with vocals: syllabic vocal on top of a drum beat and chords.
    {
        Signal song = synth::music (fs, 25.0, 10);
        Signal vox = synth::speech (fs, 25.0, 11);
        for (int i = 0; i < song.frames; ++i)
        {
            song.ch[0][(size_t) i] += vox.ch[0][(size_t) i];
            song.ch[1][(size_t) i] += vox.ch[1][(size_t) i];
        }
        const float pSong = analyse (std::move (song), "song with vocals", 0x3);
        check (pSong < 0.38f, "vocals over a beat are classed as music", pSong);
    }

    // 5.1 stream with active surround channels -> film.
    Signal mc (6, (int) (fs * 12.0));
    {
        Signal m = synth::music (fs, 12.0, 6);
        std::mt19937 rng (7);
        std::normal_distribution<float> nd (0.0f, 0.05f);
        for (int i = 0; i < mc.frames; ++i)
        {
            mc.ch[0][(size_t) i] = m.ch[0][(size_t) i];
            mc.ch[1][(size_t) i] = m.ch[1][(size_t) i];
            mc.ch[4][(size_t) i] = nd (rng);   // BL
            mc.ch[5][(size_t) i] = nd (rng);   // BR
        }
    }
    const float pMc = analyse (mc, "5.1 with surrounds", 0x3F);
    check (pMc > 0.62f, "multichannel with active surrounds is classed as film", pMc);
}

static void testSmartTone()
{
    std::printf ("[smart tone] automatic bass / treble corrects towards a balanced mix\n");
    const double fs = 48000.0;

    auto tone = [&] (Signal s, const char* label)
    {
        ContentAnalyzer a;
        a.prepare (fs, ChannelLayout::fromMask (0x3, 2));
        synth::runAnalyzer (a, s);
        std::printf ("    %-20s bass share %6.1f dB -> bass %+.1f dB | treble share %6.1f dB -> treble %+.1f dB\n",
                     label, a.bassShareDb(), a.smartBassDb(), a.trebleShareDb(), a.smartTrebleDb());
        return std::make_pair (a.smartBassDb(), a.smartTrebleDb());
    };

    // Thin: pink noise without bass.
    Signal thin = synth::pink (fs, 14.0, 11);
    for (auto& ch : thin.ch) { Biquad hp; hp.c = design::make (FilterType::HighPass, fs, 250.0, 0.7071, 0.0); for (auto& v : ch) v = hp.process (v); }
    // Boomy: pink noise with +12 dB below 150 Hz.
    Signal boomy = synth::pink (fs, 14.0, 12);
    for (auto& ch : boomy.ch) { Biquad ls; ls.c = design::make (FilterType::LowShelf, fs, 150.0, 0.7071, 12.0); for (auto& v : ch) v = 0.5f * ls.process (v); }
    // Harsh: pink noise with +10 dB above 5 kHz.
    Signal harsh = synth::pink (fs, 14.0, 13);
    for (auto& ch : harsh.ch) { Biquad hs; hs.c = design::make (FilterType::HighShelf, fs, 5000.0, 0.7071, 10.0); for (auto& v : ch) v = 0.5f * hs.process (v); }

    const auto t1 = tone (std::move (thin), "thin (no bass)");
    const auto t2 = tone (std::move (boomy), "boomy (+12 dB bass)");
    const auto t3 = tone (std::move (harsh), "harsh (+10 dB top)");
    check (t1.first > 2.0f, "thin track gets more bass", t1.first);
    check (t2.first < -1.5f, "boomy track gets less bass", t2.first);
    check (t3.second < -2.0f, "harsh track gets softer treble", t3.second);
    check (std::abs (t1.second) <= 4.0f && t1.first <= 5.0f && t3.second >= -4.0f, "corrections stay within limits");

    // End to end: with smart tone on, a thin track comes out with more bass.
    auto bassShareOut = [&] (bool smart)
    {
        DspChain chain;
        chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
        ProcessingParams pp;
        pp.user = makePreset (PresetId::Neutral);
        pp.smartTone = smart;
        chain.setParams (pp);
        Signal in = synth::pink (fs, 14.0, 21);
        for (auto& ch : in.ch) { Biquad hp; hp.c = design::make (FilterType::HighPass, fs, 250.0, 0.7071, 0.0); for (auto& v : ch) v = hp.process (v); }
        std::vector<float> l, r;
        runChain (chain, in, l, r, 480);
        Biquad bp; bp.c = design::make (FilterType::LowPass, fs, 160.0, 0.7071, 0.0);
        double eb = 0, ef = 0;
        for (size_t i = (size_t) (fs * 11); i < l.size(); ++i) { const float b = bp.process (l[i]); eb += (double) b * b; ef += (double) l[i] * l[i]; }
        return 10.0 * std::log10 (eb / ef);
    };
    const double off = bassShareOut (false), on = bassShareOut (true);
    std::printf ("    end to end: output bass share %.1f dB (smart off) -> %.1f dB (smart on)\n", off, on);
    check (on - off > 1.5, "smart tone raises bass on a thin track", on - off);
}

static void testAppHints()
{
    std::printf ("[auto] app / site recognition\n");
    using namespace cinemax::win;
    auto session = [] (const wchar_t* exe, std::vector<std::wstring> titles, float peak = 0.5f)
    {
        AudioSessionInfo s;
        s.exeName = exe;
        s.windowTitles = std::move (titles);
        s.peak = peak;
        s.active = true;
        return s;
    };

    check (classifySession (session (L"vlc.exe", { L"movie.mkv - VLC media player" })).hint == ContentHint::Movie, "VLC -> movie");
    check (classifySession (session (L"chrome.exe", { L"Stranger Things | Netflix - Google Chrome" })).hint == ContentHint::Movie, "Netflix tab -> movie");
    check (classifySession (session (L"msedge.exe", { L"Watch Show | Disney+ Hotstar - Work - Microsoft Edge" })).hint == ContentHint::Movie, "Hotstar tab -> movie");
    check (classifySession (session (L"YouTube Music.exe", { L"YouTube Music" })).hint == ContentHint::Music, "YouTube Music app -> music");
    check (classifySession (session (L"chrome.exe", { L"Spotify - Web Player: Music for everyone - Google Chrome" })).hint == ContentHint::Music, "Spotify tab -> music");
    check (classifySession (session (L"chrome.exe", { L"Some vlog - YouTube - Google Chrome" })).hint == ContentHint::None, "plain YouTube -> no hint (sound analysis)");

    // YouTube / YouTube Music play both: the title decides (formats seen on this PC).
    check (classifySession (session (L"YouTube Music.exe", { L"Fear Song | YouTube Music" })).hint == ContentHint::Music, "YouTube Music app, song -> music");
    check (classifySession (session (L"YouTube Music.exe", { L"Pathaan Full Movie | YouTube Music" })).hint == ContentHint::Movie, "YouTube Music app, full movie -> movie");
    check (classifySession (session (L"chrome.exe", { L"418 (2026) HDRip [Telugu] | Site - Google Chrome" })).hint == ContentHint::Movie, "HDRip tab -> movie");
    check (classifySession (session (L"chrome.exe", { L"Kesariya Full Video Song | Brahmastra - YouTube - Google Chrome" })).hint == ContentHint::Music, "YouTube video song -> music");
    check (classifySession (session (L"chrome.exe", { L"Animal Official Trailer - YouTube - Google Chrome" })).hint == ContentHint::Movie, "YouTube trailer -> movie");
    check (classifySession (session (L"msedge.exe", { L"Comedy Scene | Hindi Dubbed - YouTube - Microsoft Edge" })).hint == ContentHint::Movie, "YouTube dubbed scene -> movie");
    check (classifySession (session (L"vlc.exe", { L"track01.flac - VLC media player" })).hint == ContentHint::Music, "VLC playing an audio file -> music");
    check (classifySession (session (L"chrome.exe", { L"Arijit Singh Songs Jukebox - YouTube - Google Chrome" })).hint == ContentHint::Music, "YouTube jukebox -> music");

    // The loudest *audible* recognised session wins; silent ones are ignored.
    std::vector<AudioSessionInfo> list { session (L"spotify.exe", { L"Spotify" }, 0.0f),
                                         session (L"vlc.exe", { L"film.mp4" }, 0.4f),
                                         session (L"discord.exe", { L"Discord" }, 0.9f) };
    const auto best = detectAppHint (list);
    check (best.hint == ContentHint::Movie && best.source == "VLC", "loudest audible recognised app decides");
}

static void testLoudnessMeter()
{
    std::printf ("[loudness] BS.1770 calibration\n");
    for (double fs : { 44100.0, 48000.0 })
    {
        LoudnessMeter m;
        m.prepare (fs);
        Signal s (2, (int) (fs * 4));
        fillSine (s, 997.0, fs, 0.1f);    // -20 dBFS peak in both channels -> -20.0 LUFS
        m.process (s.ch[0].data(), s.ch[1].data(), s.frames);
        check (std::abs (m.shortTermLufs() + 20.0f) < 0.15f, "997 Hz -20 dBFS stereo reads -20 LUFS", m.shortTermLufs());
    }
}

static void testPresetLoudness()
{
    std::printf ("[chain] presets land near their loudness target (no runaway gain)\n");
    const double fs = 48000.0;

    for (float inputDb : { -32.0f, -24.0f, -16.0f })
        for (auto& info : presetCatalog())
        {
            if (info.id == PresetId::Custom)
                continue;

            DspChain chain;
            chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
            ProcessingParams pp;
            pp.user = makePreset (info.id);
            pp.headphones = true;
            chain.setParams (pp);

            Signal in (2, (int) (fs * 12));
            fillNoise (in, inputDb, 21);
            LoudnessMeter inMeter;
            inMeter.prepare (fs);
            inMeter.process (in.ch[0].data(), in.ch[1].data(), in.frames);

            std::vector<float> l, r;
            runChain (chain, in, l, r, 480);

            // Measure the last 4 s (after the normaliser has settled).
            LoudnessMeter outMeter;
            outMeter.prepare (fs);
            const size_t from = l.size() - (size_t) (fs * 4);
            outMeter.process (l.data() + from, r.data() + from, (int) (fs * 4));

            const float inL = inMeter.integratedLufs(), outL = outMeter.integratedLufs();
            const auto& u = pp.user;
            if (u.dynamicsEnabled && u.loudnessEnabled)
            {
                const float maxBoost = info.id == PresetId::Night ? 12.0f : 9.0f;
                const float expected = std::clamp (u.loudnessTargetLufs, inL - 12.0f, inL + maxBoost);
                const bool ok = std::abs (outL - expected) < 2.5f;
                if (! ok) std::printf ("    %s in %.1f -> out %.1f LUFS (expected ~%.1f)\n", info.key, inL, outL, expected);
                check (ok, "loudness within 2.5 LU of target", outL - expected);
            }
            else
            {
                const bool ok = outL - inL < 6.0f;
                if (! ok) std::printf ("    %s in %.1f -> out %.1f LUFS\n", info.key, inL, outL);
                check (ok, "un-normalised preset adds < 6 LU", outL - inL);
            }
        }
}

static void testResampler()
{
    std::printf ("[resampler] conversion accuracy and drift handling\n");
    struct Case { double in, out; bool hq; double maxErrDb; };
    for (auto c : { Case { 48000, 44100, false, -60 }, Case { 48000, 44100, true, -80 },
                     Case { 44100, 48000, true, -80 }, Case { 48000, 48000, false, -60 }, Case { 48000, 16000, true, -70 } })
    {
        Resampler rs;
        rs.prepare (1, c.in, c.out, c.hq, 1.0);
        const double f0 = 997.0;
        const int inFrames = (int) c.in;   // 1 s
        std::vector<float> x ((size_t) inFrames);
        for (int i = 0; i < inFrames; ++i)
            x[(size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * f0 * i / c.in);
        rs.pushInterleaved (x.data(), inFrames, 1);

        std::vector<float> y ((size_t) c.out);
        float* outPtr[1] { y.data() };
        const int produced = rs.produce (outPtr, (int) (c.out * 0.9));

        // Compare with the ideal sine (resampler delay = 'half' input samples is
        // compensated by fitting the phase via least squares on sin/cos).
        double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
        const int from = 2000;
        for (int i = from; i < produced; ++i)
        {
            const double t = 2.0 * kPi * f0 * i / c.out;
            const double s = std::sin (t), co = std::cos (t);
            ss += s * s; cc += co * co; sc += s * co;
            ys += y[(size_t) i] * s; yc += y[(size_t) i] * co;
        }
        const double det = ss * cc - sc * sc;
        const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
        double err = 0, sig = 0;
        for (int i = from; i < produced; ++i)
        {
            const double t = 2.0 * kPi * f0 * i / c.out;
            const double ideal = a * std::sin (t) + b * std::cos (t);
            err += (y[(size_t) i] - ideal) * (y[(size_t) i] - ideal);
            sig += ideal * ideal;
        }
        const double errDb = 10.0 * std::log10 (err / sig);
        std::printf ("    %.0f -> %.0f %s: error %.1f dB\n", c.in, c.out, c.hq ? "HQ " : "std", errDb);
        check (errDb < c.maxErrDb, "resampler error below threshold", errDb);
    }

    // Drift: +1000 ppm must consume ~0.1 % more input.
    Resampler rs;
    rs.prepare (2, 48000, 48000, false, 2.0);
    std::vector<float> silence (48000 * 2 * 2, 0.0f);
    rs.pushInterleaved (silence.data(), 96000, 2);
    rs.setDriftPpm (1000.0);
    const double before = rs.bufferedFrames();
    std::vector<float> o1 (48000), o2 (48000);
    float* outs[2] { o1.data(), o2.data() };
    rs.produce (outs, 48000);
    const double consumed = before - rs.bufferedFrames();
    check (std::abs (consumed - 48048.0) < 2.0, "drift +1000 ppm consumes 48048 frames", consumed);
}

static void testNoAllocationInProcess()
{
    std::printf ("[realtime] zero heap allocations in DspChain::process\n");
    const double fs = 48000.0;
    for (auto [mask, chans] : std::vector<std::pair<uint32_t, int>> { { 0x3, 2 }, { 0x63F, 8 }, { 0x2D63F, 12 } })
    {
        DspChain chain;
        chain.prepare (fs, ChannelLayout::fromMask (mask, chans));
        MeterBus meters;
        meters.analysisEnabled = true;

        Signal in (chans, 4800);
        fillNoise (in, -18.0f, 9);
        std::vector<float> l (4800), r (4800);
        float* ptrs[kMaxInputChannels] {};

        allocationCount = 0;
        trackAllocations = true;
        for (int k = 0; k < 50; ++k)
        {
            ProcessingParams pp;
            pp.user = makePreset (k % 2 == 0 ? PresetId::Cinema : PresetId::Night);   // parameter changes too
            pp.headphones = (k % 3) == 0;
            pp.smartTone = true;
            chain.setParams (pp);
            for (int c = 0; c < chans; ++c) ptrs[c] = in.ch[(size_t) c].data();
            chain.process (ptrs, 480, 1.0f, l.data(), r.data(), &meters);
        }
        trackAllocations = false;
        check (allocationCount == 0, "no allocations during process()", (double) allocationCount);

        Resampler rs;
        rs.prepare (chans, 48000, 44100, true, 0.5);
        std::vector<float> inter ((size_t) chans * 480, 0.1f);
        std::vector<std::vector<float>> o ((size_t) chans, std::vector<float> (480));
        float* op[kMaxInputChannels] {};
        for (int c = 0; c < chans; ++c) op[c] = o[(size_t) c].data();

        allocationCount = 0;
        trackAllocations = true;
        for (int k = 0; k < 200; ++k)
        {
            rs.pushInterleaved (inter.data(), 480, chans);
            rs.setDriftPpm ((k % 7) * 50.0);
            if (rs.canProduce (440)) rs.produce (op, 440);
        }
        trackAllocations = false;
        check (allocationCount == 0, "no allocations in resampler", (double) allocationCount);
    }
}

//==============================================================================
// Measured HRTFs ("real ears"): file format, interpolation, rendering, head tracking.
namespace
{
    /** A synthetic set with a simple, known level difference: the ear facing the source is
        louder (gain 0.6 +/- 0.4 sin(az)), the far ear is delayed by up to 30 samples. */
    std::vector<uint8_t> makeSyntheticHrtfFile (int taps = 64)
    {
        const int numAz = 72, numEl = 9;
        std::vector<uint8_t> b;
        auto put = [&b] (const void* p, size_t n) { auto* c = (const uint8_t*) p; b.insert (b.end(), c, c + n); };
        auto u32 = [&] (uint32_t v) { put (&v, 4); };
        auto f32 = [&] (float v) { put (&v, 4); };
        auto str = [&] (const std::string& s) { u32 ((uint32_t) s.size()); put (s.data(), s.size()); };
        put ("CHRTF01\0", 8);
        u32 (48000); u32 ((uint32_t) taps); u32 ((uint32_t) numAz); u32 ((uint32_t) numEl);
        f32 (5.0f); f32 (-30.0f); f32 (15.0f);
        const float scale = 1.0f;
        f32 (scale);
        str ("synthetic"); str ("Synthetic test ears"); str ("unit test");
        for (int e = 0; e < numEl; ++e)
            for (int a = 0; a < numAz; ++a)
            {
                const double s = std::sin (a * 5.0 * kPi / 180.0);
                f32 ((float) (s > 0 ? 30.0 * s : 0.0));     // left ear delayed when the source is right
                f32 ((float) (s < 0 ? -30.0 * s : 0.0));
            }
        for (int e = 0; e < numEl; ++e)
            for (int a = 0; a < numAz; ++a)
                for (int r = 0; r < 2; ++r)
                {
                    const double s = std::sin (a * 5.0 * kPi / 180.0);
                    const double g = r == 0 ? 0.6 - 0.4 * s : 0.6 + 0.4 * s;
                    for (int t = 0; t < taps; ++t)
                    {
                        const double v = t == 0 ? g * 0.8 : (t == 1 ? g * 0.2 : 0.0);
                        const int16_t q = (int16_t) std::lround (v / scale * 32767.0);
                        put (&q, 2);
                    }
                }
        return b;
    }

    double rmsOf (const std::vector<float>& v, size_t from = 0)
    {
        double s = 0;
        for (size_t i = from; i < v.size(); ++i) s += (double) v[i] * v[i];
        return std::sqrt (s / std::max<size_t> (1, v.size() - from));
    }
}

static void testRealEars()
{
    std::printf ("[real ears] measured HRTF sets, interpolation, rendering, head tracking, ear test\n");

    // --- file format ------------------------------------------------------------------
    auto bytes = makeSyntheticHrtfFile();
    std::string err;
    auto set = parseHrtf (bytes.data(), bytes.size(), err);
    check (set != nullptr, "synthetic .chrtf parses");
    if (! set) { std::printf ("    %s\n", err.c_str()); return; }
    check (set->numAz == 72 && set->numEl == 9 && set->taps == 64 && set->key == "synthetic", "header round-trips");
    auto truncated = bytes;
    truncated.resize (truncated.size() - 100);
    check (parseHrtf (truncated.data(), truncated.size(), err) == nullptr, "truncated file is rejected");
    check (parseHrtf ("garbage", 7, err) == nullptr, "garbage is rejected");

    // --- lookup / interpolation ---------------------------------------------------------
    auto p48 = prepareHrtf (set, 48000.0);
    std::vector<float> l ((size_t) p48->taps), r ((size_t) p48->taps);
    float dl = 0, dr = 0;
    p48->lookup (90.0f, 0.0f, l.data(), r.data(), dl, dr);    // right
    check (std::abs (l[0] - 0.16f) < 0.002f && std::abs (r[0] - 0.8f) < 0.002f, "right source: right ear louder", r[0] / std::max (l[0], 1e-6f));
    check (std::abs (dl - 30.0f) < 0.01f && dr == 0.0f, "right source: left ear delayed 30 samples", dl);
    p48->lookup (-90.0f, 0.0f, l.data(), r.data(), dl, dr);   // left (negative azimuth wraps)
    check (l[0] > r[0] * 4.0f && dr > 29.0f, "left source mirrors it", dr);
    p48->lookup (2.5f, 0.0f, l.data(), r.data(), dl, dr);     // half-way between 0 and 5 degrees
    const float expectR = 0.8f * (float) (0.6 + 0.4 * 0.5 * std::sin (5.0 * kPi / 180.0));
    check (std::abs (r[0] - expectR) < 0.002f, "between grid points it interpolates", r[0]);
    p48->lookup (0.0f, 200.0f, l.data(), r.data(), dl, dr);   // out of range elevation clamps
    check (std::isfinite (l[0]) && std::isfinite (dl), "elevation outside the grid clamps");

    // --- other sample rates keep the response's DC gain and scale the delay ------------------
    for (double fs : { 44100.0, 96000.0 })
    {
        auto p = prepareHrtf (set, fs);
        std::vector<float> a ((size_t) p->taps), c ((size_t) p->taps);
        p->lookup (90.0f, 0.0f, a.data(), c.data(), dl, dr);
        double sum = 0;
        for (float v : c) sum += v;
        check (std::abs (sum - 0.6 * 1.0 - 0.4) < 0.03, "resampled response keeps its gain", sum);
        check (std::abs (dl - 30.0f * (float) (fs / 48000.0)) < 0.01f, "delay scales with the sample rate", dl);
    }

    // --- voice: impulse response and click-free direction changes ---------------------------
    {
        HrirVoice v;
        v.prepare (64, 256);
        std::vector<float> ir1 (64, 0.0f), ir2 (64, 0.0f);
        ir1[0] = 0.5f; ir1[1] = 0.25f;
        v.setFilter (ir1.data(), ir1.data(), 3.0f, 0.0f, true);
        std::vector<float> x (16, 0.0f), ol (16, 0.0f), orr (16, 0.0f);
        x[0] = 1.0f;
        v.process (x.data(), ol.data(), orr.data(), 16);
        check (orr[0] == 0.5f && orr[1] == 0.25f && ol[3] == 0.5f && ol[4] == 0.25f && ol[2] == 0.0f,
               "voice output = IR, far ear delayed");

        // switch filters while a sine plays: the change is a smooth crossfade, never a jump
        v.reset();
        ir2[0] = -0.5f;
        v.setFilter (ir1.data(), ir1.data(), 0.0f, 0.0f, true);
        const int n = 4800;
        std::vector<float> s (n), yl (n, 0.0f), yr (n, 0.0f);
        for (int i = 0; i < n; ++i) s[(size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * 1000.0 * i / 48000.0);
        v.process (s.data(), yl.data(), yr.data(), 2400);
        v.setFilter (ir2.data(), ir2.data(), 6.0f, 6.0f, false);
        v.process (s.data() + 2400, yl.data() + 2400, yr.data() + 2400, 2400);
        float maxStep = 0.0f;
        for (int i = 1; i < n; ++i) maxStep = std::max (maxStep, std::abs (yl[(size_t) i] - yl[(size_t) i - 1]));
        // largest step of the steady 1 kHz output itself is ~0.75 * 2 pi * 1000 / 48000 * 0.5 = 0.05
        check (maxStep < 0.08f, "filter change has no click", maxStep);
        check (std::abs (yl[(size_t) n - 1]) > 0.0f, "new filter is playing after the fade");
    }

    // --- spatializer with the set installed ---------------------------------------------------
    HrtfLibrary::install ({ set });
    check (HrtfLibrary::indexOf ("synthetic") == 0 && HrtfLibrary::indexOf ("nope") == -1, "library lookup by key");

    auto runMono = [] (int hrtf, float yaw, bool hp) -> std::pair<double, double>
    {
        DspChain chain;
        chain.prepare (48000.0, ChannelLayout::fromMask (0x3, 2));
        HeadPose pose;
        pose.yaw = yaw;
        pose.active = true;
        chain.setHeadPose (&pose);
        ProcessingParams pp;
        pp.user = makePreset (PresetId::Cinema);
        pp.user.surroundEnabled = true;
        pp.user.surroundIntensity = 1.0f;
        pp.user.roomAmount = 0.0f;
        pp.user.heightAmount = 0.0f;
        pp.headphones = hp;
        pp.hrtfSet = hrtf;
        chain.setParams (pp);
        Signal in (2, 48000);
        fillNoise (in, -24.0f, 3);
        in.ch[1] = in.ch[0];                     // mono: a centred phantom source
        std::vector<float> ol (48000), orr (48000);
        float* ptrs[2] { in.ch[0].data(), in.ch[1].data() };
        chain.process (ptrs, 48000, 1.0f, ol.data(), orr.data(), nullptr);
        return { rmsOf (ol, 24000), rmsOf (orr, 24000) };
    };

    const auto front = runMono (0, 0.0f, true);
    check (std::isfinite (front.first) && front.first > 1e-4, "real ears render audio");
    check (std::abs (20 * std::log10 (front.first / front.second)) < 1.0, "centred source stays centred", 20 * std::log10 (front.first / front.second));
    const auto turned = runMono (0, 90.0f, true);
    const double balance = 20 * std::log10 (turned.first / turned.second);
    check (balance > 3.0, "head turned right: the screen sound moves to the left ear", balance);
    const auto turnedLeft = runMono (0, -90.0f, true);
    check (20 * std::log10 (turnedLeft.second / turnedLeft.first) > 3.0, "head turned left: sound moves right");
    const auto model = runMono (-1, 90.0f, true);
    check (std::abs (20 * std::log10 (model.first / model.second)) < 1.0, "head model ignores the pose (no tracking)");
    const auto speakers = runMono (0, 90.0f, false);
    check (std::abs (20 * std::log10 (speakers.first / speakers.second)) < 1.0, "speakers: real ears/pose not used");
    const auto badIndex = runMono (7, 0.0f, true);
    check (std::isfinite (badIndex.first) && badIndex.first > 1e-4, "unknown set index falls back to the head model");

    // --- ear-fit test sequence ---------------------------------------------------------------
    for (int which : { 0, -1 })
    {
        DspChain chain;
        chain.prepare (48000.0, ChannelLayout::fromMask (0x3, 2));
        ProcessingParams pp;
        pp.user = makePreset (PresetId::Cinema);
        pp.headphones = true;
        pp.auditionSet = which;
        pp.auditionId = 1;
        chain.setParams (pp);
        const int total = 48000 * 6;
        std::vector<float> zl (480, 0.0f), zr (480, 0.0f), ol (480), orr (480);
        double first = 0, last = 0;
        for (int done = 0; done < total; done += 480)
        {
            float* ptrs[2] { zl.data(), zr.data() };
            chain.process (ptrs, 480, 1.0f, ol.data(), orr.data(), nullptr);
            const double e = rmsOf (ol) + rmsOf (orr);
            if (done < 48000) first += e;
            if (done > 48000 * 5) last += e;
        }
        check (first > 0.05, which == 0 ? "ear test plays (real ears)" : "ear test plays (head model)", first);
        check (last < 1e-6, "ear test stops by itself after the walk round the head", last);
    }

    // --- real-time safety with real ears, head movement and the ear test ----------------------
    {
        DspChain chain;
        chain.prepare (48000.0, ChannelLayout::fromMask (0x2D63F, 12));
        HeadPose pose;
        pose.active = true;
        chain.setHeadPose (&pose);
        Signal in (12, 4800);
        fillNoise (in, -18.0f, 5);
        std::vector<float> ol (4800), orr (4800);
        float* ptrs[kMaxInputChannels] {};
        allocationCount = 0;
        trackAllocations = true;
        for (int k = 0; k < 60; ++k)
        {
            ProcessingParams pp;
            pp.user = makePreset (PresetId::Imax);
            pp.headphones = true;
            pp.hrtfSet = k < 40 ? 0 : -1;
            pp.auditionSet = k >= 20 && k < 30 ? 0 : -2;
            pp.auditionId = 3;
            chain.setParams (pp);
            pose.yaw = (float) (40.0 * std::sin (k * 0.3));
            pose.pitch = (float) (10.0 * std::cos (k * 0.2));
            for (int c = 0; c < 12; ++c) ptrs[c] = in.ch[(size_t) c].data();
            chain.process (ptrs, 480, 1.0f, ol.data(), orr.data(), nullptr);
        }
        trackAllocations = false;
        check (allocationCount == 0, "no allocations with real ears + head tracking", (double) allocationCount);
        bool finite = true;
        for (float v : ol) finite = finite && std::isfinite (v) && std::abs (v) <= 1.0f;
        check (finite, "output finite and within the ceiling");
    }

    HrtfLibrary::install ({});
}

//==============================================================================
// The real-ear sets that ship with CINEMAX (data/hrtf): they load, behave like ears, and the
// renderer is cheap enough.
static void testShippedRealEars()
{
    std::printf ("[real ears] shipped sets (data/hrtf): load, symmetry, CPU\n");
#ifdef CINEMAX_SOURCE_DIR
    const juce::File dir = juce::File (CINEMAX_SOURCE_DIR).getChildFile ("data/hrtf");
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.chrtf");
    files.sort();
    check (files.size() >= 2, "real-ear sets present", (double) files.size());
    std::vector<std::shared_ptr<const HrtfSet>> sets;
    for (auto& f : files)
    {
        juce::MemoryBlock mb;
        f.loadFileAsData (mb);
        std::string err;
        auto s = parseHrtf (mb.getData(), mb.getSize(), err);
        check (s != nullptr, "shipped set parses");
        if (s) sets.push_back (s);
    }
    if (sets.empty()) return;

    for (auto& s : sets)
    {
        auto p = prepareHrtf (s, 48000.0);
        std::vector<float> l ((size_t) p->taps), r ((size_t) p->taps);
        float dl, dr;
        auto energy = [] (const std::vector<float>& v) { double e = 0; for (float x : v) e += (double) x * x; return e; };
        p->lookup (0.0f, 0.0f, l.data(), r.data(), dl, dr);
        const double frontBal = 10 * std::log10 (energy (l) / energy (r));
        p->lookup (90.0f, 0.0f, l.data(), r.data(), dl, dr);
        const double rightIld = 10 * std::log10 (energy (r) / energy (l));
        const double itdUs = (dl - dr) / 48.0 * 1000.0;
        std::printf ("    %-34s front L/R %+.1f dB, right ILD %.1f dB, ITD %.0f us\n", s->name.c_str(), frontBal, rightIld, itdUs);
        check (std::abs (frontBal) < 2.0, "front is balanced between the ears", frontBal);
        check (rightIld > 6.0, "a source on the right is louder in the right ear", rightIld);
        check (itdUs > 550.0 && itdUs < 900.0, "interaural delay like a human head", itdUs);
    }

    // CPU: 7.1.4 film through real ears with constant head movement, 10 s of audio.
    HrtfLibrary::install (sets);
    DspChain chain;
    chain.prepare (48000.0, ChannelLayout::fromMask (0x2D63F, 12));
    HeadPose pose;
    pose.active = true;
    chain.setHeadPose (&pose);
    ProcessingParams pp;
    pp.user = makePreset (PresetId::Imax);
    pp.headphones = true;
    pp.hrtfSet = 0;
    chain.setParams (pp);
    Signal in (12, 480);
    fillNoise (in, -20.0f, 11);
    std::vector<float> ol (480), orr (480);
    float* ptrs[kMaxInputChannels] {};
    const auto t0 = juce::Time::getHighResolutionTicks();
    for (int k = 0; k < 1000; ++k)
    {
        pose.yaw = (float) (35.0 * std::sin (k * 0.02));
        for (int c = 0; c < 12; ++c) ptrs[c] = in.ch[(size_t) c].data();
        chain.process (ptrs, 480, 1.0f, ol.data(), orr.data(), nullptr);
    }
    const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    std::printf ("    7.1.4 + real ears + head tracking: %.1f%% of one CPU core\n", secs / 10.0 * 100.0);
    check (secs < 1.5, "whole chain stays under 15% of a core", secs * 10.0);
    HrtfLibrary::install ({});
#else
    std::printf ("    (skipped: source dir unknown)\n");
#endif
}

//==============================================================================
// Smart EQ (Dolby-style intelligent equalizer)
static void testIntelligentEq()
{
    std::printf ("[smart eq] steers dull material brighter, bright material darker, ignores silence\n");
    const double fs = 48000.0;

    auto pinkish = [] (std::vector<float>& l, std::vector<float>& r, unsigned seed, float tiltDb)
    {
        // pink noise (Kellet) with a first-order tilt above 3 kHz: tiltDb < 0 = dull, > 0 = bright
        std::mt19937 rng (seed);
        std::uniform_real_distribution<float> u (-1.0f, 1.0f);
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        Biquad shelf;
        shelf.c = design::make (FilterType::HighShelf, 48000.0, 3000.0, 0.7071, tiltDb);
        for (size_t i = 0; i < l.size(); ++i)
        {
            const float w = u (rng);
            b0 = 0.99886f * b0 + w * 0.0555179f; b1 = 0.99332f * b1 + w * 0.0750759f;
            b2 = 0.96900f * b2 + w * 0.1538520f; b3 = 0.86650f * b3 + w * 0.3104856f;
            b4 = 0.55000f * b4 + w * 0.5329522f; b5 = -0.7616f * b5 - w * 0.0168980f;
            const float p = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f) * 0.05f;
            b6 = w * 0.115926f;
            l[i] = r[i] = shelf.process (p);
        }
    };

    auto run = [&] (float tiltDb, int profile, bool enabled, float seconds = 12.0f) -> IntelligentEq
    {
        IntelligentEq q;
        q.prepare (fs);
        DspParams p;
        p.ieqEnabled = enabled;
        p.ieqProfile = profile;
        p.ieqAmount = 1.0f;
        q.setParams (p);
        const int n = (int) (fs * seconds);
        std::vector<float> l ((size_t) n), r ((size_t) n);
        pinkish (l, r, 7, tiltDb);
        for (int s = 0; s < n; s += 512)
        {
            const int len = std::min (512, n - s);
            q.analyse (l.data() + s, r.data() + s, len);
            q.process (l.data() + s, r.data() + s, len);
        }
        return q;
    };

    const auto dull = run (-12.0f, 0, true);
    const auto bright = run (+8.0f, 0, true);
    std::printf ("    dull material:   8k %+.1f dB, 16k %+.1f dB, 125 Hz %+.1f dB\n", dull.correctionDb (8), dull.correctionDb (9), dull.correctionDb (2));
    std::printf ("    bright material: 8k %+.1f dB, 16k %+.1f dB, 125 Hz %+.1f dB\n", bright.correctionDb (8), bright.correctionDb (9), bright.correctionDb (2));
    check (dull.correctionDb (8) > 2.0f, "dull material: treble lifted", dull.correctionDb (8));
    check (bright.correctionDb (8) < -1.5f, "bright material: treble eased", bright.correctionDb (8));
    for (int b = 0; b < IntelligentEq::kBands; ++b)
    {
        check (dull.correctionDb (b) <= 5.01f && dull.correctionDb (b) >= -4.01f, "corrections stay within +5/-4 dB", dull.correctionDb (b));
        check (bright.correctionDb (b) <= 5.01f && bright.correctionDb (b) >= -4.01f, "corrections stay within +5/-4 dB", bright.correctionDb (b));
    }
    const auto warm = run (0.0f, 1, true), detailed = run (0.0f, 2, true);
    check (warm.correctionDb (1) > detailed.correctionDb (1) && detailed.correctionDb (7) > warm.correctionDb (7),
           "Warm adds body, Detailed adds presence");

    const auto off = run (-12.0f, 0, false);
    float maxOff = 0.0f;
    for (int b = 0; b < IntelligentEq::kBands; ++b) maxOff = std::max (maxOff, std::abs (off.correctionDb (b)));
    check (maxOff == 0.0f, "disabled: no correction", maxOff);

    // silence must not move it
    IntelligentEq q;
    q.prepare (fs);
    DspParams p;
    p.ieqEnabled = true;
    q.setParams (p);
    std::vector<float> z (48000, 0.0f), z2 (48000, 0.0f);
    for (int k = 0; k < 10; ++k) { q.analyse (z.data(), z2.data(), 48000); q.process (z.data(), z2.data(), 48000); }
    float maxSilent = 0.0f;
    for (int b = 0; b < IntelligentEq::kBands; ++b) maxSilent = std::max (maxSilent, std::abs (q.correctionDb (b)));
    check (maxSilent == 0.0f, "silence leaves the EQ flat", maxSilent);
}

//==============================================================================
// TV outputs: speaker tuning, room geometry, lip-sync, setup storage
//==============================================================================
// DOLBY-STYLE: immersive 7.1.4 up-mix, centre dialogue, directional room, volume-aware tone
static void testImmersive()
{
    std::printf ("[immersive] 7.1.4 up-mix, dialogue, latency, switching, volume-aware tone\n");
    const double fs = 48000.0;
    using Feeds = std::array<std::vector<float>, ImmersiveUpmixer::kFeeds>;
    // Loudness of the spatial stage, classic vs immersive: the bed must not change the level much.
    {
        for (bool hp : { true, false })
            for (int kind = 0; kind < 3; ++kind)
            {
                double res[2] {};
                for (int imm = 0; imm < 2; ++imm)
                {
                    DspChain chain;
                    chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
                    ProcessingParams pp;
                    pp.user = makePreset (PresetId::Neutral);
                    pp.user.surroundEnabled = true;
                    pp.user.surroundIntensity = 1.0f;
                    pp.user.stereoWidth = imm ? 0.75f : 0.9f;
                    pp.user.roomAmount = imm ? 0.3f : 0.35f;
                    pp.user.heightAmount = imm ? 0.8f : 0.7f;
                    pp.user.roomSize = imm ? 0.3f : 0.45f;
                    pp.user.immersive = imm == 1;
                    pp.headphones = hp;
                    chain.setParams (pp);
                    Signal in (2, (int) (fs * 6));
                    fillNoise (in, -24.0f, 77);
                    if (kind == 1) in.ch[1] = in.ch[0];                       // mono
                    if (kind == 2) for (size_t i = 0; i < in.ch[1].size(); ++i) in.ch[1][i] = 0.7f * in.ch[0][i] + 0.71f * in.ch[1][i];   // ~0.7 coherent
                    LoudnessMeter mi; mi.prepare (fs); mi.process (in.ch[0].data(), in.ch[1].data(), in.frames);
                    std::vector<float> l, r;
                    runChain (chain, in, l, r, 480);
                    LoudnessMeter mo; mo.prepare (fs);
                    const size_t from = (size_t) fs;
                    mo.process (l.data() + from, r.data() + from, (int) (l.size() - from));
                    res[imm] = mo.integratedLufs() - mi.integratedLufs();
                }
                std::printf ("    level %s %-12s classic %+.2f LU   immersive %+.2f LU\n", hp ? "headphones" : "speakers  ",
                             kind == 0 ? "wide" : kind == 1 ? "mono" : "music-like", res[0], res[1]);
                check (std::abs (res[1]) < (kind == 1 ? 2.5 : 2.0), "immersive: spatial stage keeps the level (LU)", res[1]);
            }
    }

    auto runUpmix = [fs] (const std::vector<float>& L, const std::vector<float>& R, Feeds& out, int block,
                          float height = 0.8f, float dialogue = 0.0f, float speech = 0.0f, ImmersiveUpmixer* keep = nullptr)
    {
        ImmersiveUpmixer local;
        ImmersiveUpmixer& u = keep != nullptr ? *keep : local;
        u.prepare (fs);
        u.setParams (height, dialogue, dialogue > 0.0f ? -6.0f : 0.0f);
        u.setSpeech (speech);
        const int n = (int) L.size();
        for (auto& o : out) o.assign ((size_t) n, 0.0f);
        float* ptrs[ImmersiveUpmixer::kFeeds] {};
        for (int pos = 0; pos < n; pos += block)
        {
            const int m = std::min (block, n - pos);
            for (int f = 0; f < ImmersiveUpmixer::kFeeds; ++f) ptrs[f] = out[(size_t) f].data() + pos;
            u.process (L.data() + pos, R.data() + pos, ptrs, m);
        }
        return u.latencySamples();
    };
    auto energy = [] (const std::vector<float>& x, size_t from)
    {
        double s = 0.0;
        for (size_t i = from; i < x.size(); ++i) s += (double) x[i] * x[i];
        return s;
    };
    const int N = (int) (fs * 3);
    auto noise = [&] (unsigned seed, double hpHz)
    {
        std::mt19937 rng (seed);
        std::normal_distribution<float> nd (0.0f, 0.1f);
        Biquad hp;
        hp.c = design::make (FilterType::HighPass, fs, hpHz, 0.7071, 0.0);
        std::vector<float> v ((size_t) N);
        for (auto& x : v) x = hp.process (nd (rng));
        return v;
    };
    auto shares = [&] (const Feeds& f, double inEnergy, const char* label, double& c, double& front, double& around, double& total)
    {
        const size_t from = (size_t) (fs * 0.5);
        double e[ImmersiveUpmixer::kFeeds];
        total = 0.0;
        for (int k = 0; k < ImmersiveUpmixer::kFeeds; ++k) { e[k] = energy (f[(size_t) k], from); total += e[k]; }
        c = e[ImmersiveUpmixer::Centre] / total;
        front = (e[ImmersiveUpmixer::FrontL] + e[ImmersiveUpmixer::FrontR]) / total;
        around = (e[ImmersiveUpmixer::SurL] + e[ImmersiveUpmixer::SurR] + e[ImmersiveUpmixer::TopL] + e[ImmersiveUpmixer::TopR]) / total;
        std::printf ("    %-22s centre %4.0f%%  front %4.0f%%  surround+height %4.0f%%  level %+.2f dB\n", label,
                     100 * c, 100 * front, 100 * around, 10 * std::log10 (total / inEnergy));
    };

    const auto a = noise (1, 250.0), b = noise (2, 250.0);
    const auto inE = [&] (const std::vector<float>& L, const std::vector<float>& R)
    {
        // the input energy over the same span, accounting for the latency shift (stationary noise)
        return energy (L, (size_t) (fs * 0.5)) + energy (R, (size_t) (fs * 0.5));
    };
    Feeds f;
    double c, front, around, total;

    // Centre: identical channels -> the real centre speaker.
    runUpmix (a, a, f, 480);
    shares (f, inE (a, a), "mono (dialogue)", c, front, around, total);
    check (c > 0.9, "immersive: centred sound goes to the centre speaker", c);
    check (std::abs (10 * std::log10 (total / inE (a, a))) < 1.0, "immersive: centred sound keeps its level", 10 * std::log10 (total / inE (a, a)));

    // Uncorrelated: ambience -> around and above, little in the centre.
    runUpmix (a, b, f, 480);
    shares (f, inE (a, b), "uncorrelated (ambience)", c, front, around, total);
    check (c < 0.08, "immersive: ambience stays out of the centre", c);
    check (around > 0.45, "immersive: ambience wraps around / above", around);
    check (std::abs (10 * std::log10 (total / inE (a, b))) < 1.5, "immersive: ambience keeps its level", 10 * std::log10 (total / inE (a, b)));
    double eTop = energy (f[ImmersiveUpmixer::TopL], (size_t) fs) + energy (f[ImmersiveUpmixer::TopR], (size_t) fs);
    check (eTop / total > 0.08, "immersive: ceiling speakers get ambience", eTop / total);

    // No height: nothing overhead.
    {
        Feeds g;
        runUpmix (a, b, g, 480, 0.0f);
        const double e0 = energy (g[ImmersiveUpmixer::TopL], (size_t) fs) + energy (g[ImmersiveUpmixer::TopR], (size_t) fs);
        check (e0 < 1.0e-9, "immersive: height 0 leaves the ceiling silent", e0);
    }

    // Hard left: stays front-left.
    std::vector<float> zero ((size_t) N, 0.0f);
    runUpmix (a, zero, f, 480);
    shares (f, inE (a, zero), "hard left", c, front, around, total);
    const double fl = energy (f[ImmersiveUpmixer::FrontL], (size_t) (fs * 0.5)) / total;
    check (fl > 0.95, "immersive: hard-left sound stays front-left", fl);

    // Anti-phase (matrix surround, Lt/Rt): to the surrounds.
    std::vector<float> neg ((size_t) N);
    for (size_t i = 0; i < neg.size(); ++i) neg[i] = -a[i];
    runUpmix (a, neg, f, 480);
    shares (f, inE (a, neg), "anti-phase (Lt/Rt S)", c, front, around, total);
    check (around > 0.6, "immersive: matrix surround (anti-phase) goes to the surrounds", around);

    // Bass (< 100 Hz) is never split: identical channels stay in the front pair, not the centre.
    {
        std::vector<float> lo ((size_t) N);
        for (int i = 0; i < N; ++i) lo[(size_t) i] = 0.2f * (float) std::sin (2.0 * kPi * 50.0 * i / fs);
        runUpmix (lo, lo, f, 480);
        const double eF = energy (f[ImmersiveUpmixer::FrontL], (size_t) fs) + energy (f[ImmersiveUpmixer::FrontR], (size_t) fs);
        double eAll = 0.0;
        for (auto& x : f) eAll += energy (x, (size_t) fs);
        check (eF / eAll > 0.97, "immersive: 50 Hz bass stays in the front pair", eF / eAll);
    }

    // Latency: an impulse comes out exactly latencySamples() later; block size doesn't matter.
    {
        std::vector<float> imp ((size_t) 20000, 0.0f);
        imp[10000] = 1.0f;
        const int lat = runUpmix (imp, imp, f, 333);
        size_t at = 0;
        float best = 0.0f;
        for (size_t i = 0; i < f[ImmersiveUpmixer::Centre].size(); ++i)
        {
            const float v = std::abs (f[ImmersiveUpmixer::FrontL][i]) + std::abs (f[ImmersiveUpmixer::Centre][i]);
            if (v > best) { best = v; at = i; }
        }
        check ((int) at == 10000 + lat, "immersive: latency is exactly one frame", (double) at - 10000.0);
        check (lat * 1000.0 / fs < 12.0, "immersive: added latency under 12 ms", lat * 1000.0 / fs);

        Feeds f1, f2;
        std::vector<float> mix ((size_t) N);
        for (int i = 0; i < N; ++i) mix[(size_t) i] = a[(size_t) i] + 0.5f * b[(size_t) i];
        runUpmix (mix, a, f1, 1);
        runUpmix (mix, a, f2, 1024);
        double maxDiff = 0.0;
        for (int k = 0; k < ImmersiveUpmixer::kFeeds; ++k)
            for (size_t i = 0; i < f1[(size_t) k].size(); ++i)
                maxDiff = std::max (maxDiff, (double) std::abs (f1[(size_t) k][i] - f2[(size_t) k][i]));
        check (maxDiff < 1.0e-6, "immersive: output independent of block size", maxDiff);
    }

    // Dialogue: speech-like centre (syllable-rate bursts in the voice band) over ambience.
    {
        Biquad bp1, bp2;
        bp1.c = design::make (FilterType::HighPass, fs, 300.0, 0.7071, 0.0);
        bp2.c = design::make (FilterType::LowPass, fs, 3400.0, 0.7071, 0.0);
        std::mt19937 rng (5);
        std::normal_distribution<float> nd (0.0f, 0.1f);
        std::vector<float> L ((size_t) N), R ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            const double t = i / fs;
            const float env = (float) std::max (0.0, std::sin (2.0 * kPi * 4.0 * t));   // ~4 syllables / s
            const float voice = bp2.process (bp1.process (nd (rng))) * 2.0f * env;
            L[(size_t) i] = voice + 0.25f * b[(size_t) i];
            R[(size_t) i] = voice + 0.25f * a[(size_t) i];
        }
        ImmersiveUpmixer u;
        Feeds plain, lifted;
        runUpmix (L, R, plain, 480, 0.8f, 0.0f, 0.8f);
        runUpmix (L, R, lifted, 480, 0.8f, 1.0f, 0.8f, &u);
        check (u.speechGate() > 0.5, "immersive: speech in the centre opens the dialogue gate", u.speechGate());
        const double lift = 10 * std::log10 (energy (lifted[ImmersiveUpmixer::Centre], (size_t) fs) / energy (plain[ImmersiveUpmixer::Centre], (size_t) fs));
        check (lift > 2.0 && lift < 7.0, "immersive: voices lifted in the centre", lift);
        const double duck = 10 * std::log10 ((energy (lifted[ImmersiveUpmixer::SurL], (size_t) fs) + 1e-30) / (energy (plain[ImmersiveUpmixer::SurL], (size_t) fs) + 1e-30));
        check (duck < -1.5, "immersive: ambience steps back under speech", duck);

        ImmersiveUpmixer v;
        runUpmix (a, b, f, 480, 0.8f, 1.0f, 0.8f, &v);
        check (v.speechGate() < 0.15, "immersive: no gate on wide ambience", v.speechGate());
    }

    // --- whole chain ------------------------------------------------------------------
    auto makeChain = [fs] (DspChain& chain, PresetId id, bool headphones)
    {
        chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
        ProcessingParams pp;
        pp.user = makePreset (id);
        pp.headphones = headphones;
        chain.setParams (pp);
        return pp;
    };

    // Bypass ("hold to compare") is time-aligned with the processed sound.
    {
        DspChain chain;
        auto pp = makeChain (chain, PresetId::DolbyStyle, true);
        pp.bypass = true;
        chain.setParams (pp);
        check (chain.spatializer().immersiveRunning(), "chain: DOLBY-STYLE runs the immersive bed");
        Signal in (2, (int) fs);
        fillNoise (in, -26.0f, 11);
        const Signal copy = in;
        std::vector<float> l, r;
        runChain (chain, in, l, r, 512);
        const int lat = chain.latencySamples();
        double maxErr = 0.0;
        for (int i = (int) (fs * 0.2); i < in.frames; ++i)
            maxErr = std::max (maxErr, (double) std::abs (l[(size_t) i] - copy.ch[0][(size_t) (i - lat)]));
        check (maxErr < 1.0e-5, "chain: DOLBY-STYLE bypass == dry, aligned", maxErr);
    }

    // Switching presets in and out of the immersive bed never clicks.
    {
        DspChain chain;
        auto pp = makeChain (chain, PresetId::Cinema, true);
        const int total = (int) (fs * 2);
        Signal in (2, total);
        fillSine (in, 150.0, fs, 0.1f);
        std::vector<float> l ((size_t) total), r ((size_t) total);
        float* ptrs[kMaxInputChannels] {};
        for (int pos = 0, k = 0; pos < total; pos += 480, ++k)
        {
            if (k % 40 == 20)
            {
                pp.user = makePreset ((k / 40) % 2 == 0 ? PresetId::DolbyStyle : PresetId::Cinema);
                chain.setParams (pp);
            }
            ptrs[0] = in.ch[0].data() + pos;
            ptrs[1] = in.ch[1].data() + pos;
            chain.process (ptrs, 480, 1.0f, l.data() + pos, r.data() + pos, nullptr);
        }
        float maxStep = 0.0f;
        for (size_t i = (size_t) (fs * 0.3); i < l.size(); ++i)
            maxStep = std::max (maxStep, std::abs (l[i] - l[i - 1]));
        check (maxStep < 0.06f, "chain: switching to / from DOLBY-STYLE doesn't click", maxStep);
    }

    // Directional room: on headphones the immersive bed adds reflections (output differs from speakers).
    {
        DspChain hp, spk;
        makeChain (hp, PresetId::DolbyStyle, true);
        makeChain (spk, PresetId::DolbyStyle, false);
        Signal in1 (2, (int) fs), in2 (2, (int) fs);
        fillNoise (in1, -24.0f, 3);
        fillNoise (in2, -24.0f, 3);
        std::vector<float> l1, r1, l2, r2;
        runChain (hp, in1, l1, r1, 480);
        runChain (spk, in2, l2, r2, 480);
        check (allFinite (l1) && allFinite (r1) && allFinite (l2) && allFinite (r2), "chain: DOLBY-STYLE finite on headphones and speakers");
        double d = 0.0;
        for (size_t i = (size_t) (fs * 0.5); i < l1.size(); ++i) d += std::abs (l1[i] - l2[i]);
        check (d > 1.0, "chain: headphone and speaker rendering differ", d);
    }

    // No allocations while switching the bed, outputs and ears.
    {
        DspChain chain;
        auto pp = makeChain (chain, PresetId::DolbyStyle, true);
        Signal in (2, 480);
        fillNoise (in, -20.0f, 4);
        std::vector<float> ol (480), orr (480);
        allocationCount = 0;
        trackAllocations = true;
        for (int k = 0; k < 120; ++k)
        {
            pp.user = makePreset (k % 3 == 0 ? PresetId::Cinema : PresetId::DolbyStyle);
            pp.headphones = k % 5 != 0;
            pp.hrtfSet = HrtfLibrary::count() > 0 && k % 2 == 0 ? 0 : -1;
            pp.listenVolumeDb = -(float) (k % 40);
            chain.setParams (pp);
            chain.process (in.ptrs, 480, 1.0f + (float) (k % 7), ol.data(), orr.data(), nullptr);
        }
        trackAllocations = false;
        check (allocationCount == 0, "chain: no allocations with the immersive bed", (double) allocationCount);
    }

    // CPU: DOLBY-STYLE on headphones - head model, and the shipped real ears with head tracking.
    for (bool realEars : { false, true })
    {
#ifdef CINEMAX_SOURCE_DIR
        if (realEars)
        {
            std::vector<std::shared_ptr<const HrtfSet>> sets;
            for (auto& f : juce::File (CINEMAX_SOURCE_DIR).getChildFile ("data/hrtf").findChildFiles (juce::File::findFiles, false, "*.chrtf"))
            {
                juce::MemoryBlock mb;
                f.loadFileAsData (mb);
                std::string err;
                if (auto set = parseHrtf (mb.getData(), mb.getSize(), err)) sets.push_back (set);
            }
            HrtfLibrary::install (sets);
        }
#endif
        if (realEars && HrtfLibrary::count() == 0)
            continue;
        DspChain chain;
        HeadPose pose;
        pose.active = realEars;
        chain.setHeadPose (&pose);
        auto pp = makeChain (chain, PresetId::DolbyStyle, true);
        pp.hrtfSet = realEars ? 0 : -1;
        chain.setParams (pp);
        Signal in (2, 480);
        fillNoise (in, -20.0f, 8);
        std::vector<float> ol (480), orr (480);
        const int blocks = 2000;   // 20 s
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < blocks; ++k)
        {
            pose.yaw = (float) (35.0 * std::sin (k * 0.02));
            chain.process (in.ptrs, 480, 1.0f, ol.data(), orr.data(), nullptr);
        }
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("    DOLBY-STYLE on headphones (%s): %.1f %% of one core\n", realEars ? "real ears + head tracking" : "head model", 100.0 * sec / 20.0);
        check (sec / 20.0 < 0.15, "chain: DOLBY-STYLE uses < 15 % of one core", 100.0 * sec / 20.0);
        if (realEars)
            HrtfLibrary::install ({});
    }

    // --- volume-aware tone ------------------------------------------------------------
    {
        auto gainAt = [fs] (bool on, float att, double hz)
        {
            VolumeModeler vm;
            vm.prepare (fs);
            vm.setTarget (on, att);
            const int n = (int) fs;
            std::vector<float> l ((size_t) n), r ((size_t) n);
            for (int i = 0; i < n; ++i) l[(size_t) i] = r[(size_t) i] = 0.1f * (float) std::sin (2.0 * kPi * hz * i / fs);
            vm.process (l.data(), r.data(), n);
            double s = 0.0;
            for (int i = n / 2; i < n; ++i) s += (double) l[(size_t) i] * l[(size_t) i];
            return 10 * std::log10 (s / (n / 2) / 0.005);
        };
        check (std::abs (gainAt (true, 4.0f, 50.0)) < 0.05, "volume tone: nothing near full volume", gainAt (true, 4.0f, 50.0));
        check (std::abs (gainAt (false, 30.0f, 50.0)) < 0.05, "volume tone: off is transparent", gainAt (false, 30.0f, 50.0));
        const double lo = gainAt (true, 30.0f, 50.0), mid = gainAt (true, 30.0f, 1000.0), hi = gainAt (true, 30.0f, 15000.0);
        std::printf ("    30 dB down: 50 Hz %+.1f dB, 1 kHz %+.1f dB, 15 kHz %+.1f dB\n", lo, mid, hi);
        check (lo > 6.0 && lo < 9.0, "volume tone: bass restored at low volume", lo);
        check (std::abs (mid) < 0.6, "volume tone: mid-range untouched", mid);
        check (hi > 1.2 && hi < 3.2, "volume tone: a little top restored", hi);
        check (gainAt (true, 80.0f, 50.0) < 9.3, "volume tone: bass lift capped", gainAt (true, 80.0f, 50.0));

        // through the chain: the phone's media volume arrives as listenVolumeDb
        auto chainBass = [fs] (float listenDb, float deviceDb = 0.0f)
        {
            DspChain chain;
            chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
            ProcessingParams pp;
            pp.user = makePreset (PresetId::Neutral);
            pp.user.volumeModeler = true;
            pp.listenVolumeDb = listenDb;
            chain.setParams (pp);
            chain.setDeviceVolumeDb (deviceDb);
            Signal in (2, (int) fs);
            fillSine (in, 60.0, fs, 0.05f);
            std::vector<float> l, r;
            runChain (chain, in, l, r, 480);
            return rmsDb (l, (size_t) (fs * 0.5));
        };
        const double lift = chainBass (-30.0f) - chainBass (0.0f);
        check (lift > 5.0, "volume tone: phone media volume drives the bass lift", lift);
        const double liftDev = chainBass (0.0f, -30.0f) - chainBass (0.0f);
        check (liftDev > 5.0, "volume tone: Windows device volume drives the bass lift", liftDev);
    }
}

static void testTvOutput()
{
    std::printf ("[tv] speaker tuning, virtual surround geometry, lip-sync, setup storage\n");
    const double fs = 48000.0;

    // --- geometry ------------------------------------------------------------------------
    TvSetup tv;
    tv.isTv = true;
    tv.speakers = 1; tv.sizeInch = 55; tv.distanceM = 2.5f;
    const float a55 = tvSpeakerHalfAngle (tv);
    tv.distanceM = 1.2f;
    const float aClose = tvSpeakerHalfAngle (tv);
    tv.speakers = 3;
    const float aRecv = tvSpeakerHalfAngle (tv);
    std::printf ("    55-inch TV at 2.5 m: speakers +-%.1f deg; at 1.2 m: +-%.1f deg; AV receiver: +-%.0f deg\n", a55, aClose, aRecv);
    check (a55 > 9.0f && a55 < 13.0f, "55-inch TV at 2.5 m is about +-11 deg", a55);
    check (aClose > a55, "sitting closer widens the angle", aClose);
    check (aRecv == 30.0f, "AV receiver uses the standard +-30 deg");

    // --- speaker tuning response ---------------------------------------------------------------
    auto gainAt = [&] (int speakers, double f) -> double
    {
        TvSpeakerTuning t;
        t.prepare (fs);
        TvSetup s;
        s.isTv = true;
        s.speakers = speakers;
        t.setSetup (s);
        const int n = (int) fs;
        std::vector<float> l ((size_t) n), r ((size_t) n);
        for (int i = 0; i < n; ++i) l[(size_t) i] = r[(size_t) i] = 0.25f * (float) std::sin (2.0 * kPi * f * i / fs);
        t.process (l.data(), r.data(), n);
        double e = 0;
        for (int i = n / 2; i < n; ++i) e += (double) l[(size_t) i] * l[(size_t) i];
        return 20.0 * std::log10 (std::sqrt (e / (n / 2)) / (0.25 / std::sqrt (2.0)));
    };
    const double tv30 = gainAt (1, 30.0), tv160 = gainAt (1, 160.0), tv2800 = gainAt (1, 2800.0), tv1k = gainAt (1, 1000.0);
    std::printf ("    TV speakers: 30 Hz %+.1f dB, 160 Hz %+.1f dB, 1 kHz %+.1f dB, 2.8 kHz %+.1f dB\n", tv30, tv160, tv1k, tv2800);
    check (tv30 < -4.0, "TV speakers: deep bass below what they can play is reduced", tv30);
    check (tv160 > 1.0, "TV speakers: body restored around 160 Hz", tv160);
    check (tv2800 > 2.0, "TV speakers: voices forward around 2.8 kHz", tv2800);
    check (std::abs (gainAt (3, 2800.0)) < 0.1 && std::abs (gainAt (3, 50.0)) < 0.1, "AV receiver: flat");
    check (std::abs (gainAt (2, 2800.0) - 1.0) < 0.4, "soundbar: gentle presence only", gainAt (2, 2800.0));

    // --- lip-sync delay ----------------------------------------------------------------------------
    {
        LipSyncDelay d;
        d.prepare (fs);
        std::vector<float> l (48000, 0.0f), r (48000, 0.0f);
        d.process (l.data(), r.data(), 480);      // idle: untouched
        d.setDelayMs (100.0f);
        std::vector<float> warm (4800, 0.0f), warmR (4800, 0.0f);
        d.process (warm.data(), warmR.data(), 4800);   // the change happens while silent
        l.assign (48000, 0.0f); r.assign (48000, 0.0f);
        l[0] = r[0] = 1.0f;
        d.process (l.data(), r.data(), 48000);
        int peak = (int) (std::max_element (l.begin(), l.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); }) - l.begin());
        check (peak == 4800 && l[4800] == 1.0f, "lip-sync 100 ms delays by exactly 4800 samples", peak);
        check (d.delaySamples() == 4800, "latency includes the lip-sync delay");

        // changing the delay while a tone plays: a short dip, never a click
        const int n = 48000;
        std::vector<float> s ((size_t) n), sr ((size_t) n);
        for (int i = 0; i < n; ++i) s[(size_t) i] = sr[(size_t) i] = 0.5f * (float) std::sin (2.0 * kPi * 440.0 * i / fs);
        d.process (s.data(), sr.data(), n / 2);
        d.setDelayMs (37.0f);
        d.process (s.data() + n / 2, sr.data() + n / 2, n / 2);
        float maxStep = 0;
        for (int i = n / 2; i < n; ++i) maxStep = std::max (maxStep, std::abs (s[(size_t) i] - s[(size_t) i - 1]));
        check (maxStep < 0.06f, "changing lip-sync doesn't click", maxStep);   // the tone itself steps <= 0.029
    }

    // --- whole chain: TV setup on, off; no allocations --------------------------------------------
    auto runChain = [&] (bool isTv, int speakers, std::vector<float>& outL)
    {
        DspChain chain;
        chain.prepare (fs, ChannelLayout::fromMask (0x3, 2));
        ProcessingParams pp;
        pp.user = makePreset (PresetId::DolbyStyle);
        pp.headphones = false;
        pp.tv.isTv = isTv;
        pp.tv.speakers = speakers;
        chain.setParams (pp);
        Signal in (2, 48000);
        fillNoise (in, -24.0f, 21);
        outL.assign (48000, 0.0f);
        std::vector<float> outR (48000);
        chain.process (in.ptrs, 48000, 1.0f, outL.data(), outR.data(), nullptr);
    };
    std::vector<float> laptop, tvOut, recv;
    runChain (false, 1, laptop);
    runChain (true, 1, tvOut);
    runChain (true, 3, recv);
    double diff = 0;
    for (size_t i = 24000; i < laptop.size(); ++i) diff += std::abs (laptop[i] - tvOut[i]);
    check (diff > 1.0, "TV setup changes the sound (tuning + geometry)", diff);
    bool finite = true;
    for (float v : tvOut) finite = finite && std::isfinite (v) && std::abs (v) <= 1.0f;
    for (float v : recv) finite = finite && std::isfinite (v) && std::abs (v) <= 1.0f;
    check (finite, "TV output finite and within the ceiling");

    {
        DspChain chain;
        chain.prepare (fs, ChannelLayout::fromMask (0x63F, 8));
        Signal in (8, 480);
        fillNoise (in, -20.0f, 4);
        std::vector<float> ol (480), orr (480);
        allocationCount = 0;
        trackAllocations = true;
        for (int k = 0; k < 60; ++k)
        {
            ProcessingParams pp;
            pp.user = makePreset (PresetId::DolbyStyle);
            pp.tv.isTv = k % 2 == 0;
            pp.tv.speakers = 1 + k % 3;
            pp.tv.distanceM = 1.0f + (float) (k % 5);
            pp.tv.lipSyncMs = (float) ((k / 10) * 40);
            chain.setParams (pp);
            chain.process (in.ptrs, 480, 1.0f, ol.data(), orr.data(), nullptr);
        }
        trackAllocations = false;
        check (allocationCount == 0, "no allocations with TV tuning, geometry and lip-sync changes", (double) allocationCount);
    }

    // --- TVs over Bluetooth are recognised by name (Windows only says speaker / headphones) -------
    for (auto* n : { L"[TV] Samsung 7 Series (55)", L"LG webOS TV UQ7500", L"BRAVIA 4K VH2", L"Mi TV 4A Horizon",
                     L"Hisense 55A6", L"OnePlus TV 43Y1", L"realme Smart TV", L"TCL 50P635", L"Samsung QLED" })
        check (win::looksLikeTv (n), "TV name recognised");
    for (auto* n : { L"Galaxy Buds2 Pro", L"Headphones (TWS)", L"Speakers (Cirrus Logic High Definition Audio)", L"JBL Flip 5",
                     L"boAt Rockerz 450", L"Activity tracker", L"Sony WH-1000XM4", L"CABLE Input (VB-Audio Virtual Cable)" })
        check (! win::looksLikeTv (n), "non-TV name not mistaken for a TV");

    // --- setup storage (JSON) ------------------------------------------------------------------
    TvSetup t;
    t.isTv = true; t.speakers = 2; t.sizeInch = 65; t.distanceM = 3.2f; t.lipSyncMs = 80; t.letTvDecode = true;
    const auto back = mergeTv (TvSetup {}, toVar (t));
    check (back.isTv && back.speakers == 2 && back.sizeInch == 65.0f && back.distanceM == 3.2f && back.lipSyncMs == 80.0f && back.letTvDecode,
           "TV setup survives saving and loading");
    auto* bad = new juce::DynamicObject();
    bad->setProperty ("sizeInch", 9999);
    bad->setProperty ("lipSyncMs", -50);
    bad->setProperty ("speakers", "spaceship");
    const auto clamped = mergeTv (TvSetup {}, juce::var (bad));
    check (clamped.sizeInch == 120.0f && clamped.lipSyncMs == 0.0f && clamped.speakers == 1, "bad TV values are clamped");
}

//==============================================================================
// CINEMAX Vision: picture maths (the 3D LUT loaded into the display pipe).
static void testPicture()
{
    using namespace cinemax::vision;
    std::printf ("\n[picture]\n");

    // Neutral settings must be an exact identity (Vision on + Custom default = untouched picture).
    {
        PictureTransform t (PictureParams {});
        double maxErr = 0.0;
        for (int r = 0; r <= 16; ++r)
            for (int g = 0; g <= 16; ++g)
                for (int b = 0; b <= 16; ++b)
                {
                    const auto o = t.apply ({ r / 16.0, g / 16.0, b / 16.0 });
                    maxErr = std::max ({ maxErr, std::abs (o.r - r / 16.0), std::abs (o.g - g / 16.0), std::abs (o.b - b / 16.0) });
                }
        check (maxErr == 0.0, "picture: neutral transform is the exact identity", maxErr);
        check (isNeutralColour (PictureParams {}), "picture: default params are neutral");
    }

    // White point: 6500 K is unchanged, warmer temperatures reduce blue, never clip white.
    {
        PictureParams p;
        const auto g = PictureTransform (p).whiteGains();
        check (std::abs (g[0] - 1.0) < 0.01 && std::abs (g[1] - 1.0) < 0.01 && std::abs (g[2] - 1.0) < 0.01, "picture: 6500 K gains = 1", g[2]);
        p.temperatureK = 5000;
        const auto w = PictureTransform (p).whiteGains();
        check (w[2] < w[1] && w[1] < w[0] && w[0] <= 1.0 + 1e-9, "picture: 5000 K is warm and white does not clip", w[2]);
        PictureParams b;
        b.blueLight = 60;
        check (PictureTransform (b).whiteGains()[2] < 0.75, "picture: blue-light reduction lowers blue", PictureTransform (b).whiteGains()[2]);
    }

    // Every preset: grey ramp monotonic, black stays black, shadow + highlight detail kept.
    for (auto& info : picturePresetCatalog())
    {
        const auto a = analysePicture (makePicturePreset (info.id));
        char name[128];
        std::snprintf (name, sizeof (name), "picture %s: monotonic grey ramp", info.key);
        check (a.monotonic, name);
        std::snprintf (name, sizeof (name), "picture %s: black stays black", info.key);
        check (a.black[0] <= 1 && a.black[1] <= 1 && a.black[2] <= 1, name, a.black[1]);
        std::snprintf (name, sizeof (name), "picture %s: >= 20/25 near-black steps kept", info.key);
        check (a.shadowStepsKept >= 20, name, a.shadowStepsKept);
        std::snprintf (name, sizeof (name), "picture %s: >= 20/25 near-white steps kept", info.key);
        check (a.highlightStepsKept >= 20, name, a.highlightStepsKept);
    }

    // Extreme user settings stay monotonic (no reversed tones, no wrap-around).
    {
        PictureParams e;
        e.contrast = 50; e.blackLevel = 20; e.gamma = 1.8f; e.brightness = 50; e.saturation = 50; e.vibrance = 100;
        check (analysePicture (e).monotonic, "picture: extreme bright settings monotonic");
        PictureParams f;
        f.contrast = 50; f.blackLevel = -20; f.gamma = 2.6f; f.brightness = -50;
        check (analysePicture (f).monotonic, "picture: extreme dark settings monotonic");
        PictureParams l;
        l.contrast = -50;
        check (analysePicture (l).monotonic, "picture: minimum contrast monotonic");
    }

    // Skin protection: vibrance changes a skin tone far less than a muted sky blue.
    {
        PictureParams v;
        v.vibrance = 100;
        PictureTransform t (v);
        auto chroma = [] (Rgb c) { return std::max ({ c.r, c.g, c.b }) - std::min ({ c.r, c.g, c.b }); };
        const Rgb skin { 224 / 255.0, 172 / 255.0, 140 / 255.0 }, sky { 140 / 255.0, 165 / 255.0, 195 / 255.0 };
        const double skinGain = chroma (t.apply (skin)) / chroma (skin);
        const double skyGain = chroma (t.apply (sky)) / chroma (sky);
        check (skinGain < 1.12 && skyGain > skinGain + 0.15, "picture: vibrance protects skin tones", skinGain);
    }

    // Picture modes must be clearly different from the original picture (TV-like), measured
    // as the mean OKLab difference (x100) over natural test colours; backlight offsets sane.
    {
        auto oklab = [] (Rgb c) {
            auto lin = [] (double v) { return std::pow (std::clamp (v, 0.0, 1.0), 2.2); };
            const double r = lin (c.r), g = lin (c.g), b = lin (c.b);
            const double l = std::cbrt (0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
            const double m = std::cbrt (0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
            const double q = std::cbrt (0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
            return std::array<double, 3> { 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * q,
                                           1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * q,
                                           0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * q };
        };
        const Rgb samples[] = { { 0.878, 0.675, 0.549 }, { 0.553, 0.333, 0.220 }, { 0.471, 0.627, 0.824 }, { 0.275, 0.431, 0.196 },
                                { 0.784, 0.235, 0.196 }, { 0.157, 0.157, 0.176 }, { 0.5, 0.5, 0.5 }, { 0.922, 0.922, 0.922 } };
        for (auto& info : picturePresetCatalog())
        {
            if (info.id == PicturePreset::Custom) continue;
            PictureTransform t (makePicturePreset (info.id));
            double sum = 0.0;
            for (auto& c : samples)
            {
                const auto x = oklab (c), y = oklab (t.apply (c));
                sum += 100.0 * std::sqrt ((x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1]) + (x[2] - y[2]) * (x[2] - y[2]));
            }
            char name[128];
            std::snprintf (name, sizeof (name), "picture %s: visibly different from the original (mean dE >= 3)", info.key);
            check (sum / std::size (samples) >= 3.0, name, sum / std::size (samples));
            std::snprintf (name, sizeof (name), "picture %s: backlight offset within +-30", info.key);
            check (std::abs (presetBacklightOffset (info.id)) <= 30, name, presetBacklightOffset (info.id));
        }
    }

    // LUT: right size, values in range, neutral LUT exact.
    {
        const auto lut = build3DLut (makePicturePreset (PicturePreset::Vivid), 17);
        bool inRange = lut.size() == 17u * 17u * 17u * 3u;
        for (float x : lut) inRange = inRange && x >= 0.0f && x <= 1.0f;
        check (inRange, "picture: 17^3 LUT size and range");
        const auto id = build3DLut (PictureParams {}, 17);
        check (std::abs (id[(size_t) ((5 * 17 + 9) * 17 + 13) * 3 + 1] - 9.0f / 16.0f) < 1e-6f, "picture: neutral LUT node exact");
    }
}

int main()
{
    std::printf ("CINEMAX DSP tests\n=================\n");
    testFilterMaths();
    testTripleBufferAndLayout();
    testNeutralTransparency();
    testEqAccuracy();
    testLimiterGuarantee();
    testBypassAndSurroundOff();
    testSpatialEnergy();
    testHeightLayer();
    testVenueReverb();
    testConcertHallClarity();
    testContentAnalyzer();
    testSmartTone();
    testAppHints();
    testLoudnessMeter();
    testPresetLoudness();
    testResampler();
    testIntelligentEq();
    testTvOutput();
    testRealEars();
    testShippedRealEars();
    testImmersive();
    testNoAllocationInProcess();
    testAllPresetsStable();
    testPicture();

    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
