#include "Spatializer.h"

#include <cstring>

namespace cinemax::dsp
{
namespace
{
    constexpr double kHeadRadius = 0.0875;   // metres
    constexpr double kSpeedOfSound = 343.0;  // m/s
    constexpr float kBinauralNorm = 0.6f;
    constexpr float kMeasuredNorm = 0.6f;   // measured sets are normalised to ~0 dB in front at 1 kHz
    constexpr float kImmersiveHeadphoneMakeup = 1.2589f;   // +2 dB (measured: DspTests [immersive])

    int nextPow2 (int v)
    {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }
}

//==============================================================================
// RoomAmbience (theatre .. concert hall)
//==============================================================================
void RoomAmbience::prepare (double sampleRate)
{
    fs = sampleRate;
    const double scale = fs / 48000.0;

    // Early reflections: tap times at the largest venue (size 1); scaled down for smaller rooms.
    const float tapsLms[kErTaps] { 9.7f, 15.1f, 21.3f, 28.9f, 37.7f, 46.3f };
    const float tapsRms[kErTaps] { 11.3f, 17.9f, 24.1f, 32.6f, 41.2f, 52.4f };
    const float gainsL[kErTaps]  { 0.30f, 0.24f, 0.19f, 0.15f, 0.11f, 0.08f };
    const float gainsR[kErTaps]  { 0.28f, 0.23f, 0.18f, 0.14f, 0.10f, 0.07f };
    for (int i = 0; i < kErTaps; ++i)
    {
        erBaseL[(size_t) i] = (float) (tapsLms[i] * 0.001 * fs);
        erBaseR[(size_t) i] = (float) (tapsRms[i] * 0.001 * fs);
        erGainL[(size_t) i] = gainsL[i];
        erGainR[(size_t) i] = gainsR[i];
    }

    const int erSize = nextPow2 ((int) (0.25 * fs) + 8);   // ER taps (<= 80 ms) + pre-delay (<= 40 ms) + margin
    erBuffer.assign ((size_t) erSize, 0.0f);
    erMask = erSize - 1;
    erWrite = 0;

    // FDN line lengths (mutually prime, 23..52 ms at 48 kHz).
    const int baseLengths[kLines] { 1123, 1307, 1489, 1657, 1871, 2053, 2281, 2477 };
    lfoDepth = (float) (12.0 * scale);
    for (size_t i = 0; i < (size_t) kLines; ++i)
    {
        auto& l = lines[i];
        l.length = std::max (16, (int) (baseLengths[i] * scale));
        const int size = nextPow2 (l.length + (int) lfoDepth + 8);
        l.data.assign ((size_t) size, 0.0f);
        l.mask = size - 1;
        l.write = 0;
    }

    // Input diffusion (Schroeder all-passes) smooths the reverb onset.
    const int diffDelays[4] { 142, 107, 379, 277 };
    for (size_t i = 0; i < diffusers.size(); ++i)
    {
        auto& d = diffusers[i];
        d.delay = std::max (1, (int) (diffDelays[i] * scale));
        const int size = nextPow2 (d.delay + 2);
        d.data.assign ((size_t) size, 0.0f);
        d.mask = size - 1;
        d.write = 0;
        d.g = i < 2 ? 0.65f : 0.55f;
    }

    inHp.c = design::make (FilterType::HighPass, fs, 160.0, 0.7071, 0.0);
    inLp.c = design::make (FilterType::LowPass, fs, 9000.0, 0.7071, 0.0);

    lfoInc1 = 2.0 * kPi * 0.31 / fs;
    lfoInc2 = 2.0 * kPi * 0.47 / fs;
    glideCoeff = (float) timeCoeff (fs, 120.0);

    revWet.reset (0.0f);
    erWetRamp.reset (0.0f);
    revWetTarget = erWetTarget = 0.0f;
    appliedSize = -1.0f;
    applySize (sizeTarget);
    preDelay = preDelayTarget;
    erScale = erScaleTarget;
    reset();
}

void RoomAmbience::reset() noexcept
{
    std::fill (erBuffer.begin(), erBuffer.end(), 0.0f);
    for (auto& l : lines)
    {
        std::fill (l.data.begin(), l.data.end(), 0.0f);
        l.damping.reset();
    }
    for (auto& d : diffusers)
        std::fill (d.data.begin(), d.data.end(), 0.0f);
    inHp.reset();
    inLp.reset();
}

void RoomAmbience::applySize (float size) noexcept
{
    size = std::clamp (size, 0.0f, 1.0f);
    if (std::abs (size - appliedSize) < 0.005f)
        return;
    appliedSize = size;

    // The per-line damping removes extra energy each pass; measured broadband decay ran
    // ~14 % short of the design value (Schroeder T30 in DspTests), so compensate.
    const double rt60 = rt60ForSize (size) * 1.16;
    // Bigger halls sound darker (air absorption over longer paths).
    const double dampHz = 7500.0 - 3000.0 * size;

    for (auto& l : lines)
    {
        l.feedback = (float) std::pow (10.0, -3.0 * (double) l.length / (fs * rt60));
        l.damping.setLowPass (fs, dampHz);
    }

    // Keep reflections dense: spacing them too far apart (>~50 ms) is heard as
    // distinct repeats ("echo") instead of spaciousness.
    preDelayTarget = (float) ((0.008 + 0.020 * size) * fs);
    erScaleTarget = 0.55f + 0.55f * size;
}

inline float RoomAmbience::readEr (float delaySamples) const noexcept
{
    const int di = (int) delaySamples;
    const float frac = delaySamples - (float) di;
    const float a = erBuffer[(size_t) ((erWrite - di) & erMask)];
    const float b = erBuffer[(size_t) ((erWrite - di - 1) & erMask)];
    return a + frac * (b - a);
}

void RoomAmbience::setParams (float reverbWet, float erWet, float size) noexcept
{
    revWetTarget = reverbWet;
    erWetTarget = erWet;
    sizeTarget = size;
    applySize (size);
}

void RoomAmbience::process (float* left, float* right, int n) noexcept
{
    revWet.setTarget (revWetTarget, (int) (0.08 * fs));
    erWetRamp.setTarget (erWetTarget, (int) (0.08 * fs));

    if (revWetTarget == 0.0f && erWetTarget == 0.0f && revWet.current == 0.0f && erWetRamp.current == 0.0f)
        return;

    constexpr float h = 0.35355339f;   // 1 / sqrt(8): orthonormal 8x8 Hadamard

    for (int i = 0; i < n; ++i)
    {
        // Parameter glides (pre-delay and ER spacing move smoothly, no clicks).
        preDelay = preDelayTarget + (preDelay - preDelayTarget) * glideCoeff;
        erScale = erScaleTarget + (erScale - erScaleTarget) * glideCoeff;

        const float in = inLp.process (inHp.process (0.5f * (left[i] + right[i])));
        erBuffer[(size_t) erWrite] = in;

        float erL = 0.0f, erR = 0.0f;
        for (int t = 0; t < kErTaps; ++t)
        {
            erL += erGainL[(size_t) t] * readEr (erBaseL[(size_t) t] * erScale);
            erR += erGainR[(size_t) t] * readEr (erBaseR[(size_t) t] * erScale);
        }

        float x = readEr (std::max (1.0f, preDelay));
        erWrite = (erWrite + 1) & erMask;

        for (auto& d : diffusers)
            x = d.process (x);

        // Read the 8 lines (two of them slowly modulated) and damp them.
        lfoPhase1 += lfoInc1; if (lfoPhase1 > 2.0 * kPi) lfoPhase1 -= 2.0 * kPi;
        lfoPhase2 += lfoInc2; if (lfoPhase2 > 2.0 * kPi) lfoPhase2 -= 2.0 * kPi;
        const float mod1 = lfoDepth * (0.5f + 0.5f * (float) std::sin (lfoPhase1));
        const float mod2 = lfoDepth * (0.5f + 0.5f * (float) std::sin (lfoPhase2));

        float s[kLines];
        for (int k = 0; k < kLines; ++k)
        {
            const float extra = k == 0 ? mod1 : (k == 4 ? mod2 : 0.0f);
            s[k] = lines[(size_t) k].damping.process (lines[(size_t) k].read ((float) lines[(size_t) k].length + extra));
        }

        // Fast Walsh-Hadamard transform (3 butterfly stages).
        float v[kLines];
        for (int k = 0; k < kLines; ++k) v[k] = s[k];
        for (int span = 1; span < kLines; span <<= 1)
            for (int a = 0; a < kLines; a += 2 * span)
                for (int b = a; b < a + span; ++b)
                {
                    const float p = v[b], q = v[b + span];
                    v[b] = p + q;
                    v[b + span] = p - q;
                }

        for (int k = 0; k < kLines; ++k)
            lines[(size_t) k].push (x * 0.35f + lines[(size_t) k].feedback * v[k] * h);

        const float revL = 0.5f * (s[0] - s[2] + s[4] - s[6] + 0.7f * s[1]);
        const float revR = 0.5f * (s[1] - s[3] + s[5] - s[7] + 0.7f * s[2]);

        const float rw = revWet.next();
        const float ew = erWetRamp.next();

        left[i]  += ew * erL + rw * revL;
        right[i] += ew * erR + rw * revR;
    }
}

//==============================================================================
// Binaural source (spherical head model)
//==============================================================================
void Spatializer::BinauralSource::configure (double fs, float azimuthDeg, float elevationDeg) noexcept
{
    const double az = azimuthDeg * kPi / 180.0;
    const double el = elevationDeg * kPi / 180.0;

    // Direction vector: x = front, y = right, z = up.
    const double y = std::cos (el) * std::sin (az);
    const double x = std::cos (el) * std::cos (az);

    // Woodworth ITD from the lateral angle (cone of confusion: front/back/up symmetric).
    const double lateral = std::asin (std::clamp (y, -1.0, 1.0));
    const double itd = (kHeadRadius / kSpeedOfSound) * (std::abs (lateral) + std::sin (std::abs (lateral)));
    const float itdSamples = (float) (itd * fs);

    delayL = lateral > 0.0 ? itdSamples : 0.0f;   // source on the right -> left ear is far
    delayR = lateral < 0.0 ? itdSamples : 0.0f;

    // Brown-Duda head shadow: H(s) = (1 + a s / 2w0) / (1 + s / 2w0), bilinear transform.
    // The angle to each ear axis comes from the 3-D direction, so elevated sources get
    // less interaural level difference - one of the cues that they are above you.
    auto shadow = [&] (OnePole& f, double thetaDeg)
    {
        constexpr double alphaMin = 0.1, thetaMin = 150.0;
        const double alpha = (1.0 + alphaMin / 2.0) + (1.0 - alphaMin / 2.0) * std::cos (thetaDeg / thetaMin * kPi);
        const double w0 = kSpeedOfSound / kHeadRadius;
        const double K = fs / w0;
        f.b0 = (1.0 + alpha * K) / (1.0 + K);
        f.b1 = (1.0 - alpha * K) / (1.0 + K);
        f.a1 = (1.0 - K) / (1.0 + K);
    };

    shadow (shadowL, std::acos (std::clamp (-y, -1.0, 1.0)) * 180.0 / kPi);   // left ear axis (0,-1,0)
    shadow (shadowR, std::acos (std::clamp (y, -1.0, 1.0)) * 180.0 / kPi);    // right ear axis (0,+1,0)

    rear = x < -0.2;
    rearL.c = rearR.c = design::make (FilterType::HighShelf, fs, 4000.0, 0.7071, -5.0);

    // Elevation cues (pinna): boost of Blauert's "above" band near 8 kHz and a dip in the
    // 3-4 kHz "front" band, growing with elevation.
    elevated = elevationDeg > 10.0f;
    const double e = std::clamp (elevationDeg / 45.0, 0.0, 1.4);
    elevBoostL.c = elevBoostR.c = design::make (FilterType::Peak, fs, std::min (8000.0, fs * 0.4), 1.4, 5.0 * e);
    elevCutL.c = elevCutR.c = design::make (FilterType::Peak, fs, 3600.0, 1.0, -3.0 * e);

    reset();
}

void Spatializer::BinauralSource::reset() noexcept
{
    ring.fill (0.0f);
    write = 0;
    shadowL.reset();
    shadowR.reset();
    for (auto* b : { &rearL, &rearR, &elevBoostL, &elevBoostR, &elevCutL, &elevCutR })
        b->reset();
}

inline void Spatializer::BinauralSource::render (float x, float& earL, float& earR) noexcept
{
    ring[(size_t) write] = x;

    auto readDelayed = [this] (float d) noexcept
    {
        const int di = (int) d;
        const float frac = d - (float) di;
        const float a = ring[(size_t) ((write - di) & (kRing - 1))];
        const float b = ring[(size_t) ((write - di - 1) & (kRing - 1))];
        return a + frac * (b - a);
    };

    float l = shadowL.process (readDelayed (delayL));
    float r = shadowR.process (readDelayed (delayR));

    if (rear)
    {
        l = rearL.process (l);
        r = rearR.process (r);
    }

    if (elevated)
    {
        l = elevCutL.process (elevBoostL.process (l));
        r = elevCutR.process (elevBoostR.process (r));
    }

    write = (write + 1) & (kRing - 1);
    earL += l;
    earR += r;
}

//==============================================================================
// Delay buffer
//==============================================================================
void Spatializer::DelayBuffer::allocate (int minSize)
{
    const int size = nextPow2 (std::max (16, minSize + 2));
    data.assign ((size_t) size, 0.0f);
    mask = size - 1;
    write = 0;
}

void Spatializer::DelayBuffer::clear() noexcept
{
    std::fill (data.begin(), data.end(), 0.0f);
    write = 0;
}

//==============================================================================
// Spatializer
//==============================================================================
void Spatializer::prepare (double sampleRate, const ChannelLayout& l, int block)
{
    fs = sampleRate;
    layout = l;
    maxBlock = block;

    srcData.assign ((size_t) kMaxSources * (size_t) maxBlock, 0.0f);
    for (int s = 0; s < kMaxSources; ++s)
        src[s] = srcData.data() + (size_t) s * (size_t) maxBlock;

    directL.assign ((size_t) maxBlock, 0.0f);
    directR.assign ((size_t) maxBlock, 0.0f);
    effL.assign ((size_t) maxBlock, 0.0f);
    effR.assign ((size_t) maxBlock, 0.0f);
    lfeBus.assign ((size_t) maxBlock, 0.0f);
    itBuf.assign ((size_t) maxBlock, 0.0f);
    wBuf.assign ((size_t) maxBlock, 1.0f);
    hBuf.assign ((size_t) maxBlock, 0.0f);
    sideBuf.assign ((size_t) maxBlock, 0.0f);
    midBuf.assign ((size_t) maxBlock, 0.0f);

    sideHp.c = design::make (FilterType::HighPass, fs, 120.0, 0.7071, 0.0);

    surroundDelayLSamples = (int) (0.011 * fs);
    surroundDelayRSamples = (int) (0.0137 * fs);
    surroundDelayL.allocate (surroundDelayLSamples);
    surroundDelayR.allocate (surroundDelayRSamples);

    const double apFreqL[3] { 420.0, 1350.0, 4100.0 };
    const double apFreqR[3] { 610.0, 1900.0, 5300.0 };
    for (size_t i = 0; i < 3; ++i)
    {
        apL[i].setAllPass (fs, apFreqL[i]);
        apR[i].setAllPass (fs, apFreqR[i]);
    }

    xtcDelaySamples = std::max (1, (int) std::lround (0.00009 * fs));
    xtcMaxSamples = std::max (2, (int) std::lround (0.0008 * fs));
    xtcDelayL.allocate (xtcMaxSamples);
    xtcDelayR.allocate (xtcMaxSamples);
    xtcBandL.c = xtcBandR.c = design::make (FilterType::HighPass, fs, 300.0, 0.7071, 0.0);
    xtcLpL.c = xtcLpR.c = design::make (FilterType::LowPass, fs, 3000.0, 0.7071, 0.0);

    lfeLp.c = design::make (FilterType::LowPass, fs, 120.0, 0.7071, 0.0);

    //                           FL     FR    C    SL      SR     BL      BR     FLC    FRC   BC     TFL    TFR   TBL     TBR    TC
    const float az[kMaxSources] { -30.0f, 30.0f, 0.0f, -110.0f, 110.0f, -145.0f, 145.0f, -15.0f, 15.0f, 180.0f, -45.0f, 45.0f, -135.0f, 135.0f, 0.0f };
    const float el[kMaxSources] { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 45.0f, 45.0f, 45.0f, 45.0f, 60.0f };
    for (int s = 0; s < kMaxSources; ++s)
    {
        binaural[(size_t) s].configure (fs, az[s], el[s]);
        srcAz[(size_t) s] = az[s];
        srcEl[(size_t) s] = el[s];
    }

    // Measured HRTFs: every installed set, resampled to this rate (cached per rate).
    hrtfSets = HrtfLibrary::preparedFor (fs);
    int maxTaps = 16;
    for (auto& s : hrtfSets)
        maxTaps = std::max (maxTaps, s->taps);
    const int fade = std::max (32, (int) (0.006 * fs));
    for (auto& v : hrir)
        v.prepare (maxTaps, fade);
    voiceSet.fill (-1);
    voiceUsed.fill (false);
    auditionVoice.prepare (maxTaps, fade);
    auditionIn.assign ((size_t) maxBlock, 0.0f);
    auditionPos = kAuditionSteps;

    // Height up-mix feeds: high-passed (bass stays on the ground), short decorrelating
    // delays (overhead reflections arrive early) and their own all-pass chains so they
    // don't correlate with the surround feeds.
    heightHpL1.c = heightHpR1.c = design::make (FilterType::HighPass, fs, 600.0, 0.5412, 0.0);
    heightHpL2.c = heightHpR2.c = design::make (FilterType::HighPass, fs, 600.0, 1.3066, 0.0);
    heightDelayLSamples = (int) (0.0073 * fs);
    heightDelayRSamples = (int) (0.0091 * fs);
    heightDelayL.allocate (heightDelayLSamples);
    heightDelayR.allocate (heightDelayRSamples);
    const double hapFreqL[3] { 330.0, 1100.0, 3300.0 };
    const double hapFreqR[3] { 470.0, 1500.0, 4600.0 };
    for (size_t i = 0; i < 3; ++i)
    {
        hapL[i].setAllPass (fs, hapFreqL[i]);
        hapR[i].setAllPass (fs, hapFreqR[i]);
    }

    // Speaker mode: same elevation cues applied to the height bus.
    spkElevBoostL.c = spkElevBoostR.c = design::make (FilterType::Peak, fs, std::min (8000.0, fs * 0.4), 1.4, 5.0);
    spkElevCutL.c = spkElevCutR.c = design::make (FilterType::Peak, fs, 3600.0, 1.0, -3.0);
    height.reset (0.0f);
    heightTarget = 0.0f;

    room.prepare (fs);

    // --- immersive bed (DOLBY-STYLE) ---------------------------------------------------
    upmix.prepare (fs);
    feedData.assign ((size_t) ImmersiveUpmixer::kFeeds * (size_t) maxBlock, 0.0f);
    for (int f = 0; f < ImmersiveUpmixer::kFeeds; ++f)
        feeds[f] = feedData.data() + (size_t) f * (size_t) maxBlock;
    alignL.allocate (upmix.latencySamples());
    alignR.allocate (upmix.latencySamples());

    // Each surround / height feed drives a pair of bed speakers (side + back, front-top +
    // back-top): energy split between the two, distinct delays (later than the front, so
    // the front stays the anchor) and all-pass chains.
    struct BedSpec { double ms; float gain; double f[3]; };
    const BedSpec specs[kBed] {
        {  8.0, 0.74f, { 420.0, 1350.0, 4100.0 } },   // SL
        { 15.0, 0.67f, { 290.0,  950.0, 3100.0 } },   // BL
        {  9.3, 0.74f, { 610.0, 1900.0, 5300.0 } },   // SR
        { 16.7, 0.67f, { 360.0, 1150.0, 3700.0 } },   // BR
        {  3.8, 0.77f, { 330.0, 1100.0, 3300.0 } },   // TFL
        { 11.0, 0.63f, { 520.0, 1700.0, 6100.0 } },   // TBL
        {  4.6, 0.77f, { 470.0, 1500.0, 4600.0 } },   // TFR
        { 12.4, 0.63f, { 260.0,  840.0, 2700.0 } },   // TBR
    };
    for (size_t b = 0; b < (size_t) kBed; ++b)
    {
        auto& d = bed[b];
        d.delay = std::max (1, (int) std::lround (specs[b].ms * 0.001 * fs));
        d.line.allocate (d.delay);
        d.gain = specs[b].gain;
        for (size_t i = 0; i < 3; ++i)
            d.ap[i].setAllPass (fs, specs[b].f[i]);
    }

    // Early reflections of a medium room: left wall, right wall, back wall, ceiling.
    struct ReflSpec { float az, el; double ms; };
    const ReflSpec rs[kReflections] { { -72.0f, 5.0f, 6.3 }, { 72.0f, 5.0f, 7.4 }, { 180.0f, 10.0f, 12.8 }, { 0.0f, 70.0f, 4.9 } };
    for (size_t r = 0; r < (size_t) kReflections; ++r)
    {
        auto& rf = refl[r];
        rf.ear.configure (fs, rs[r].az, rs[r].el);
        rf.delay = std::max (1, (int) std::lround (rs[r].ms * 0.001 * fs));
        rf.line.allocate (rf.delay);
        rf.lp.setLowPass (fs, 5500.0);    // walls absorb the top
        rf.hp.setHighPass (fs, 180.0);    // and no boom
        rf.send.fill (0.0f);
    }
    auto sends = [this] (int r, std::initializer_list<std::pair<int, float>> list)
    {
        for (auto [id, g] : list)
            refl[(size_t) r].send[(size_t) id] = g;
    };
    sends (0, { { SrcFL, 0.55f }, { SrcC, 0.30f }, { SrcFR, 0.15f }, { SrcSL, 0.50f }, { SrcBL, 0.40f }, { SrcSR, 0.10f }, { SrcTFL, 0.30f }, { SrcTBL, 0.20f } });
    sends (1, { { SrcFR, 0.55f }, { SrcC, 0.30f }, { SrcFL, 0.15f }, { SrcSR, 0.50f }, { SrcBR, 0.40f }, { SrcSL, 0.10f }, { SrcTFR, 0.30f }, { SrcTBR, 0.20f } });
    sends (2, { { SrcSL, 0.30f }, { SrcSR, 0.30f }, { SrcBL, 0.50f }, { SrcBR, 0.50f }, { SrcFL, 0.15f }, { SrcFR, 0.15f }, { SrcC, 0.10f }, { SrcTBL, 0.25f }, { SrcTBR, 0.25f } });
    sends (3, { { SrcFL, 0.30f }, { SrcFR, 0.30f }, { SrcC, 0.35f }, { SrcTFL, 0.45f }, { SrcTFR, 0.45f }, { SrcTBL, 0.35f }, { SrcTBR, 0.35f }, { SrcSL, 0.15f }, { SrcSR, 0.15f } });
    reflGain.reset (0.0f);
    modeGain.reset (1.0f);
    immersiveActive = immersiveWanted;

    intensity.reset (0.0f);
    width.reset (1.0f);
    intensityTarget = 0.0f;
    widthTarget = 1.0f;
    roomTarget = 0.0f;
    reset();
}

void Spatializer::reset() noexcept
{
    sideHp.reset();
    eLR = eLL = eRR = corrCoeff = 0.0;
    surroundDelayL.clear();
    surroundDelayR.clear();
    for (auto& a : apL) a.reset();
    for (auto& a : apR) a.reset();
    xtcDelayL.clear();
    xtcDelayR.clear();
    xtcBandL.reset(); xtcBandR.reset(); xtcLpL.reset(); xtcLpR.reset();
    lfeLp.reset();
    for (auto* b : { &heightHpL1, &heightHpL2, &heightHpR1, &heightHpR2,
                     &spkElevBoostL, &spkElevBoostR, &spkElevCutL, &spkElevCutR })
        b->reset();
    heightDelayL.clear();
    heightDelayR.clear();
    for (auto& a : hapL) a.reset();
    for (auto& a : hapR) a.reset();
    for (auto& b : binaural)
        b.reset();
    for (auto& v : hrir)
        v.reset();
    voiceSet.fill (-1);
    voiceUsed.fill (false);
    auditionVoice.reset();
    room.reset();
    resetImmersive();
    freshStart = true;
}

void Spatializer::resetImmersive() noexcept
{
    upmix.reset();
    alignL.clear();
    alignR.clear();
    for (auto& b : bed)
        b.reset();
    for (auto& rf : refl)
    {
        rf.line.clear();
        rf.lp.reset();
        rf.hp.reset();
        rf.ear.reset();
    }
}

//==============================================================================
// Measured HRTFs
//==============================================================================
void Spatializer::setSpeakerGeometry (float halfAngleDeg, float crosstalkAmount) noexcept
{
    // The far ear hears each speaker later by the interaural time difference for its angle
    // (Woodworth): that is the delay the crosstalk canceller must match.
    const double th = std::clamp ((double) halfAngleDeg, 3.0, 45.0) * kPi / 180.0;
    const double itd = (kHeadRadius / kSpeedOfSound) * (th + std::sin (th));
    xtcDelaySamples = std::clamp ((int) std::lround (itd * fs), 1, std::max (1, xtcMaxSamples));
    xtcAmount = std::clamp (crosstalkAmount, 0.0f, 0.8f);
}

void Spatializer::setHrtf (int setIndex) noexcept
{
    hrtfIndex = setIndex;
}

void Spatializer::relativeDirection (float az, float el, float& relAz, float& relEl) const noexcept
{
    // Source direction in the room (x front, y right, z up), then into head coordinates:
    // undo the head's yaw (about z) and pitch (about the ear axis y).
    const double a = az * kPi / 180.0, e = el * kPi / 180.0;
    const double x = std::cos (e) * std::cos (a), y = std::cos (e) * std::sin (a), z = std::sin (e);
    const double yaw = poseYaw * kPi / 180.0, pitch = posePitch * kPi / 180.0;
    const double x1 = x * std::cos (yaw) + y * std::sin (yaw);
    const double y1 = y * std::cos (yaw) - x * std::sin (yaw);
    const double x2 = x1 * std::cos (pitch) + z * std::sin (pitch);
    const double z2 = z * std::cos (pitch) - x1 * std::sin (pitch);
    relAz = (float) (std::atan2 (y1, x2) * 180.0 / kPi);
    relEl = (float) (std::asin (std::clamp (z2, -1.0, 1.0)) * 180.0 / kPi);
}

void Spatializer::updateHrirDirections (int numSources, const int* ids) noexcept
{
    const auto& set = *hrtfSets[(size_t) hrtfIndex];
    std::array<bool, kMaxSources> usedNow {};

    for (int k = 0; k < numSources; ++k)
    {
        const auto id = (size_t) ids[k];
        usedNow[id] = true;
        auto& v = hrir[id];

        const bool wasIdle = ! voiceUsed[id];
        if (wasIdle)
            v.reset();   // stale history from the last time this speaker was used

        const bool moved = std::abs (poseYaw - voiceYaw[id]) > 0.4f || std::abs (posePitch - voicePitch[id]) > 0.4f;
        if (! wasIdle && voiceSet[id] == hrtfIndex && ! moved && v.hasFilter())
            continue;
        if (! v.ready())
            continue;   // previous change still fading; picked up next block

        float relAz = 0.0f, relEl = 0.0f;
        relativeDirection (srcAz[id], srcEl[id], relAz, relEl);
        float dL = 0.0f, dR = 0.0f;
        float* l = v.stagingL();
        float* r = v.stagingR();
        set.lookup (relAz, relEl, l, r, dL, dR);
        std::fill (l + set.taps, l + v.numTaps(), 0.0f);
        std::fill (r + set.taps, r + v.numTaps(), 0.0f);
        v.commit (dL, dR, wasIdle || ! v.hasFilter());
        voiceSet[id] = hrtfIndex;
        voiceYaw[id] = poseYaw;
        voicePitch[id] = posePitch;
    }
    voiceUsed = usedNow;
}

//==============================================================================
// Ear-fit test
//==============================================================================
void Spatializer::setAudition (int setIndex, unsigned id) noexcept
{
    if (setIndex == -2)
    {
        auditionSet = -2;
        auditionPos = kAuditionSteps;
        return;
    }
    if (setIndex != auditionSet || id != auditionId)
    {
        auditionSet = setIndex;
        auditionId = id;
        auditionPos = 0;
        auditionSample = 0;
        auditionVoice.reset();
        auditionModel.reset();
    }
}

void Spatializer::renderAudition (float* outL, float* outR, int n) noexcept
{
    // Each position: three 120 ms pink-noise bursts (10 ms ramps) 70 ms apart, then 330 ms of
    // silence before the next position. About 0.9 s per position, 4.5 s for the whole walk.
    const int burst = (int) (0.12 * fs), gap = (int) (0.07 * fs), pause = (int) (0.33 * fs);
    const int ramp = (int) (0.01 * fs);
    const int perPos = 3 * burst + 2 * gap + pause;

    std::fill (outL, outL + n, 0.0f);
    std::fill (outR, outR + n, 0.0f);
    if (! auditionActive())
        return;

    // configure the direction at the start of each position
    if (auditionSample == 0)
    {
        const float az = kAuditionAz[auditionPos], el = kAuditionEl[auditionPos];
        if (validSet (auditionSet))
        {
            float dL = 0.0f, dR = 0.0f;
            const auto& set = *hrtfSets[(size_t) auditionSet];
            float* l = auditionVoice.stagingL();
            float* r = auditionVoice.stagingR();
            set.lookup (az, el, l, r, dL, dR);
            std::fill (l + set.taps, l + auditionVoice.numTaps(), 0.0f);
            std::fill (r + set.taps, r + auditionVoice.numTaps(), 0.0f);
            auditionVoice.commit (dL, dR, true);
        }
        else
        {
            auditionModel.configure (fs, az > 180.0f ? az - 360.0f : az, el);
        }
    }

    float* x = auditionIn.data();
    for (int i = 0; i < n; ++i)
    {
        const int t = auditionSample + i;
        float env = 0.0f;
        if (t < perPos - pause)
        {
            const int cycle = t % (burst + gap);
            if (cycle < burst)
                env = std::min ({ 1.0f, (float) cycle / ramp, (float) (burst - cycle) / ramp });
        }
        // xorshift white noise -> Paul Kellet's pink filter
        noiseState ^= noiseState << 13; noiseState ^= noiseState >> 17; noiseState ^= noiseState << 5;
        const float w = (float) (int32_t) noiseState * (1.0f / 2147483648.0f);
        pink[0] = 0.99886f * pink[0] + w * 0.0555179f;
        pink[1] = 0.99332f * pink[1] + w * 0.0750759f;
        pink[2] = 0.96900f * pink[2] + w * 0.1538520f;
        pink[3] = 0.86650f * pink[3] + w * 0.3104856f;
        pink[4] = 0.55000f * pink[4] + w * 0.5329522f;
        pink[5] = -0.7616f * pink[5] - w * 0.0168980f;
        const float p = pink[0] + pink[1] + pink[2] + pink[3] + pink[4] + pink[5] + pink[6] + w * 0.5362f;
        pink[6] = w * 0.115926f;
        x[i] = 0.06f * p * env;
    }

    if (validSet (auditionSet))
    {
        auditionVoice.process (x, outL, outR, n);
    }
    else
    {
        for (int i = 0; i < n; ++i)
        {
            float l = 0.0f, r = 0.0f;
            auditionModel.render (x[i], l, r);
            outL[i] = l * kBinauralNorm;
            outR[i] = r * kBinauralNorm;
        }
    }

    auditionSample += n;
    if (auditionSample >= perPos)
    {
        auditionSample = 0;
        ++auditionPos;   // == kAuditionSteps: finished
    }
}

void Spatializer::setParams (const DspParams& p, bool headphonesOutput) noexcept
{
    surroundOn = p.surroundEnabled;

    switch (p.surroundMode)
    {
        case SurroundMode::Headphones: headphones = true; break;
        case SurroundMode::Speakers:   headphones = false; break;
        case SurroundMode::Auto:       headphones = headphonesOutput; break;
    }

    intensityTarget = surroundOn ? p.surroundIntensity : 0.0f;
    widthTarget = surroundOn ? p.stereoWidth * 2.0f : 1.0f;
    roomTarget = surroundOn ? p.roomAmount : 0.0f;
    heightTarget = surroundOn ? p.heightAmount : 0.0f;

    // Immersive bed: stereo (or mono) input only - real multichannel is placed as it is.
    immersiveWanted = p.immersive && surroundOn && (layout.isStereo() || layout.numChannels == 1);
    upmix.setParams (p.heightAmount, p.dialogueEnabled ? p.dialogueAmount : 0.0f, p.dialogueEnabled ? p.backgroundDb : 0.0f);
    const bool directionalRoom = immersiveWanted && headphones;
    reflGainTarget = directionalRoom ? 0.55f + 0.6f * roomTarget : 0.0f;
    if (freshStart)
        immersiveActive = immersiveWanted;

    // Real speakers already play into a real room that adds its own reverberation,
    // so use less artificial room there than on headphones.
    const float speakerRoomScale = headphones ? 1.0f : 0.65f;
    const float reverbWet = roomTarget * 0.32f * speakerRoomScale;
    // With the directional reflections the generic (mono-fed) early reflections step back.
    const float erWet = ! surroundOn ? 0.0f
                      : directionalRoom ? roomTarget * 0.35f * 0.4f
                      : roomTarget * 0.35f * speakerRoomScale + (headphones ? intensityTarget * 0.12f : 0.0f);
    room.setParams (reverbWet, erWet, p.roomSize);
}

void Spatializer::buildImmersive (const float* inL, const float* inR, int n) noexcept
{
    upmix.process (inL, inR, feeds, n);
    const int lat = upmix.latencySamples();

    for (int i = 0; i < n; ++i)
    {
        directL[(size_t) i] = alignL.process (inL[i], lat);
        directR[(size_t) i] = alignR.process (inR[i], lat);

        src[SrcFL][i] = feeds[ImmersiveUpmixer::FrontL][i];
        src[SrcFR][i] = feeds[ImmersiveUpmixer::FrontR][i];
        src[SrcC][i]  = feeds[ImmersiveUpmixer::Centre][i];

        const float sl = feeds[ImmersiveUpmixer::SurL][i], sr = feeds[ImmersiveUpmixer::SurR][i];
        const float tl = feeds[ImmersiveUpmixer::TopL][i], tr = feeds[ImmersiveUpmixer::TopR][i];
        src[SrcSL][i]  = bed[BedSL].process (sl);
        src[SrcBL][i]  = bed[BedBL].process (sl);
        src[SrcSR][i]  = bed[BedSR].process (sr);
        src[SrcBR][i]  = bed[BedBR].process (sr);
        src[SrcTFL][i] = bed[BedTFL].process (tl);
        src[SrcTBL][i] = bed[BedTBL].process (tl);
        src[SrcTFR][i] = bed[BedTFR].process (tr);
        src[SrcTBR][i] = bed[BedTBR].process (tr);
    }
}

void Spatializer::addReflections (float* outL, float* outR, int n) noexcept
{
    reflGain.setTarget (reflGainTarget, (int) (0.08 * fs));
    if (reflGainTarget == 0.0f && reflGain.current == 0.0f)
        return;

    static constexpr int ids[] { SrcFL, SrcFR, SrcC, SrcSL, SrcSR, SrcBL, SrcBR, SrcTFL, SrcTFR, SrcTBL, SrcTBR };
    for (int i = 0; i < n; ++i)
    {
        const float g = reflGain.next() * kBinauralNorm;
        float l = 0.0f, r = 0.0f;
        for (auto& rf : refl)
        {
            float x = 0.0f;
            for (int id : ids)
                x += rf.send[(size_t) id] * src[id][i];
            x = rf.hp.process (rf.lp.process (x));
            rf.ear.render (rf.line.process (x, rf.delay), l, r);
        }
        outL[i] += g * l;
        outR[i] += g * r;
    }
}

void Spatializer::renderSources (int numSources, const int* ids, float* outL, float* outR, int n, bool hp) noexcept
{
    if (hp && validSet (hrtfIndex))
    {
        updateHrirDirections (numSources, ids);
        std::fill (outL, outL + n, 0.0f);
        std::fill (outR, outR + n, 0.0f);
        for (int k = 0; k < numSources; ++k)
            hrir[(size_t) ids[k]].process (src[ids[k]], outL, outR, n);
        for (int i = 0; i < n; ++i)
        {
            outL[i] *= kMeasuredNorm;
            outR[i] *= kMeasuredNorm;
        }
        return;
    }

    if (hp)
    {
        for (int i = 0; i < n; ++i)
        {
            float l = 0.0f, r = 0.0f;
            for (int k = 0; k < numSources; ++k)
                binaural[(size_t) ids[k]].render (src[ids[k]][i], l, r);
            outL[i] = l * kBinauralNorm;
            outR[i] = r * kBinauralNorm;
        }
        return;
    }

    // Speaker virtualisation: constant-power placement, surrounds folded to the
    // sides with an out-of-phase component, a height bus with elevation EQ, then
    // band-limited crosstalk cancellation.

    bool anyHeight = false;
    for (int k = 0; k < numSources; ++k)
        anyHeight = anyHeight || isHeightSource (ids[k]);

    for (int i = 0; i < n; ++i)
    {
        float l = 0.0f, r = 0.0f, hl = 0.0f, hr = 0.0f;
        for (int k = 0; k < numSources; ++k)
        {
            const float x = src[ids[k]][i];
            switch (ids[k])
            {
                case SrcFL:  l += x; break;
                case SrcFR:  r += x; break;
                case SrcC:   l += 0.7071f * x; r += 0.7071f * x; break;
                case SrcFLC: l += 0.92f * x; r += 0.38f * x; break;
                case SrcFRC: r += 0.92f * x; l += 0.38f * x; break;
                case SrcSL:  l += 0.85f * x; r -= 0.35f * x; break;
                case SrcSR:  r += 0.85f * x; l -= 0.35f * x; break;
                case SrcBL:  l += 0.75f * x; r -= 0.45f * x; break;
                case SrcBR:  r += 0.75f * x; l -= 0.45f * x; break;
                case SrcBC:  l += 0.6f * x;  r += 0.6f * x;  break;
                case SrcTFL: hl += x; break;
                case SrcTFR: hr += x; break;
                case SrcTBL: hl += 0.8f * x; break;
                case SrcTBR: hr += 0.8f * x; break;
                case SrcTC:  hl += 0.7f * x; hr += 0.7f * x; break;
                default: break;
            }
        }

        if (anyHeight)
        {
            hl = spkElevCutL.process (spkElevBoostL.process (hl));
            hr = spkElevCutR.process (spkElevBoostR.process (hr));
            l += 0.85f * hl - 0.25f * hr;
            r += 0.85f * hr - 0.25f * hl;
        }

        const float crossL = xtcLpL.process (xtcBandL.process (xtcDelayL.process (l, xtcDelaySamples)));
        const float crossR = xtcLpR.process (xtcBandR.process (xtcDelayR.process (r, xtcDelaySamples)));

        outL[i] = l - xtcAmount * crossR;
        outR[i] = r - xtcAmount * crossL;
    }
}

void Spatializer::synthesiseHeight (const float* sideW, const float* mid, int n) noexcept
{
    // Overhead layer for content without real height channels: left/right-leaning
    // ambience plus a little of the mid, high-passed, delayed and decorrelated, then
    // rendered on virtual ceiling speakers. Scaled by height x intensity per sample.
    for (int i = 0; i < n; ++i)
    {
        const float g = hBuf[(size_t) i] * 1.1f;
        float fl = 0.7f * sideW[i] + 0.45f * mid[i];
        float fr = -0.7f * sideW[i] + 0.45f * mid[i];
        fl = heightHpL2.process (heightHpL1.process (fl));
        fr = heightHpR2.process (heightHpR1.process (fr));
        fl = heightDelayL.process (fl, heightDelayLSamples);
        fr = heightDelayR.process (fr, heightDelayRSamples);
        for (auto& a : hapL) fl = a.process (fl);
        for (auto& a : hapR) fr = a.process (fr);
        src[SrcTFL][i] = g * fl;
        src[SrcTFR][i] = g * fr;
    }
}

void Spatializer::process (const float* const* in, float* outL, float* outR, int n) noexcept
{
    // Classic <-> immersive: the two paths have different latencies, so fade out, switch
    // while silent, fade back in (~15 ms each way) instead of crossfading them.
    freshStart = false;
    if (immersiveWanted != immersiveActive && modeGain.current <= 0.0f && modeGain.remaining == 0)
    {
        immersiveActive = immersiveWanted;
        resetImmersive();
    }
    modeGain.setTarget (immersiveWanted != immersiveActive ? 0.0f : 1.0f, std::max (1, (int) (0.015 * fs)));

    intensity.setTarget (intensityTarget, (int) (0.06 * fs));
    width.setTarget (widthTarget, (int) (0.06 * fs));
    height.setTarget (heightTarget, (int) (0.08 * fs));

    float* it = itBuf.data();
    float* wd = wBuf.data();
    float* hb = hBuf.data();
    for (int i = 0; i < n; ++i)
    {
        it[i] = intensity.next();
        wd[i] = width.next();
        hb[i] = height.next() * it[i];   // height lives inside the spatial effect
    }
    const bool heightActive = hb[0] > 0.0f || hb[n - 1] > 0.0f || heightTarget > 0.0f;

    const bool stereo = layout.isStereo();
    const bool mono = layout.numChannels == 1;
    const double corrK = 1.0 - timeCoeff (fs, 80.0);

    int ids[kMaxSources];
    int numSources = 0;
    bool hasLfe = false;

    if ((stereo || mono) && immersiveActive)
    {
        buildImmersive (in[0], mono ? in[0] : in[1], n);
        for (int id : { SrcFL, SrcFR, SrcC, SrcSL, SrcSR, SrcBL, SrcBR, SrcTFL, SrcTFR, SrcTBL, SrcTBR })
            ids[numSources++] = id;
    }
    else if (stereo || mono)
    {
        const float* inL = in[0];
        const float* inR = mono ? in[0] : in[1];

        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (inL[i] + inR[i]);
            const float s = 0.5f * (inL[i] - inR[i]);
            const float sHigh = sideHp.process (s);
            const float sW = (s - sHigh) + sHigh * wd[i];   // bass untouched, width above 120 Hz

            const float L = m + sW;
            const float R = m - sW;
            directL[(size_t) i] = L;
            directR[(size_t) i] = R;
            sideBuf[(size_t) i] = sW;
            midBuf[(size_t) i] = m;

            // Inter-channel correlation drives centre extraction.
            eLR += ((double) L * R - eLR) * corrK;
            eLL += ((double) L * L - eLL) * corrK;
            eRR += ((double) R * R - eRR) * corrK;
            corrCoeff = std::clamp (eLR / (std::sqrt (eLL * eRR) + 1.0e-12), 0.0, 1.0);

            const float cg = it[i] * 0.75f * (float) (corrCoeff * std::sqrt (corrCoeff));
            const float c = cg * m;

            src[SrcFL][i] = L - c;
            src[SrcFR][i] = R - c;
            src[SrcC][i]  = c;

            // Decorrelated surround ambience from the side signal.
            const float amb = sW * it[i] * 0.7f;
            float sl = surroundDelayL.process (amb, surroundDelayLSamples);
            float sr = surroundDelayR.process (amb, surroundDelayRSamples);
            for (auto& a : apL) sl = a.process (sl);
            for (auto& a : apR) sr = a.process (sr);
            src[SrcSL][i] = sl;
            src[SrcSR][i] = -sr;
        }

        ids[numSources++] = SrcFL;
        ids[numSources++] = SrcFR;
        ids[numSources++] = SrcC;
        ids[numSources++] = SrcSL;
        ids[numSources++] = SrcSR;

        if (heightActive)
        {
            synthesiseHeight (sideBuf.data(), midBuf.data(), n);
            ids[numSources++] = SrcTFL;
            ids[numSources++] = SrcTFR;
        }
    }
    else
    {
        // Multichannel (up to 7.1.4): every channel becomes a virtual source at its position.
        const int iLFE = layout.indexOf (Speaker::LFE);
        const bool haveSides = layout.indexOf (Speaker::SL) >= 0 || layout.indexOf (Speaker::SR) >= 0;
        const bool haveTops = layout.hasTopChannels();

        for (int ch = 0; ch < layout.numChannels; ++ch)
        {
            int id = -1;
            switch (layout.speakers[(size_t) ch])
            {
                case Speaker::FL:  id = SrcFL; break;
                case Speaker::FR:  id = SrcFR; break;
                case Speaker::FC:  id = SrcC; break;
                case Speaker::SL:  id = SrcSL; break;
                case Speaker::SR:  id = SrcSR; break;
                case Speaker::BL:  id = haveSides ? SrcBL : SrcSL; break;
                case Speaker::BR:  id = haveSides ? SrcBR : SrcSR; break;
                case Speaker::FLC: id = SrcFLC; break;
                case Speaker::FRC: id = SrcFRC; break;
                case Speaker::BC:  id = SrcBC; break;
                case Speaker::TFL: id = SrcTFL; break;
                case Speaker::TFR: id = SrcTFR; break;
                case Speaker::TBL: id = SrcTBL; break;
                case Speaker::TBR: id = SrcTBR; break;
                case Speaker::TC:
                case Speaker::TFC:
                case Speaker::TBC: id = SrcTC; break;
                case Speaker::LFE:
                case Speaker::Other: break;
            }

            if (id < 0)
                continue;

            bool duplicate = false;
            for (int k = 0; k < numSources; ++k)
                duplicate = duplicate || ids[k] == id;

            if (duplicate)
            {
                // e.g. TC + TFC both map to the top-centre source: sum them.
                for (int i = 0; i < n; ++i)
                    src[id][i] += in[ch][i];
                continue;
            }

            std::memcpy (src[id], in[ch], sizeof (float) * (size_t) n);
            ids[numSources++] = id;
        }

        // Width on the front pair; keep its side/mid for height synthesis.
        const bool havePair = layout.indexOf (Speaker::FL) >= 0 && layout.indexOf (Speaker::FR) >= 0;
        if (havePair)
        {
            for (int i = 0; i < n; ++i)
            {
                const float m = 0.5f * (src[SrcFL][i] + src[SrcFR][i]);
                const float s = 0.5f * (src[SrcFL][i] - src[SrcFR][i]);
                const float sHigh = sideHp.process (s);
                const float sW = (s - sHigh) + sHigh * wd[i];
                src[SrcFL][i] = m + sW;
                src[SrcFR][i] = m - sW;
                sideBuf[(size_t) i] = sW;
                midBuf[(size_t) i] = m;
            }
        }

        // ITU-style downmix as the reference ("direct") path (real channels only).
        std::fill (directL.begin(), directL.begin() + n, 0.0f);
        std::fill (directR.begin(), directR.begin() + n, 0.0f);

        for (int k = 0; k < numSources; ++k)
        {
            const float* x = src[ids[k]];
            float gl = 0.0f, gr = 0.0f;
            switch (ids[k])
            {
                case SrcFL:  gl = 1.0f; break;
                case SrcFR:  gr = 1.0f; break;
                case SrcC:   gl = gr = 0.7071f; break;
                case SrcSL:  case SrcBL: case SrcTFL: case SrcTBL: gl = 0.7071f; break;
                case SrcSR:  case SrcBR: case SrcTFR: case SrcTBR: gr = 0.7071f; break;
                case SrcFLC: gl = 0.92f; gr = 0.38f; break;
                case SrcFRC: gl = 0.38f; gr = 0.92f; break;
                case SrcBC:  case SrcTC: gl = gr = 0.5f; break;
                default: break;
            }
            for (int i = 0; i < n; ++i)
            {
                directL[(size_t) i] += gl * x[i];
                directR[(size_t) i] += gr * x[i];
            }
        }

        // No real height channels: synthesise the overhead layer (effect path only).
        if (! haveTops && havePair && heightActive)
        {
            synthesiseHeight (sideBuf.data(), midBuf.data(), n);
            ids[numSources++] = SrcTFL;
            ids[numSources++] = SrcTFR;
        }

        // LFE bus: low-passed, -3 dB, added to both outputs after the spatial stage.
        if (iLFE >= 0)
        {
            hasLfe = true;
            for (int i = 0; i < n; ++i)
                lfeBus[(size_t) i] = 0.7071f * lfeLp.process (in[iLFE][i]);
        }
    }

    // Spatial path only while it contributes (or fades).
    const bool effectActive = numSources > 0 && (it[0] > 0.0f || it[n - 1] > 0.0f);

    if (effectActive)
    {
        renderSources (numSources, ids, effL.data(), effR.data(), n, headphones);
        if (immersiveActive && headphones)
            addReflections (effL.data(), effR.data(), n);

        // The immersive bed keeps the input's energy (the classic path widens, adding level);
        // on headphones its rendering lands ~2 dB under the input for typical music, so make
        // that up to keep DOLBY-STYLE as loud as the original.
        const float mk = immersiveActive && headphones ? kImmersiveHeadphoneMakeup : 1.0f;
        for (int i = 0; i < n; ++i)
        {
            const float a = it[i];
            outL[i] = directL[(size_t) i] + a * (mk * effL[(size_t) i] - directL[(size_t) i]);
            outR[i] = directR[(size_t) i] + a * (mk * effR[(size_t) i] - directR[(size_t) i]);
        }
    }
    else
    {
        std::memcpy (outL, directL.data(), sizeof (float) * (size_t) n);
        std::memcpy (outR, directR.data(), sizeof (float) * (size_t) n);
    }

    if (modeGain.remaining > 0 || modeGain.current < 1.0f)
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = modeGain.next();
            outL[i] *= g;
            outR[i] *= g;
        }
    }

    if (hasLfe)
    {
        for (int i = 0; i < n; ++i)
        {
            outL[i] += lfeBus[(size_t) i];
            outR[i] += lfeBus[(size_t) i];
        }
    }

    room.process (outL, outR, n);
}

} // namespace cinemax::dsp
