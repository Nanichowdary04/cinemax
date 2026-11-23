// Display detection and the display controls Windows itself offers (no JUCE):
// topology and capabilities (QueryDisplayConfig + DXGI), HDR / Advanced Color state,
// panel backlight (WMI) or monitor brightness (DDC/CI), per-display gamma ramps,
// and Windows Night Light state.
#pragma once

#include "Win32Common.h"

#include <array>

namespace cinemax::win
{
struct DisplayInfo
{
    std::wstring key;           // stable per monitor: monitor device path (EDID based)
    std::wstring name;          // "Built-in display" or the monitor's EDID name
    std::wstring gdiName;       // \\.\DISPLAY1
    std::wstring adapterName;   // "Intel(R) Iris(R) Xe Graphics"
    LUID adapterId {};
    UINT32 targetId = 0;
    UINT32 sourceId = 0;
    bool internal = false;
    bool hdmi = false;          // connected over HDMI (TVs, most monitors with speakers)
    int width = 0, height = 0;
    double refreshHz = 0.0;
    int bitsPerColor = 8;
    bool hdrSupported = false, hdrEnabled = false;
    bool wideColorSupported = false, wideColorEnabled = false;
    double sdrWhiteNits = 80.0, maxNits = 0.0, minNits = 0.0;
    std::array<double, 8> primaries {};   // Rx Ry Gx Gy Bx By Wx Wy (CIE 1931)
    double gamutAreaPercentOfSrgb = 0.0;   // triangle area relative to sRGB
    bool ddcci = false;                    // monitor answers DDC/CI commands
    int backlight = -1;                    // 0..100 when controllable, else -1
    std::wstring wmiInstance;              // WMI brightness instance for the built-in panel
};

/** Active displays. COM must be initialised on the calling thread (WMI part). */
std::vector<DisplayInfo> listDisplays();

/** Hash of the current display topology (paths, modes, HDR state) - cheap. */
std::string displayTopologyKey();

/** Turns Windows HDR on/off. Only call on an explicit user request; never forced. */
bool setHdr (const DisplayInfo& d, bool on);

/** Backlight (built-in panel, WMI) or monitor brightness (external, DDC/CI). */
bool setBacklight (const DisplayInfo& d, int percent);
/** Current backlight / monitor brightness (0..100), -1 if it can't be read. */
int getBacklight (const DisplayInfo& d);
bool setBacklightRaw (bool internalPanel, const std::wstring& wmiInstance, const std::wstring& gdiName, int percent);

/** Per-display gamma ramp (fallback picture path). */
using GammaRamp = std::array<std::array<WORD, 256>, 3>;
bool getGammaRamp (const std::wstring& gdiName, GammaRamp& out);
bool setGammaRamp (const std::wstring& gdiName, const GammaRamp& ramp);
GammaRamp identityRamp();

/** True while Windows Night Light is active. */
bool nightLightOn();

} // namespace cinemax::win
