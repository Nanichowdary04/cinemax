#include "Hrtf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
  #include <immintrin.h>
  #define CINEMAX_HRTF_SSE 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
  #include <arm_neon.h>
  #define CINEMAX_HRTF_NEON 1
#endif

namespace cinemax::dsp
{
namespace
{
    constexpr double kPiD = 3.14159265358979323846;

    struct Reader
    {
        const uint8_t* p;
        const uint8_t* end;
        bool ok = true;

        template <typename T> T get()
        {
            T v {};
            if (end - p < (ptrdiff_t) sizeof (T)) { ok = false; return v; }
            std::memcpy (&v, p, sizeof (T));
            p += sizeof (T);
            return v;
        }
        std::string str()
        {
            const auto n = get<uint32_t>();
            if (! ok || n > 4096 || end - p < (ptrdiff_t) n) { ok = false; return {}; }
            std::string s ((const char*) p, n);
            p += n;
            return s;
        }
    };

    double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 30; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    /** Band-limited (Kaiser-windowed sinc) resampling of one short impulse response. */
    void resampleIr (const float* in, int inLen, double inRate, float* out, int outLen, double outRate)
    {
        const double ratio = outRate / inRate;
        const double cutoff = std::min (1.0, ratio) * 0.95;      // of the input Nyquist
        constexpr int halfWidth = 16;
        constexpr double beta = 8.0;
        const double i0b = besselI0 (beta);
        // Keep the energy of the response: when upsampling every input sample is spread
        // over 'ratio' output samples, so scale by 1/ratio.
        const double gain = cutoff / ratio;

        for (int n = 0; n < outLen; ++n)
        {
            const double t = n / ratio;   // position in input samples
            const int k0 = (int) std::floor (t) - halfWidth + 1;
            double acc = 0.0;
            for (int k = k0; k < k0 + 2 * halfWidth; ++k)
            {
                if (k < 0 || k >= inLen)
                    continue;
                const double x = t - k;
                const double w = std::abs (x) >= halfWidth ? 0.0
                               : besselI0 (beta * std::sqrt (1.0 - (x / halfWidth) * (x / halfWidth))) / i0b;
                const double arg = kPiD * cutoff * x;
                const double sinc = std::abs (arg) < 1e-9 ? 1.0 : std::sin (arg) / arg;
                acc += in[k] * sinc * w;
            }
            out[n] = (float) (acc * gain);
        }
    }

    std::vector<std::shared_ptr<const HrtfSet>>& installed()
    {
        static std::vector<std::shared_ptr<const HrtfSet>> v;
        return v;
    }

    std::map<long long, std::vector<std::shared_ptr<const PreparedHrtf>>>& preparedCache()
    {
        static std::map<long long, std::vector<std::shared_ptr<const PreparedHrtf>>> m;
        return m;
    }
}

//==============================================================================
std::shared_ptr<const HrtfSet> parseHrtf (const void* data, size_t size, std::string& error)
{
    Reader r { (const uint8_t*) data, (const uint8_t*) data + size };
    char magic[8] {};
    for (auto& c : magic) c = r.get<char>();
    if (! r.ok || std::memcmp (magic, "CHRTF01", 8) != 0) { error = "not a CINEMAX HRTF file"; return nullptr; }

    auto s = std::make_shared<HrtfSet>();
    s->sampleRate = r.get<uint32_t>();
    s->taps = (int) r.get<uint32_t>();
    s->numAz = (int) r.get<uint32_t>();
    s->numEl = (int) r.get<uint32_t>();
    s->azStep = r.get<float>();
    s->elMin = r.get<float>();
    s->elStep = r.get<float>();
    const float irScale = r.get<float>();
    s->key = r.str();
    s->name = r.str();
    s->info = r.str();

    if (! r.ok || s->sampleRate < 8000 || s->taps < 16 || s->taps > 1024 || s->numAz < 4 || s->numAz > 720
        || s->numEl < 1 || s->numEl > 64 || s->azStep <= 0.0f || s->elStep <= 0.0f
        || std::abs (s->azStep * (float) s->numAz - 360.0f) > 0.01f)
    {
        error = "bad HRTF header";
        return nullptr;
    }

    const size_t dirs = (size_t) s->numAz * (size_t) s->numEl;
    s->delay.resize (dirs * 2);
    for (auto& d : s->delay)
        d = r.get<float>();
    s->ir.resize (dirs * 2 * (size_t) s->taps);
    for (auto& v : s->ir)
        v = (float) r.get<int16_t>() * irScale / 32767.0f;

    if (! r.ok) { error = "HRTF file is truncated"; return nullptr; }
    for (auto d : s->delay)
        if (! std::isfinite (d) || d < 0.0f || d > 200.0f) { error = "bad HRTF delay"; return nullptr; }
    return s;
}

std::shared_ptr<const PreparedHrtf> prepareHrtf (std::shared_ptr<const HrtfSet> set, double fs)
{
    auto p = std::make_shared<PreparedHrtf>();
    p->source = set;
    p->fs = fs;
    p->numAz = set->numAz;
    p->numEl = set->numEl;
    p->azStep = set->azStep;
    p->elMin = set->elMin;
    p->elStep = set->elStep;

    const double ratio = fs / set->sampleRate;
    const bool same = std::abs (ratio - 1.0) < 1e-9;
    // Minimum-phase responses start at their first sample, so a symmetric resampling kernel
    // would lose the half of it that falls before t = 0. Pad the start instead: every
    // direction and both ears get the same small extra delay, which changes nothing audible.
    constexpr int pad = 16;
    const int inLen = same ? set->taps : set->taps + pad;
    p->taps = std::clamp ((int) std::lround (inLen * ratio), 16, HrirVoice::kMaxTaps);
    const size_t dirs = (size_t) set->numAz * (size_t) set->numEl;
    p->ir.assign (dirs * 2 * (size_t) p->taps, 0.0f);
    p->delay.resize (dirs * 2);
    std::vector<float> padded ((size_t) inLen, 0.0f);

    for (size_t i = 0; i < dirs * 2; ++i)
    {
        p->delay[i] = (float) (set->delay[i] * ratio);
        const float* in = set->ir.data() + i * (size_t) set->taps;
        float* out = p->ir.data() + i * (size_t) p->taps;
        if (same)
        {
            std::memcpy (out, in, sizeof (float) * (size_t) std::min (set->taps, p->taps));
        }
        else
        {
            std::copy (in, in + set->taps, padded.begin() + pad);
            resampleIr (padded.data(), inLen, set->sampleRate, out, p->taps, fs);
        }

        // short fade-out so truncation never clicks
        const int fade = std::max (4, p->taps / 8);
        for (int k = 0; k < fade; ++k)
            out[p->taps - 1 - k] *= (float) (0.5 - 0.5 * std::cos (kPiD * k / fade));
    }
    return p;
}

void PreparedHrtf::lookup (float azDeg, float elDeg, float* irL, float* irR, float& delayL, float& delayR) const noexcept
{
    float az = std::fmod (azDeg, 360.0f);
    if (az < 0.0f) az += 360.0f;
    const float fa = az / azStep;
    const int a0 = ((int) std::floor (fa)) % numAz;
    const int a1 = (a0 + 1) % numAz;
    const float ta = fa - std::floor (fa);

    const float fe = std::clamp ((elDeg - elMin) / elStep, 0.0f, (float) (numEl - 1));
    const int e0 = std::min ((int) std::floor (fe), numEl - 1);
    const int e1 = std::min (e0 + 1, numEl - 1);
    const float te = fe - (float) e0;

    const float w[4] { (1 - ta) * (1 - te), ta * (1 - te), (1 - ta) * te, ta * te };
    const int ee[4] { e0, e0, e1, e1 };
    const int aa[4] { a0, a1, a0, a1 };

    std::fill (irL, irL + taps, 0.0f);
    std::fill (irR, irR + taps, 0.0f);
    delayL = delayR = 0.0f;
    for (int k = 0; k < 4; ++k)
    {
        if (w[k] <= 0.0f)
            continue;
        const float* l = irAt (ee[k], aa[k], 0);
        const float* r = irAt (ee[k], aa[k], 1);
        for (int i = 0; i < taps; ++i)
        {
            irL[i] += w[k] * l[i];
            irR[i] += w[k] * r[i];
        }
        delayL += w[k] * delayAt (ee[k], aa[k], 0);
        delayR += w[k] * delayAt (ee[k], aa[k], 1);
    }
}

//==============================================================================
std::mutex& HrtfLibrary::lock()
{
    static std::mutex m;
    return m;
}

void HrtfLibrary::install (std::vector<std::shared_ptr<const HrtfSet>> sets)
{
    std::lock_guard<std::mutex> g (lock());
    installed() = std::move (sets);
    preparedCache().clear();
}

std::vector<std::shared_ptr<const HrtfSet>> HrtfLibrary::sets()
{
    std::lock_guard<std::mutex> g (lock());
    return installed();
}

int HrtfLibrary::count()
{
    std::lock_guard<std::mutex> g (lock());
    return (int) installed().size();
}

int HrtfLibrary::indexOf (const std::string& key)
{
    std::lock_guard<std::mutex> g (lock());
    const auto& v = installed();
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i]->key == key)
            return (int) i;
    return -1;
}

std::vector<std::shared_ptr<const PreparedHrtf>> HrtfLibrary::preparedFor (double fs)
{
    std::lock_guard<std::mutex> g (lock());
    const auto key = (long long) std::llround (fs);
    auto& cache = preparedCache();
    auto it = cache.find (key);
    if (it != cache.end())
        return it->second;
    std::vector<std::shared_ptr<const PreparedHrtf>> out;
    for (auto& s : installed())
        out.push_back (prepareHrtf (s, fs));
    cache[key] = out;
    return out;
}

//==============================================================================
void HrirVoice::prepare (int numTaps, int fadeSamples)
{
    taps = std::clamp (numTaps, 1, kMaxTaps);
    fadeLen = std::max (16, fadeSamples);
    for (int e = 0; e < 2; ++e)
    {
        cur[(size_t) e].assign ((size_t) taps, 0.0f);
        prev[(size_t) e].assign ((size_t) taps, 0.0f);
        staging[(size_t) e].assign ((size_t) taps, 0.0f);
        hist[(size_t) e].assign ((size_t) taps * 2, 0.0f);
    }
    reset();
}

void HrirVoice::reset() noexcept
{
    inRing.fill (0.0f);
    for (auto& h : hist)
        std::fill (h.begin(), h.end(), 0.0f);
    histPos = inWrite = 0;
    fadeLeft = 0;
    primed = false;
}

void HrirVoice::setFilter (const float* irL, const float* irR, float dL, float dR, bool immediate) noexcept
{
    std::copy (irL, irL + taps, staging[0].begin());
    std::copy (irR, irR + taps, staging[1].begin());
    commit (dL, dR, immediate);
}

void HrirVoice::commit (float dL, float dR, bool immediate) noexcept
{
    const float maxDelay = (float) (kDelayRing - 4);
    for (int e = 0; e < 2; ++e)
    {
        auto& c = cur[(size_t) e];
        auto& p = prev[(size_t) e];
        const auto& s = staging[(size_t) e];
        std::swap (p, c);                                  // previous filter keeps playing during the fade
        for (int i = 0; i < taps; ++i)                     // stored reversed: newest sample * h[0]
            c[(size_t) (taps - 1 - i)] = s[(size_t) i];
    }
    delayTarget = { std::clamp (dL, 0.0f, maxDelay), std::clamp (dR, 0.0f, maxDelay) };

    if (immediate || ! primed)
    {
        delay = delayTarget;
        prev = cur;
        fadeLeft = 0;
        primed = true;
    }
    else
    {
        fadeLeft = fadeLen;
    }
}

namespace
{
    inline float dot (const float* a, const float* b, int n) noexcept
    {
#if CINEMAX_HRTF_SSE
        __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
        int i = 0;
        for (; i + 8 <= n; i += 8)
        {
            s0 = _mm_add_ps (s0, _mm_mul_ps (_mm_loadu_ps (a + i), _mm_loadu_ps (b + i)));
            s1 = _mm_add_ps (s1, _mm_mul_ps (_mm_loadu_ps (a + i + 4), _mm_loadu_ps (b + i + 4)));
        }
        s0 = _mm_add_ps (s0, s1);
        alignas (16) float t[4];
        _mm_store_ps (t, s0);
        float sum = t[0] + t[1] + t[2] + t[3];
        for (; i < n; ++i)
            sum += a[i] * b[i];
        return sum;
#elif CINEMAX_HRTF_NEON
        float32x4_t s0 = vdupq_n_f32 (0.0f), s1 = vdupq_n_f32 (0.0f);
        int i = 0;
        for (; i + 8 <= n; i += 8)
        {
            s0 = vmlaq_f32 (s0, vld1q_f32 (a + i), vld1q_f32 (b + i));
            s1 = vmlaq_f32 (s1, vld1q_f32 (a + i + 4), vld1q_f32 (b + i + 4));
        }
        s0 = vaddq_f32 (s0, s1);
        float sum = vgetq_lane_f32 (s0, 0) + vgetq_lane_f32 (s0, 1) + vgetq_lane_f32 (s0, 2) + vgetq_lane_f32 (s0, 3);
        for (; i < n; ++i)
            sum += a[i] * b[i];
        return sum;
#else
        float s[4] {};
        int i = 0;
        for (; i + 4 <= n; i += 4)
        {
            s[0] += a[i] * b[i]; s[1] += a[i + 1] * b[i + 1];
            s[2] += a[i + 2] * b[i + 2]; s[3] += a[i + 3] * b[i + 3];
        }
        float sum = s[0] + s[1] + s[2] + s[3];
        for (; i < n; ++i)
            sum += a[i] * b[i];
        return sum;
#endif
    }
}

void HrirVoice::process (const float* in, float* outL, float* outR, int n) noexcept
{
    if (! primed)
        return;

    constexpr int ringMask = kDelayRing - 1;
    constexpr float maxStep = 0.05f;   // samples of delay change per sample (inaudible pitch shift)

    for (int i = 0; i < n; ++i)
    {
        inRing[(size_t) inWrite] = in[i];

        float y[2];
        for (int e = 0; e < 2; ++e)
        {
            float& d = delay[(size_t) e];
            const float diff = delayTarget[(size_t) e] - d;
            d += std::clamp (diff, -maxStep, maxStep);

            const int di = (int) d;
            const float frac = d - (float) di;
            const float a = inRing[(size_t) ((inWrite - di) & ringMask)];
            const float b = inRing[(size_t) ((inWrite - di - 1) & ringMask)];
            const float x = a + frac * (b - a);

            auto& h = hist[(size_t) e];
            h[(size_t) histPos] = x;
            h[(size_t) (histPos + taps)] = x;
            y[e] = 0.0f;
        }

        histPos = histPos + 1 == taps ? 0 : histPos + 1;
        inWrite = (inWrite + 1) & ringMask;

        for (int e = 0; e < 2; ++e)
        {
            const float* window = hist[(size_t) e].data() + histPos;   // oldest .. newest
            float v = dot (window, cur[(size_t) e].data(), taps);
            if (fadeLeft > 0)
            {
                const float g = (float) fadeLeft / (float) fadeLen;   // weight of the old filter
                v = v * (1.0f - g) + g * dot (window, prev[(size_t) e].data(), taps);
            }
            y[e] = v;
        }
        if (fadeLeft > 0)
            --fadeLeft;

        outL[i] += y[0];
        outR[i] += y[1];
    }
}

} // namespace cinemax::dsp
