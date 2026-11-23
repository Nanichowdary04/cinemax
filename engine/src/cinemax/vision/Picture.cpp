#include "Picture.h"

#include <algorithm>
#include <cmath>

namespace cinemax::vision
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;

    inline double clamp01 (double v) noexcept { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

    inline double smoothstep (double e0, double e1, double x) noexcept
    {
        const double t = clamp01 ((x - e0) / (e1 - e0));
        return t * t * (3.0 - 2.0 * t);
    }

    // The panel's native response (EDID gamma 2.2) - used to reach linear light.
    inline double decode (double v) noexcept { return v <= 0.0 ? 0.0 : std::pow (v, 2.2); }
    inline double encode (double v) noexcept { return v <= 0.0 ? 0.0 : std::pow (v, 1.0 / 2.2); }

    struct Lab { double L, a, b; };

    // OKLab (Björn Ottosson, 2020) from/to linear sRGB.
    Lab linToOklab (double r, double g, double b) noexcept
    {
        double l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
        double m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
        double s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
        l = std::cbrt (l); m = std::cbrt (m); s = std::cbrt (s);
        return { 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
                 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s };
    }

    void oklabToLin (const Lab& c, double& r, double& g, double& b) noexcept
    {
        double l = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
        double m = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
        double s = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
        l = l * l * l; m = m * m * m; s = s * s * s;
        r = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
        g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
        b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    }

    inline bool inGamut (double r, double g, double b) noexcept
    {
        constexpr double e = 2.0e-4;   // OKLab round-trip precision
        return r >= -e && g >= -e && b >= -e && r <= 1.0 + e && g <= 1.0 + e && b <= 1.0 + e;
    }

    inline int code8 (double v) noexcept { return (int) std::lround (clamp01 (v) * 255.0); }
}

//==============================================================================
const std::vector<PicturePresetInfo>& picturePresetCatalog()
{
    static const std::vector<PicturePresetInfo> catalog {
        { PicturePreset::Cinema, "cinema", "CINEMA",         "Film-like colour, natural skin tones, deep but detailed shadows, warm white." },
        { PicturePreset::Hdr,    "hdr",    "HDR EXPERIENCE", "HDR-style tone curve: lifted shadow detail, rich mid-tones, protected highlights." },
        { PicturePreset::Vivid,  "vivid",  "VIVID",          "Richer colour and stronger perceived contrast, kept short of clipping." },
        { PicturePreset::Gaming, "gaming", "GAMING",         "Shadow visibility, vibrant colour, hardware processing with zero added latency." },
        { PicturePreset::Work,   "work",   "WORK",           "Neutral, natural colour with less eye-straining blue light." },
        { PicturePreset::Night,  "night",  "NIGHT",          "Warm, dimmer picture with strongly reduced blue light." },
        { PicturePreset::Custom, "custom", "CUSTOM",         "Your own picture settings for this display." },
    };
    return catalog;
}

const char* picturePresetKey (PicturePreset p)
{
    for (auto& i : picturePresetCatalog())
        if (i.id == p) return i.key;
    return "custom";
}

std::optional<PicturePreset> picturePresetFromKey (const std::string& key)
{
    for (auto& i : picturePresetCatalog())
        if (key == i.key) return i.id;
    return std::nullopt;
}

PictureParams makePicturePreset (PicturePreset id)
{
    PictureParams p;
    p.preset = id;
    switch (id)
    {
        // Values are tuned for an SDR laptop panel (~2/3 of sRGB, ~270 nits): strong enough
        // to look like a TV's picture modes, but every preset keeps black at black, white
        // unclipped and at least 20 of 25 near-black / near-white steps (tests enforce it).
        case PicturePreset::Cinema:   // warm "Movie" white, dark-room gamma, deep but detailed shadows
            p.temperatureK = 5400; p.gamma = 2.45f; p.contrast = 24; p.blackLevel = 9; p.brightness = -4;
            p.saturation = 10; p.vibrance = 42;
            break;
        case PicturePreset::Hdr:      // HDR-style: lifted shadow detail, punchy mid-tones, rich colour
            p.temperatureK = 6900; p.gamma = 2.25f; p.contrast = 40; p.blackLevel = 15; p.brightness = 8;
            p.saturation = 26; p.vibrance = 80; p.sharpness = 2;
            break;
        case PicturePreset::Vivid:    // store-mode punch: cool white, strong colour and contrast
            p.temperatureK = 9300; p.gamma = 2.2f; p.contrast = 34; p.brightness = 8; p.saturation = 50;
            p.vibrance = 100; p.sharpness = 3;
            break;
        case PicturePreset::Gaming:   // see into the shadows, vivid but not cartoonish, crisp detail
            p.temperatureK = 7200; p.gamma = 1.9f; p.blackLevel = 20; p.contrast = 14; p.saturation = 22;
            p.vibrance = 75; p.sharpness = 3;
            break;
        case PicturePreset::Work:     // calm, natural, softer blue for long sessions
            p.temperatureK = 6200; p.blueLight = 25; p.brightness = -5; p.contrast = -4; p.saturation = -8;
            p.sharpness = 1;
            break;
        case PicturePreset::Night:    // candle-warm, dim, minimal blue
            p.temperatureK = 3800; p.blueLight = 55; p.brightness = -18; p.contrast = -8; p.gamma = 2.25f;
            p.blackLevel = 4; p.saturation = -10;
            break;
        case PicturePreset::Custom:
            break;
    }
    return p;
}

int presetBacklightOffset (PicturePreset p)
{
    switch (p)
    {
        case PicturePreset::Cinema: return -8;    // dimmer, film-like
        case PicturePreset::Hdr:    return 20;    // bright highlights
        case PicturePreset::Vivid:  return 25;    // showroom bright
        case PicturePreset::Gaming: return 12;
        case PicturePreset::Work:   return -5;    // comfortable for long sessions
        case PicturePreset::Night:  return -30;   // dim
        case PicturePreset::Custom: break;
    }
    return 0;
}

void clampPicture (PictureParams& p)
{
    auto c = [] (float& v, float lo, float hi) { if (! std::isfinite (v)) v = 0.5f * (lo + hi); v = std::clamp (v, lo, hi); };
    c (p.brightness, -50, 50);
    c (p.contrast, -50, 50);
    c (p.blackLevel, -20, 20);
    c (p.gamma, 1.8f, 2.6f);
    c (p.saturation, -50, 50);
    c (p.vibrance, 0, 100);
    c (p.temperatureK, 3500, 10000);
    c (p.blueLight, 0, 100);
    for (auto& g : p.gain) c (g, -50, 50);
    for (auto& o : p.offset) c (o, -20, 20);
    p.sharpness = std::clamp (p.sharpness, 0, 5);
}

bool sameColour (const PictureParams& a, const PictureParams& b)
{
    auto eq = [] (float x, float y) { return std::fabs (x - y) < 1.0e-4f; };
    return eq (a.brightness, b.brightness) && eq (a.contrast, b.contrast) && eq (a.blackLevel, b.blackLevel)
        && eq (a.gamma, b.gamma) && eq (a.saturation, b.saturation) && eq (a.vibrance, b.vibrance)
        && eq (a.temperatureK, b.temperatureK) && eq (a.blueLight, b.blueLight)
        && eq (a.gain[0], b.gain[0]) && eq (a.gain[1], b.gain[1]) && eq (a.gain[2], b.gain[2])
        && eq (a.offset[0], b.offset[0]) && eq (a.offset[1], b.offset[1]) && eq (a.offset[2], b.offset[2])
        && a.skinProtect == b.skinProtect;
}

bool isNeutralColour (const PictureParams& p)
{
    return sameColour (p, PictureParams {});
}

//==============================================================================
std::array<double, 3> kelvinToLinearRgb (double T)
{
    T = std::clamp (T, 1667.0, 25000.0);
    const double T2 = T * T, T3 = T2 * T;
    const double x = T <= 4000.0 ? -0.2661239e9 / T3 - 0.2343589e6 / T2 + 0.8776956e3 / T + 0.179910
                                 : -3.0258469e9 / T3 + 2.1070379e6 / T2 + 0.2226347e3 / T + 0.240390;
    const double x2 = x * x, x3 = x2 * x;
    const double y = T <= 2222.0 ? -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683
                   : T <= 4000.0 ? -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867
                                 :  3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    const double X = x / y, Y = 1.0, Z = (1.0 - x - y) / y;
    const double r =  3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
    const double g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
    const double b =  0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
    const double m = std::max ({ r, g, b, 1.0e-9 });
    return { std::max (0.0, r / m), std::max (0.0, g / m), std::max (0.0, b / m) };
}

//==============================================================================
PictureTransform::PictureTransform (const PictureParams& params) : p (params)
{
    clampPicture (p);

    // White point: temperature plus blue-light reduction (in mireds), relative to the
    // Planckian 6504 K white so that 6500 K with no reduction is exactly neutral.
    const double mired = 1.0e6 / p.temperatureK + p.blueLight / 100.0 * 160.0;
    const double kEff = std::clamp (1.0e6 / mired, 1900.0, 12000.0);
    const auto t = kelvinToLinearRgb (kEff);
    const auto ref = kelvinToLinearRgb (6504.0);
    for (int c = 0; c < 3; ++c)
        gains[(size_t) c] = t[(size_t) c] / ref[(size_t) c] * (1.0 + p.gain[(size_t) c] / 100.0);
    const double m = std::max ({ gains[0], gains[1], gains[2] });
    for (auto& g : gains) g /= m;   // white never clips: the brightest channel stays at 1

    neutral = isNeutralColour (p);
    gammaExp = p.gamma / 2.2;
    blackK = p.blackLevel / 100.0;
    brightK = p.brightness / 100.0 * 0.24;   // +-50 -> +-0.12 OKLab L at mid-grey (~1 stop)
    contrastK = 1.0 + p.contrast / 100.0 * 1.2;
    satK = 1.0 + p.saturation / 100.0;
    vibK = p.vibrance / 100.0;
}

double PictureTransform::tone (double L) const noexcept
{
    if (L <= 0.0) return 0.0;

    // Gamma: target display gamma g means Y' = Y^(g / 2.2); OKLab L ~ Y^(1/3) -> same exponent on L.
    double x = std::pow (std::min (L, 1.0), gammaExp);

    // Black level. Lift fades in from true black (black stays black, near-black detail
    // rises) and fades out by the mid-tones; deepening is multiplicative so dark steps
    // never merge into black.
    const double fade = 1.0 - smoothstep (0.0, 0.55, x);
    if (blackK > 0.0)
        x += blackK * 0.6 * fade * smoothstep (0.0, 0.06, x);
    else if (blackK < 0.0)
        x *= 1.0 + blackK * 2.0 * fade;

    // Brightness: mid-tone bump; 0 and 1 are fixed points (no clipping, no grey blacks).
    x += brightK * 4.0 * x * (1.0 - x);

    // Contrast: power S-curve around mid-grey, faded out near black and white so shadow
    // and highlight detail survive ("deep but detailed").
    if (std::fabs (contrastK - 1.0) > 1.0e-9)
    {
        constexpr double piv = 0.55;
        // Highlights get a softer exponent than shadows: more punch, near-white steps kept.
        const double kHigh = 1.0 + (contrastK - 1.0) * 0.55;
        const double s = x <= piv ? piv * std::pow (x / piv, contrastK)
                                  : 1.0 - (1.0 - piv) * std::pow ((1.0 - x) / (1.0 - piv), kHigh);
        const double taper = smoothstep (0.06, 0.36, x) * (1.0 - smoothstep (0.86, 1.0, x));   // deep shadows and top highlights stay detailed
        x += (s - x) * taper;
    }
    return clamp01 (x);
}

Rgb PictureTransform::apply (Rgb in, bool* gamutLimited) const
{
    if (neutral)
        return { clamp01 (in.r), clamp01 (in.g), clamp01 (in.b) };

    const double r = decode (clamp01 (in.r)) * gains[0];
    const double g = decode (clamp01 (in.g)) * gains[1];
    const double b = decode (clamp01 (in.b)) * gains[2];

    const Lab c = linToOklab (r, g, b);
    const double L0 = c.L;
    const double L1 = tone (L0);

    double scale = 1.0, boost = 1.0;
    const double C = std::hypot (c.a, c.b);
    if (C > 1.0e-7)
    {
        double h = std::atan2 (c.b, c.a) * 180.0 / kPi;
        if (h < 0.0) h += 360.0;

        // Skin tones sit around 55 deg in OKLab: boosts are mostly held back there.
        double skin = 0.0;
        if (p.skinProtect)
        {
            double d = std::fabs (h - 55.0);
            if (d > 180.0) d = 360.0 - d;
            skin = std::exp (-(d / 22.0) * (d / 22.0));
        }

        double sat = satK;
        if (sat > 1.0) sat = 1.0 + (sat - 1.0) * (1.0 - 0.5 * skin);

        // Vibrance: strong on muted colours, fades to nothing for already-saturated ones.
        double vib = 1.0 + vibK * 1.0 * std::pow (std::max (0.0, 1.0 - C / 0.26), 1.4);
        vib = 1.0 + (vib - 1.0) * (1.0 - 0.85 * skin);

        boost = sat * vib;
        scale = boost;
        // Lightness changes carry chroma along proportionally (like a per-channel curve).
        if (L0 > 1.0e-4)
            scale *= std::clamp (L1 / L0, 0.5, 2.0);
    }

    Lab o { L1, c.a * scale, c.b * scale };
    double R, G, B;
    oklabToLin (o, R, G, B);

    // Soft gamut limiting (TV-style knee). Only the *added* chroma is compressed as a
    // colour approaches the edge of the signal range, at constant lightness and hue:
    // gradients in saturated areas keep their steps and nothing hard-clips.
    constexpr double knee = 0.9;
    double r2, g2, b2;
    oklabToLin (Lab { L1, o.a / knee, o.b / knee }, r2, g2, b2);
    if (! inGamut (r2, g2, b2))
    {
        double lo = 0.0, hi = 1.0 / knee;               // largest in-range chroma factor
        for (int it = 0; it < 20; ++it)
        {
            const double mid = 0.5 * (lo + hi);
            oklabToLin (Lab { L1, o.a * mid, o.b * mid }, r2, g2, b2);
            if (inGamut (r2, g2, b2)) lo = mid; else hi = mid;
        }
        const double fmax = lo;
        const double fOrig = boost > 1.0 ? 1.0 / boost : 1.0;   // the colour before saturation/vibrance
        double f = 1.0;
        if (fOrig >= fmax)
            f = fmax;                                            // only possible after a tone change
        else
        {
            const double ks = std::max (fOrig, knee * fmax);
            if (1.0 > ks)
                f = ks + (fmax - ks) * std::tanh ((1.0 - ks) / std::max (1.0e-9, fmax - ks));
        }
        if (f < 0.98 && gamutLimited != nullptr) *gamutLimited = true;
        oklabToLin (Lab { L1, o.a * f, o.b * f }, R, G, B);
    }

    Rgb out { encode (clamp01 (R)), encode (clamp01 (G)), encode (clamp01 (B)) };

    // Shadow white balance (2-point grey scale: offsets act mostly near black).
    double* ch[3] = { &out.r, &out.g, &out.b };
    for (int k = 0; k < 3; ++k)
    {
        const double o3 = p.offset[(size_t) k] / 100.0 * 0.15;
        if (o3 != 0.0)
        {
            const double v = *ch[k];
            *ch[k] = clamp01 (v + o3 * (1.0 - v) * (1.0 - v) * (1.0 - v) * smoothstep (0.0, 0.04, v + std::max (0.0, o3)));
        }
    }
    return out;
}

//==============================================================================
std::vector<float> build3DLut (const PictureParams& p, int n)
{
    n = std::clamp (n, 2, 65);
    PictureTransform t (p);
    std::vector<float> lut;
    lut.reserve ((size_t) n * (size_t) n * (size_t) n * 3);
    for (int ri = 0; ri < n; ++ri)
        for (int gi = 0; gi < n; ++gi)
            for (int bi = 0; bi < n; ++bi)
            {
                const auto o = t.apply ({ ri / double (n - 1), gi / double (n - 1), bi / double (n - 1) });
                lut.push_back ((float) o.r);
                lut.push_back ((float) o.g);
                lut.push_back ((float) o.b);
            }
    return lut;
}

std::array<std::vector<double>, 3> buildGreyCurves (const PictureParams& p, int n)
{
    n = std::max (2, n);
    PictureTransform t (p);
    std::array<std::vector<double>, 3> c;
    for (auto& v : c) v.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const double x = i / double (n - 1);
        const auto o = t.apply ({ x, x, x });
        c[0][(size_t) i] = o.r;
        c[1][(size_t) i] = o.g;
        c[2][(size_t) i] = o.b;
    }
    return c;
}

std::array<std::array<double, 3>, 3> saturationMatrix (const PictureParams& in)
{
    PictureParams p = in;
    clampPicture (p);
    const double s = (1.0 + p.saturation / 100.0) * (1.0 + p.vibrance / 100.0 * 0.25);
    const double lr = 0.2126 * (1.0 - s), lg = 0.7152 * (1.0 - s), lb = 0.0722 * (1.0 - s);
    return { { { lr + s, lg, lb }, { lr, lg + s, lb }, { lr, lg, lb + s } } };
}

PictureAnalysis analysePicture (const PictureParams& p)
{
    PictureTransform t (p);
    PictureAnalysis a;

    std::array<std::array<int, 3>, 256> code {};
    double prevY = -1.0;
    for (int v = 0; v < 256; ++v)
    {
        const auto o = t.apply ({ v / 255.0, v / 255.0, v / 255.0 });
        const double Y = 0.2126 * decode (o.r) + 0.7152 * decode (o.g) + 0.0722 * decode (o.b);
        if (Y + 1.0e-9 < prevY) a.monotonic = false;
        prevY = Y;
        // A grey step stays visible if any channel still changes (a tinted white keeps its steps).
        code[(size_t) v] = { code8 (o.r), code8 (o.g), code8 (o.b) };
        if (v == 0) a.black = { code8 (o.r), code8 (o.g), code8 (o.b) };
        if (v == 255) a.white = { code8 (o.r), code8 (o.g), code8 (o.b) };
    }

    auto distinct = [&] (int from, int to) {
        int n = 1;
        for (int v = from + 1; v <= to; ++v)
            if (code[(size_t) v] != code[(size_t) (v - 1)]) ++n;
        return n;
    };
    a.shadowSteps = 25;
    a.shadowStepsKept = distinct (0, 24);
    a.highlightSteps = 25;
    a.highlightStepsKept = distinct (231, 255);

    constexpr int n = 17;
    int limited = 0;
    for (int ri = 0; ri < n; ++ri)
        for (int gi = 0; gi < n; ++gi)
            for (int bi = 0; bi < n; ++bi)
            {
                bool lim = false;
                t.apply ({ ri / 16.0, gi / 16.0, bi / 16.0 }, &lim);
                if (lim) ++limited;
            }
    a.gamutLimitedPercent = 100.0 * limited / double (n * n * n);
    return a;
}

} // namespace cinemax::vision
