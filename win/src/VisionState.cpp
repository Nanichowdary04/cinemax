#include "cinemax/win/VisionState.h"

#include "cinemax/win/IntelPipe.h"

#include <cstdio>
#include <sstream>

namespace cinemax::win
{
std::wstring visionStateFile()
{
    return paths::dataDir() + L"\\vision-state.ini";
}

std::wstring rampToHex (const GammaRamp& r)
{
    std::wstring s;
    s.reserve (768 * 4);
    wchar_t buf[8];
    for (auto& ch : r)
        for (auto v : ch)
        {
            swprintf (buf, 8, L"%04x", (unsigned) v);
            s += buf;
        }
    return s;
}

bool rampFromHex (const std::wstring& hex, GammaRamp& out)
{
    if (hex.size() != 768 * 4) return false;
    size_t k = 0;
    for (auto& ch : out)
        for (auto& v : ch)
        {
            v = (WORD) wcstoul (hex.substr (k, 4).c_str(), nullptr, 16);
            k += 4;
        }
    return true;
}

bool saveVisionState (const VisionState& s)
{
    std::ostringstream o;
    o << "pid=" << s.pid << "\nengaged=" << (s.engaged ? 1 : 0) << "\ndisplays=" << s.displays.size() << "\n";
    for (size_t i = 0; i < s.displays.size(); ++i)
    {
        const auto& d = s.displays[i];
        o << "d" << i << ".target=" << d.targetId << "\n"
          << "d" << i << ".gdi=" << toUtf8 (d.gdiName) << "\n"
          << "d" << i << ".lut=" << (d.lutApplied ? 1 : 0) << "\n"
          << "d" << i << ".sharpChanged=" << (d.sharpnessChanged ? 1 : 0) << "\n"
          << "d" << i << ".sharpEnable=" << (d.sharpOrigEnable ? 1 : 0) << "\n"
          << "d" << i << ".sharpFilter=" << d.sharpOrigFilter << "\n"
          << "d" << i << ".sharpIntensity=" << d.sharpOrigIntensity << "\n"
          << "d" << i << ".gamma=" << (d.gammaChanged ? 1 : 0) << "\n"
          << "d" << i << ".gammaOrig=" << toUtf8 (d.gammaOrigHex) << "\n"
          << "d" << i << ".backlightChanged=" << (d.backlightChanged ? 1 : 0) << "\n"
          << "d" << i << ".backlightBase=" << d.backlightBase << "\n"
          << "d" << i << ".internal=" << (d.internalPanel ? 1 : 0) << "\n"
          << "d" << i << ".wmi=" << toUtf8 (d.wmiInstance) << "\n"
          << "d" << i << ".tv=" << (d.tvSignalChanged ? 1 : 0) << "\n";
    }
    return writeFileAtomically (visionStateFile(), o.str());
}

std::optional<VisionState> loadVisionState()
{
    auto text = readFile (visionStateFile());
    if (! text) return std::nullopt;

    VisionState s;
    std::istringstream in (*text);
    std::string line;
    auto display = [&] (size_t i) -> VisionDisplayState& {
        if (s.displays.size() <= i) s.displays.resize (i + 1);
        return s.displays[i];
    };
    while (std::getline (in, line))
    {
        const auto eq = line.find ('=');
        if (eq == std::string::npos) continue;
        const auto k = line.substr (0, eq);
        const auto v = line.substr (eq + 1);
        if (k == "pid") s.pid = (DWORD) std::strtoul (v.c_str(), nullptr, 10);
        else if (k == "engaged") s.engaged = v == "1";
        else if (k.size() > 2 && k[0] == 'd')
        {
            const auto dot = k.find ('.');
            if (dot == std::string::npos) continue;
            auto& d = display ((size_t) std::strtoul (k.substr (1, dot - 1).c_str(), nullptr, 10));
            const auto f = k.substr (dot + 1);
            if (f == "target") d.targetId = (UINT32) std::strtoul (v.c_str(), nullptr, 10);
            else if (f == "gdi") d.gdiName = fromUtf8 (v);
            else if (f == "lut") d.lutApplied = v == "1";
            else if (f == "sharpChanged") d.sharpnessChanged = v == "1";
            else if (f == "sharpEnable") d.sharpOrigEnable = v == "1";
            else if (f == "sharpFilter") d.sharpOrigFilter = (UINT32) std::strtoul (v.c_str(), nullptr, 10);
            else if (f == "sharpIntensity") d.sharpOrigIntensity = std::strtof (v.c_str(), nullptr);
            else if (f == "gamma") d.gammaChanged = v == "1";
            else if (f == "gammaOrig") d.gammaOrigHex = fromUtf8 (v);
            else if (f == "backlightChanged") d.backlightChanged = v == "1";
            else if (f == "backlightBase") d.backlightBase = (int) std::strtol (v.c_str(), nullptr, 10);
            else if (f == "internal") d.internalPanel = v == "1";
            else if (f == "wmi") d.wmiInstance = fromUtf8 (v);
            else if (f == "tv") d.tvSignalChanged = v == "1";
        }
    }
    return s;
}

void clearVisionState()
{
    DeleteFileW (visionStateFile().c_str());
}

std::string restoreVision (const VisionState& s)
{
    ComScope com;   // brightness goes through WMI (COM); the guard has no COM of its own
    IntelPipe intel;
    int luts = 0, sharp = 0, ramps = 0, lights = 0;
    for (auto& d : s.displays)
    {
        if (d.lutApplied && intel.restore3DLut (d.targetId)) ++luts;
        if (d.tvSignalChanged) intel.setTvSettings (d.targetId, 0, 0, 0);   // back to driver defaults
        if (d.sharpnessChanged && intel.setSharpness (d.targetId, { d.sharpOrigEnable, d.sharpOrigFilter, d.sharpOrigIntensity })) ++sharp;
        if (d.gammaChanged)
        {
            GammaRamp r = identityRamp();
            rampFromHex (d.gammaOrigHex, r);
            if (setGammaRamp (d.gdiName, r)) ++ramps;
        }
        if (d.backlightChanged && d.backlightBase >= 0
            && setBacklightRaw (d.internalPanel, d.wmiInstance, d.gdiName, d.backlightBase)) ++lights;
    }
    intel.close();
    char buf[160];
    std::snprintf (buf, sizeof (buf), "Display restored: %d colour table(s), %d sharpness setting(s), %d gamma ramp(s), %d backlight(s)", luts, sharp, ramps, lights);
    return buf;
}

} // namespace cinemax::win
