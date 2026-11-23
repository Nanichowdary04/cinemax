#include "Upmixer.h"

#include <cstring>

namespace cinemax::dsp
{
namespace
{
    float smooth01 (float t) noexcept
    {
        t = std::clamp (t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    /** 0 below f0, 1 above f1, a smooth step on a log-frequency scale in between. */
    float logStep (double f, double f0, double f1) noexcept
    {
        if (f <= f0) return 0.0f;
        if (f >= f1) return 1.0f;
        return smooth01 ((float) (std::log (f / f0) / std::log (f1 / f0)));
    }

    // Share of the ambience energy that stays in the front pair (the rest wraps around).
    constexpr float kFrontAmbience = 0.38f;
    // Largest share of the ambience for the ceiling speakers (height amount 1, high frequencies).
    constexpr float kTopAmbience = 0.32f;
    // How strongly anti-phase (matrix-surround) primary sound is steered to the surrounds.
    constexpr float kMatrixSteer = 0.85f;
    // Centre voice lift at dialogue amount 1 while speech plays (dB, at the band's peak).
    constexpr float kMaxVoiceLiftDb = 6.0f;
    // Coherence floor of the smoothed estimate for unrelated signals (removed, see below).
    constexpr float kCoherenceBias = 0.16f;
}

void ImmersiveUpmixer::prepare (double sampleRate)
{
    fs = sampleRate;
    // ~11 ms frames at every rate (frequency resolution ~94 Hz at 48 kHz).
    frame = fs <= 24000.0 ? 256 : fs <= 50000.0 ? 512 : fs <= 100000.0 ? 1024 : 2048;
    hop = frame / 2;
    bins = frame / 2 + 1;
    fft.prepare (frame);

    // sqrt-Hann (sine) window for analysis and synthesis: w^2 overlap-adds to 1 at 50 %.
    window.resize ((size_t) frame);
    for (int i = 0; i < frame; ++i)
        window[(size_t) i] = (float) std::sin (kPi * (i + 0.5) / frame);

    bufL.assign ((size_t) frame, 0.0f);
    bufR.assign ((size_t) frame, 0.0f);
    for (auto& a : acc) a.assign ((size_t) frame, 0.0f);
    for (auto& r : ready) r.assign ((size_t) hop, 0.0f);
    spec.assign ((size_t) frame, Complex());
    work.assign ((size_t) frame, Complex());
    for (auto& f : feed) f.assign ((size_t) bins, Complex());

    for (auto* v : { &sll, &srr, &pll, &prr })
        v->assign ((size_t) bins, 0.0f);
    for (auto* v : { &slr, &plr, &xl, &xr })
        v->assign ((size_t) bins, Complex());

    const double hopSec = hop / fs;
    alpha = (float) std::exp (-hopSec / 0.06);            // ~60 ms statistics
    gateAttack = 1.0 - std::exp (-hopSec / 0.05);
    gateRelease = 1.0 - std::exp (-hopSec / 0.6);
    shareCoeff = 1.0 - std::exp (-hopSec / 0.08);

    splitW.assign ((size_t) bins, 0.0f);
    topW.assign ((size_t) bins, 0.0f);
    voiceW.assign ((size_t) bins, 0.0f);
    voiceBand.assign ((size_t) bins, 0.0f);
    for (int k = 0; k < bins; ++k)
    {
        const double f = k * fs / frame;
        splitW[(size_t) k] = logStep (f, 90.0, 180.0);
        topW[(size_t) k] = logStep (f, 500.0, 2500.0);
        voiceW[(size_t) k] = std::min (logStep (f, 250.0, 1000.0), 1.0f - logStep (f, 4000.0, 8000.0));
        voiceBand[(size_t) k] = (f >= 300.0 && f <= 3400.0) ? 1.0f : 0.0f;
    }

    reset();
}

void ImmersiveUpmixer::reset() noexcept
{
    std::fill (bufL.begin(), bufL.end(), 0.0f);
    std::fill (bufR.begin(), bufR.end(), 0.0f);
    for (auto& a : acc) std::fill (a.begin(), a.end(), 0.0f);
    for (auto& r : ready) std::fill (r.begin(), r.end(), 0.0f);
    std::fill (sll.begin(), sll.end(), 0.0f);
    std::fill (srr.begin(), srr.end(), 0.0f);
    std::fill (slr.begin(), slr.end(), Complex());
    pos = 0;
    centreShareSmoothed = 0.0;
    gate = 0.0;
}

void ImmersiveUpmixer::setParams (float height, float dialogue, float bgDb) noexcept
{
    heightAmount = std::clamp (height, 0.0f, 1.0f);
    dialogueAmount = std::clamp (dialogue, 0.0f, 1.0f);
    backgroundDb = std::clamp (bgDb, -12.0f, 0.0f);
}

void ImmersiveUpmixer::process (const float* inL, const float* inR, float* const* out, int n) noexcept
{
    const int writeAt = frame - hop;
    for (int i = 0; i < n; ++i)
    {
        bufL[(size_t) (writeAt + pos)] = inL[i];
        bufR[(size_t) (writeAt + pos)] = inR[i];
        for (int f = 0; f < kFeeds; ++f)
            out[f][i] = ready[(size_t) f][(size_t) pos];

        if (++pos == hop)
        {
            processFrame();
            pos = 0;
        }
    }
}

void ImmersiveUpmixer::processFrame() noexcept
{
    // --- analysis: L + jR in one transform --------------------------------------------
    for (int j = 0; j < frame; ++j)
        spec[(size_t) j] = Complex (bufL[(size_t) j] * window[(size_t) j], bufR[(size_t) j] * window[(size_t) j]);
    fft.forward (spec.data());

    std::memmove (bufL.data(), bufL.data() + hop, sizeof (float) * (size_t) (frame - hop));
    std::memmove (bufR.data(), bufR.data() + hop, sizeof (float) * (size_t) (frame - hop));

    const int mask = frame - 1;
    for (int k = 0; k < bins; ++k)
    {
        const Complex zk = spec[(size_t) k];
        const Complex zn = std::conj (spec[(size_t) ((frame - k) & mask)]);
        const Complex l = 0.5f * (zk + zn);
        const Complex d = zk - zn;                              // 2j * XR
        const Complex r (0.5f * d.imag(), -0.5f * d.real());
        xl[(size_t) k] = l;
        xr[(size_t) k] = r;
        pll[(size_t) k] = std::norm (l);
        prr[(size_t) k] = std::norm (r);
        plr[(size_t) k] = l * std::conj (r);
    }

    // --- smoothed covariance: [1 2 1] across frequency, ~45 ms in time -------------------
    const float b = 1.0f - alpha;
    for (int k = 0; k < bins; ++k)
    {
        const int km = std::max (0, k - 1), kp = std::min (bins - 1, k + 1);
        const float l = 0.25f * (pll[(size_t) km] + 2.0f * pll[(size_t) k] + pll[(size_t) kp]);
        const float r = 0.25f * (prr[(size_t) km] + 2.0f * prr[(size_t) k] + prr[(size_t) kp]);
        const Complex c = 0.25f * (plr[(size_t) km] + 2.0f * plr[(size_t) k] + plr[(size_t) kp]);
        sll[(size_t) k] = alpha * sll[(size_t) k] + b * l;
        srr[(size_t) k] = alpha * srr[(size_t) k] + b * r;
        slr[(size_t) k] = alpha * slr[(size_t) k] + b * c;
    }

    // --- dialogue: centre voice lift and ambience level, following the speech gate ------------
    const float g = (float) gate;
    const float liftDb = kMaxVoiceLiftDb * dialogueAmount * g;
    const float ambGain = dbToGain (backgroundDb * g);

    const float sqrtFront = std::sqrt (kFrontAmbience);
    constexpr float eps = 1.0e-20f;
    double eCentre = 0.0, eVoice = 0.0;

    for (int k = 0; k < bins; ++k)
    {
        const auto K = (size_t) k;
        const float a = sll[K], bb = srr[K];
        const Complex c = slr[K];
        // A finite average never shows two unrelated signals as fully uncorrelated (the
        // estimate's coherence floor is ~0.15 here). Remove that bias - fully for weakly
        // coherent bins, not at all for fully coherent ones - so ambience isn't mistaken
        // for a source in the centre.
        const float mRaw = std::abs (c), ab = std::sqrt (a * bb) + eps;
        const float cohRaw = std::min (1.0f, mRaw / ab);
        const float m = std::max (0.0f, mRaw - kCoherenceBias * ab * (1.0f - cohRaw * cohRaw));
        const float m2 = m * m;
        const float d = a - bb;
        const float pa = std::max (0.0f, 0.5f * ((a + bb) - std::sqrt (d * d + 4.0f * m2)));   // ambience power per channel

        const float u = splitW[K];
        const float ga2L = u * std::min (1.0f, pa / (a + eps));
        const float ga2R = u * std::min (1.0f, pa / (bb + eps));
        const Complex PL = std::sqrt (1.0f - ga2L) * xl[K];
        const Complex PR = std::sqrt (1.0f - ga2R) * xr[K];
        const Complex AL = std::sqrt (ga2L) * xl[K] * ambGain;
        const Complex AR = std::sqrt (ga2R) * xr[K] * ambGain;

        // Principal direction of the primary sound: pan -1 (left) .. +1 (right).
        const float ppL = std::max (0.0f, a - pa), ppR = std::max (0.0f, bb - pa), pp = ppL + ppR + eps;
        const float cL = std::sqrt (ppL / pp), cR = std::sqrt (ppR / pp);
        const float pan = (float) (4.0 / kPi) * std::atan2 (cR, cL) - 1.0f;
        const Complex ph = mRaw > 1.0e-18f ? c / mRaw : Complex (1.0f, 0.0f);   // rotates R onto L's phase

        // Anti-phase primary (matrix surround): only when both channels carry it coherently.
        const float coh = std::min (1.0f, m / ab);
        const float cosPhi = mRaw > 1.0e-18f ? c.real() / mRaw : 1.0f;
        const float both = 2.0f * std::sqrt (ppL * ppR) / pp;
        const float wSur = u * kMatrixSteer * std::clamp (-cosPhi, 0.0f, 1.0f) * coh * both;
        const float sF = std::sqrt (1.0f - wSur), sS = std::sqrt (wSur);

        // Re-pan the primary sound over front-left / centre / front-right.
        const Complex S = cL * PL + cR * (PR * ph);
        const float ap = std::abs (pan) * (float) (kPi * 0.5);
        const float side = std::sin (ap), gC = std::cos (ap);
        const float gFL = pan < 0.0f ? side : 0.0f, gFR = pan > 0.0f ? side : 0.0f;

        Complex fl = sF * ((1.0f - u) * PL + (u * gFL) * S);
        Complex fr = sF * ((1.0f - u) * PR + (u * gFR) * S);
        Complex cc = (sF * u * gC) * S;

        if (voiceBand[K] > 0.0f)
        {
            eCentre += std::norm (cc);
            eVoice += pll[K] + prr[K];
        }
        if (liftDb > 0.0f && voiceW[K] > 0.0f)
            cc *= dbToGain (liftDb * voiceW[K]);

        // Ambience: part stays in front, the rest goes round and (high up) overhead.
        const float t = heightAmount * kTopAmbience * topW[K];
        const float sr = std::sqrt (std::max (0.0f, 1.0f - kFrontAmbience - t));
        const float st = std::sqrt (t);
        fl += sqrtFront * AL;
        fr += sqrtFront * AR;

        feed[FrontL][K] = fl;
        feed[FrontR][K] = fr;
        feed[Centre][K] = cc;
        feed[SurL][K] = sr * AL + sS * PL;
        feed[SurR][K] = sr * AR + sS * PR;
        feed[TopL][K] = st * AL;
        feed[TopR][K] = st * AR;
    }

    // --- speech gate: voices concentrated in the centre + analyser speech evidence ------------
    const double share = eVoice > 1.0e-14 ? eCentre / eVoice : 0.0;
    centreShareSmoothed += (share - centreShareSmoothed) * shareCoeff;
    const double target = (double) smooth01 ((float) ((centreShareSmoothed - 0.25) / 0.35))
                          * (0.4 + 0.6 * std::min (1.0, (double) speechEvidence * 1.6));
    gate += (target - gate) * (target > gate ? gateAttack : gateRelease);

    // --- synthesis: two real feeds per inverse transform, windowed overlap-add ---------------
    auto synth = [this, mask] (int fa, int fb)
    {
        const auto& A = feed[(size_t) fa];
        for (int k = 0; k < bins; ++k)
        {
            const Complex x = A[(size_t) k];
            const Complex y = fb >= 0 ? feed[(size_t) fb][(size_t) k] : Complex();
            work[(size_t) k] = Complex (x.real() - y.imag(), x.imag() + y.real());              // A + jB
            if (k > 0 && k < bins - 1)
                work[(size_t) ((frame - k) & mask)] = Complex (x.real() + y.imag(), y.real() - x.imag());   // conj(A) + j conj(B)
        }
        fft.inverse (work.data());

        auto& accA = acc[(size_t) fa];
        for (int j = 0; j < frame; ++j)
            accA[(size_t) j] += work[(size_t) j].real() * window[(size_t) j];
        if (fb >= 0)
        {
            auto& accB = acc[(size_t) fb];
            for (int j = 0; j < frame; ++j)
                accB[(size_t) j] += work[(size_t) j].imag() * window[(size_t) j];
        }
    };
    synth (FrontL, FrontR);
    synth (Centre, SurL);
    synth (SurR, TopL);
    synth (TopR, -1);

    for (int f = 0; f < kFeeds; ++f)
    {
        auto& a = acc[(size_t) f];
        std::memcpy (ready[(size_t) f].data(), a.data(), sizeof (float) * (size_t) hop);
        std::memmove (a.data(), a.data() + hop, sizeof (float) * (size_t) (frame - hop));
        std::fill (a.begin() + (frame - hop), a.end(), 0.0f);
    }
}

} // namespace cinemax::dsp
