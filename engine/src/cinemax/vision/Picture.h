// CINEMAX Vision: picture parameters, Smart-TV presets and the colour maths that
// turns them into a display transform (3D LUT for the GPU display pipe, per-channel
// curves for the Windows gamma-ramp fallback). Pure C++, no Windows or JUCE.
//
// Working spaces:
//  - input/output: display-encoded RGB in [0,1] (what the desktop compositor sends)
//  - white balance, colour temperature, blue light: linear light (gamma 2.2 display)
//  - tone (brightness, contrast, gamma, black level) and colour (saturation,
//    vibrance): OKLab, so lightness, chroma and hue are adjusted independently
//  - out-of-gamut colours are pulled back by reducing chroma at constant lightness
//    and hue (no channel clipping, no hue shifts)
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace cinemax::vision
{
enum class PicturePreset { Cinema, Hdr, Vivid, Gaming, Work, Night, Custom };

struct PicturePresetInfo
{
    PicturePreset id;
    const char* key;      // "cinema"
    const char* name;     // "CINEMA"
    const char* tagline;
};

const std::vector<PicturePresetInfo>& picturePresetCatalog();
const char* picturePresetKey (PicturePreset p);
std::optional<PicturePreset> picturePresetFromKey (const std::string& key);

struct PictureParams
{
    PicturePreset preset = PicturePreset::Custom;
    float brightness = 0.0f;                     // -50..50   mid-tone lightness (black and white kept)
    float contrast = 0.0f;                       // -50..50   S-curve around mid-grey, ends protected
    float blackLevel = 0.0f;                     // -20..20   near-black lift (+) / deepen (-)
    float gamma = 2.2f;                          // 1.8..2.6  target display gamma (2.2 = unchanged)
    float saturation = 0.0f;                     // -50..50   %
    float vibrance = 0.0f;                       // 0..100    boosts muted colours, protects skin
    float temperatureK = 6500.0f;                // 3500..10000 white point (6500 = D65, unchanged)
    float blueLight = 0.0f;                      // 0..100    blue-light reduction
    std::array<float, 3> gain { 0, 0, 0 };       // -50..50   white balance, highlights (R, G, B)
    std::array<float, 3> offset { 0, 0, 0 };     // -20..20   white balance, shadows (R, G, B)
    int sharpness = 0;                           // 0 = off, 1..5 hardware sharpening levels
    bool skinProtect = true;                     // keep skin hues natural under saturation/vibrance
};

PictureParams makePicturePreset (PicturePreset p);

/** Like a TV's picture modes: backlight change (percentage points) from the user's own level. */
int presetBacklightOffset (PicturePreset p);
void clampPicture (PictureParams& p);

/** True when the colour transform is the identity (sharpness is separate hardware). */
bool isNeutralColour (const PictureParams& p);
bool sameColour (const PictureParams& a, const PictureParams& b);

struct Rgb { double r = 0, g = 0, b = 0; };

/** The complete per-pixel colour/tone transform for one parameter set. */
class PictureTransform
{
public:
    explicit PictureTransform (const PictureParams& p);

    /** Display-encoded RGB in [0,1] -> display-encoded RGB in [0,1]. */
    Rgb apply (Rgb encoded, bool* gamutLimited = nullptr) const;

    /** Linear white-point gains (temperature, blue light, RGB gain), max = 1. */
    std::array<double, 3> whiteGains() const noexcept { return gains; }

private:
    double tone (double L) const noexcept;

    PictureParams p;
    std::array<double, 3> gains { 1, 1, 1 };
    double gammaExp = 1.0, blackK = 0.0, brightK = 0.0, contrastK = 1.0;
    double satK = 1.0, vibK = 0.0;
    bool neutral = true;
};

/** n*n*n RGB triplets (R outermost, then G, then B), values in [0,1]. */
std::vector<float> build3DLut (const PictureParams& p, int n);

/** Per-channel curves (n samples each) from the grey axis - for gamma-ramp fallback. */
std::array<std::vector<double>, 3> buildGreyCurves (const PictureParams& p, int n);

/** Linear 3x3 saturation matrix approximating saturation+vibrance (fallback only). */
std::array<std::array<double, 3>, 3> saturationMatrix (const PictureParams& p);

/** Real numbers about what the transform does to the picture (shown in the panel). */
struct PictureAnalysis
{
    int shadowSteps = 0, shadowStepsKept = 0;         // 8-bit grey steps 0..24 still distinct
    int highlightSteps = 0, highlightStepsKept = 0;   // 8-bit grey steps 231..255 still distinct
    std::array<int, 3> white { 255, 255, 255 };       // output code for full white
    std::array<int, 3> black { 0, 0, 0 };             // output code for full black
    double gamutLimitedPercent = 0.0;                 // share of colours whose boost was softly limited (>2 %)
    bool monotonic = true;                            // grey ramp never reverses
};

PictureAnalysis analysePicture (const PictureParams& p);

/** Planckian white (Kim et al. approximation) converted to linear sRGB, normalised. */
std::array<double, 3> kelvinToLinearRgb (double kelvin);

} // namespace cinemax::vision
