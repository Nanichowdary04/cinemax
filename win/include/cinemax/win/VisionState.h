// Crash-safe record of what CINEMAX Vision changed on each display, so the picture
// can always be put back: on clean exit, from the "Restore original display" button,
// and by CinemaxGuard if CINEMAX dies. JUCE-free (used by the guard).
#pragma once

#include "Displays.h"

namespace cinemax::win
{
struct VisionDisplayState
{
    UINT32 targetId = 0;
    std::wstring gdiName;
    bool lutApplied = false;          // Intel 3D LUT loaded by CINEMAX
    bool sharpnessChanged = false;    // Intel sharpness changed by CINEMAX
    bool sharpOrigEnable = false;
    UINT32 sharpOrigFilter = 0;
    float sharpOrigIntensity = 0;
    bool gammaChanged = false;        // gamma ramp changed by CINEMAX (fallback path)
    std::wstring gammaOrigHex;        // original ramp (768 x 4 hex digits)
    bool backlightChanged = false;    // backlight differs from the user's own level
    int backlightBase = -1;           // the user's own level (restored on crash / exit)
    bool internalPanel = false;
    std::wstring wmiInstance;
    bool tvSignalChanged = false;     // HDMI range / content type / game mode sent to a TV
};

struct VisionState
{
    DWORD pid = 0;
    bool engaged = false;
    std::vector<VisionDisplayState> displays;
};

std::wstring visionStateFile();   // %LOCALAPPDATA%\CINEMAX\vision-state.ini
std::optional<VisionState> loadVisionState();
bool saveVisionState (const VisionState& s);
void clearVisionState();

std::wstring rampToHex (const GammaRamp& r);
bool rampFromHex (const std::wstring& hex, GammaRamp& out);

/** Puts every recorded display back to how it was (Intel LUT default, original
    sharpness, original gamma ramp). Returns a short human-readable summary. */
std::string restoreVision (const VisionState& s);

} // namespace cinemax::win
