#include "cinemax/win/Displays.h"

#include <dxgi1_6.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "wbemuuid.lib")

using Microsoft::WRL::ComPtr;

namespace cinemax::win
{
namespace
{
    std::wstring lower (std::wstring s)
    {
        for (auto& c : s) c = (wchar_t) std::towlower (c);
        return s;
    }

    bool isInternalTech (DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t)
    {
        return t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED
            || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
    }

    double triangleArea (double ax, double ay, double bx, double by, double cx, double cy)
    {
        return 0.5 * std::fabs (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    }

    /** "\\?\DISPLAY#CMN1522#4&3b88f39d&0&UID8388688#{guid}" -> "display\cmn1522\4&3b88f39d&0&uid8388688" */
    std::wstring devicePathToInstance (const std::wstring& path)
    {
        auto p = lower (path);
        const auto start = p.find (L"display#");
        if (start == std::wstring::npos) return {};
        auto end = p.find (L"#{", start);
        auto s = p.substr (start, end == std::wstring::npos ? std::wstring::npos : end - start);
        std::replace (s.begin(), s.end(), L'#', L'\\');
        return s;
    }

    //==========================================================================
    // Minimal WMI client for ROOT\WMI (panel backlight).
    class Wmi
    {
    public:
        Wmi()
        {
            ComPtr<IWbemLocator> loc;
            if (FAILED (CoCreateInstance (CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS (&loc))))
                return;
            BSTR ns = SysAllocString (L"ROOT\\WMI");
            const HRESULT hr = loc->ConnectServer (ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
            SysFreeString (ns);
            if (FAILED (hr)) { svc.Reset(); return; }
            CoSetProxyBlanket (svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                               RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        }

        bool ok() const { return svc != nullptr; }

        struct Panel { std::wstring instance; int brightness = -1; std::wstring path; };

        std::vector<Panel> panels()
        {
            std::vector<Panel> out;
            if (! svc) return out;
            ComPtr<IEnumWbemClassObject> en;
            BSTR lang = SysAllocString (L"WQL");
            BSTR q = SysAllocString (L"SELECT InstanceName, CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE");
            const HRESULT hr = svc->ExecQuery (lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
            SysFreeString (lang); SysFreeString (q);
            if (FAILED (hr)) return out;
            for (;;)
            {
                ComPtr<IWbemClassObject> obj;
                ULONG n = 0;
                if (en->Next (2000, 1, &obj, &n) != WBEM_S_NO_ERROR || n == 0) break;
                Panel p;
                VARIANT v; VariantInit (&v);
                if (SUCCEEDED (obj->Get (L"InstanceName", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR) p.instance = v.bstrVal;
                VariantClear (&v);
                if (SUCCEEDED (obj->Get (L"CurrentBrightness", 0, &v, nullptr, nullptr)))
                    p.brightness = v.vt == VT_UI1 ? v.bVal : (v.vt == VT_I4 ? v.lVal : -1);
                VariantClear (&v);
                out.push_back (p);
            }
            return out;
        }

        bool setBrightness (const std::wstring& instance, int percent)
        {
            if (! svc) return false;
            ComPtr<IEnumWbemClassObject> en;
            BSTR lang = SysAllocString (L"WQL");
            BSTR q = SysAllocString (L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE");
            HRESULT hr = svc->ExecQuery (lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
            SysFreeString (lang); SysFreeString (q);
            if (FAILED (hr)) return false;

            std::wstring path;
            for (;;)
            {
                ComPtr<IWbemClassObject> obj;
                ULONG n = 0;
                if (en->Next (2000, 1, &obj, &n) != WBEM_S_NO_ERROR || n == 0) break;
                VARIANT v; VariantInit (&v);
                std::wstring inst;
                if (SUCCEEDED (obj->Get (L"InstanceName", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR) inst = v.bstrVal;
                VariantClear (&v);
                if (! instance.empty() && lower (inst) != lower (instance)) continue;
                if (SUCCEEDED (obj->Get (L"__PATH", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR) path = v.bstrVal;
                VariantClear (&v);
                break;
            }
            if (path.empty()) return false;

            ComPtr<IWbemClassObject> cls, inSig, inParams, outParams;
            BSTR clsName = SysAllocString (L"WmiMonitorBrightnessMethods");
            hr = svc->GetObject (clsName, 0, nullptr, &cls, nullptr);
            SysFreeString (clsName);
            if (FAILED (hr) || FAILED (cls->GetMethod (L"WmiSetBrightness", 0, &inSig, nullptr)) || ! inSig)
                return false;
            if (FAILED (inSig->SpawnInstance (0, &inParams))) return false;

            VARIANT v; VariantInit (&v);
            v.vt = VT_I4; v.lVal = 1;
            inParams->Put (L"Timeout", 0, &v, 0);
            v.vt = VT_UI1; v.bVal = (BYTE) std::clamp (percent, 0, 100);
            inParams->Put (L"Brightness", 0, &v, 0);

            BSTR p = SysAllocString (path.c_str());
            BSTR m = SysAllocString (L"WmiSetBrightness");
            hr = svc->ExecMethod (p, m, 0, nullptr, inParams.Get(), &outParams, nullptr);
            SysFreeString (p); SysFreeString (m);
            return SUCCEEDED (hr);
        }

    private:
        ComPtr<IWbemServices> svc;
    };

    struct MonitorMatch { std::wstring gdi; HMONITOR found = nullptr; };

    BOOL CALLBACK findMonitor (HMONITOR hm, HDC, LPRECT, LPARAM lp)
    {
        auto* m = reinterpret_cast<MonitorMatch*> (lp);
        MONITORINFOEXW mi {};
        mi.cbSize = sizeof (mi);
        if (GetMonitorInfoW (hm, &mi) && _wcsicmp (mi.szDevice, m->gdi.c_str()) == 0)
        {
            m->found = hm;
            return FALSE;
        }
        return TRUE;
    }

    HMONITOR monitorFor (const std::wstring& gdi)
    {
        MonitorMatch m { gdi };
        EnumDisplayMonitors (nullptr, nullptr, findMonitor, (LPARAM) &m);
        return m.found;
    }

    /** DDC/CI: whether the monitor answers, and its brightness (0..100) if it does. */
    std::pair<bool, int> ddcState (const std::wstring& gdi)
    {
        HMONITOR hm = monitorFor (gdi);
        if (hm == nullptr) return { false, -1 };
        DWORD n = 0;
        if (! GetNumberOfPhysicalMonitorsFromHMONITOR (hm, &n) || n == 0) return { false, -1 };
        std::vector<PHYSICAL_MONITOR> pm (n);
        if (! GetPhysicalMonitorsFromHMONITOR (hm, n, pm.data())) return { false, -1 };
        bool ok = false;
        int pct = -1;
        DWORD caps = 0, temps = 0;
        if (GetMonitorCapabilities (pm[0].hPhysicalMonitor, &caps, &temps))
        {
            ok = true;
            DWORD mn = 0, cur = 0, mx = 0;
            if ((caps & MC_CAPS_BRIGHTNESS) && GetMonitorBrightness (pm[0].hPhysicalMonitor, &mn, &cur, &mx) && mx > mn)
                pct = (int) std::lround (100.0 * (cur - mn) / double (mx - mn));
        }
        DestroyPhysicalMonitors (n, pm.data());
        return { ok, pct };
    }

    bool ddcSetBrightness (const std::wstring& gdi, int percent)
    {
        HMONITOR hm = monitorFor (gdi);
        if (hm == nullptr) return false;
        DWORD n = 0;
        if (! GetNumberOfPhysicalMonitorsFromHMONITOR (hm, &n) || n == 0) return false;
        std::vector<PHYSICAL_MONITOR> pm (n);
        if (! GetPhysicalMonitorsFromHMONITOR (hm, n, pm.data())) return false;
        DWORD mn = 0, cur = 0, mx = 0;
        bool ok = false;
        if (GetMonitorBrightness (pm[0].hPhysicalMonitor, &mn, &cur, &mx) && mx > mn)
            ok = SetMonitorBrightness (pm[0].hPhysicalMonitor, mn + (DWORD) std::lround ((mx - mn) * std::clamp (percent, 0, 100) / 100.0)) != FALSE;
        DestroyPhysicalMonitors (n, pm.data());
        return ok;
    }

    bool queryPaths (std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            UINT32 np = 0, nm = 0;
            if (GetDisplayConfigBufferSizes (QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return false;
            paths.resize (np);
            modes.resize (nm);
            const LONG r = QueryDisplayConfig (QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
            if (r == ERROR_SUCCESS) { paths.resize (np); modes.resize (nm); return true; }
            if (r != ERROR_INSUFFICIENT_BUFFER) return false;
        }
        return false;
    }

    struct ColorState { bool hdrSupported = false, hdrEnabled = false, wcgSupported = false, wcgEnabled = false; int bpc = 8; };

    ColorState colorState (const DISPLAYCONFIG_PATH_INFO& p)
    {
        ColorState cs;
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ac {};
        ac.header = { DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof (ac), p.targetInfo.adapterId, p.targetInfo.id };
        if (DisplayConfigGetDeviceInfo (&ac.header) == ERROR_SUCCESS)
        {
            cs.hdrSupported = ac.advancedColorSupported && ! ac.wideColorEnforced;
            cs.hdrEnabled = ac.advancedColorEnabled && ! ac.wideColorEnforced;
            cs.wcgEnabled = ac.wideColorEnforced;
            cs.bpc = (int) ac.bitsPerColorChannel;
        }
#ifdef NTDDI_WIN11_GE
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 a2 {};
        a2.header = { DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2, sizeof (a2), p.targetInfo.adapterId, p.targetInfo.id };
        if (DisplayConfigGetDeviceInfo (&a2.header) == ERROR_SUCCESS)
        {
            cs.hdrSupported = a2.highDynamicRangeSupported;
            cs.hdrEnabled = a2.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
            cs.wcgSupported = a2.wideColorSupported;
            cs.wcgEnabled = a2.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG;
        }
#endif
        return cs;
    }
}

//==============================================================================
std::vector<DisplayInfo> listDisplays()
{
    std::vector<DisplayInfo> out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (! queryPaths (paths, modes))
        return out;

    int externalIndex = 0;
    for (auto& p : paths)
    {
        DisplayInfo d;
        d.adapterId = p.targetInfo.adapterId;
        d.targetId = p.targetInfo.id;
        d.sourceId = p.sourceInfo.id;

        DISPLAYCONFIG_TARGET_DEVICE_NAME tn {};
        tn.header = { DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof (tn), p.targetInfo.adapterId, p.targetInfo.id };
        DisplayConfigGetDeviceInfo (&tn.header);
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn {};
        sn.header = { DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof (sn), p.sourceInfo.adapterId, p.sourceInfo.id };
        DisplayConfigGetDeviceInfo (&sn.header);

        d.gdiName = sn.viewGdiDeviceName;
        d.internal = isInternalTech (tn.outputTechnology);
        d.hdmi = tn.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
        d.key = lower (tn.monitorDevicePath);
        if (d.key.empty())
            d.key = L"target-" + std::to_wstring (p.targetInfo.id);
        d.name = d.internal ? L"Built-in display"
               : (tn.monitorFriendlyDeviceName[0] != 0 ? std::wstring (tn.monitorFriendlyDeviceName) : L"Display " + std::to_wstring (++externalIndex));

        if (p.targetInfo.refreshRate.Denominator != 0)
            d.refreshHz = (double) p.targetInfo.refreshRate.Numerator / p.targetInfo.refreshRate.Denominator;
        if (p.sourceInfo.modeInfoIdx < modes.size() && modes[p.sourceInfo.modeInfoIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
        {
            d.width = (int) modes[p.sourceInfo.modeInfoIdx].sourceMode.width;
            d.height = (int) modes[p.sourceInfo.modeInfoIdx].sourceMode.height;
        }

        const auto cs = colorState (p);
        d.hdrSupported = cs.hdrSupported;
        d.hdrEnabled = cs.hdrEnabled;
        d.wideColorSupported = cs.wcgSupported;
        d.wideColorEnabled = cs.wcgEnabled;
        d.bitsPerColor = cs.bpc;

        DISPLAYCONFIG_SDR_WHITE_LEVEL wl {};
        wl.header = { DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof (wl), p.targetInfo.adapterId, p.targetInfo.id };
        if (DisplayConfigGetDeviceInfo (&wl.header) == ERROR_SUCCESS)
            d.sdrWhiteNits = wl.SDRWhiteLevel / 1000.0 * 80.0;

        out.push_back (std::move (d));
    }

    // DXGI: luminance, primaries, bit depth, adapter name.
    ComPtr<IDXGIFactory6> factory;
    if (SUCCEEDED (CreateDXGIFactory2 (0, IID_PPV_ARGS (&factory))))
    {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT ai = 0; factory->EnumAdapters1 (ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai)
        {
            DXGI_ADAPTER_DESC1 ad {};
            adapter->GetDesc1 (&ad);
            ComPtr<IDXGIOutput> o;
            for (UINT oi = 0; adapter->EnumOutputs (oi, &o) != DXGI_ERROR_NOT_FOUND; ++oi)
            {
                ComPtr<IDXGIOutput6> o6;
                if (FAILED (o.As (&o6))) continue;
                DXGI_OUTPUT_DESC1 desc {};
                if (FAILED (o6->GetDesc1 (&desc))) continue;
                for (auto& d : out)
                {
                    if (_wcsicmp (d.gdiName.c_str(), desc.DeviceName) != 0) continue;
                    d.adapterName = ad.Description;
                    if (desc.BitsPerColor > 0) d.bitsPerColor = (int) desc.BitsPerColor;
                    d.maxNits = desc.MaxLuminance;
                    d.minNits = desc.MinLuminance;
                    d.primaries = { desc.RedPrimary[0], desc.RedPrimary[1], desc.GreenPrimary[0], desc.GreenPrimary[1],
                                    desc.BluePrimary[0], desc.BluePrimary[1], desc.WhitePoint[0], desc.WhitePoint[1] };
                    const double srgb = triangleArea (0.64, 0.33, 0.30, 0.60, 0.15, 0.06);
                    const double area = triangleArea (desc.RedPrimary[0], desc.RedPrimary[1], desc.GreenPrimary[0], desc.GreenPrimary[1],
                                                      desc.BluePrimary[0], desc.BluePrimary[1]);
                    if (area > 0.0) d.gamutAreaPercentOfSrgb = 100.0 * area / srgb;
                }
            }
        }
    }

    // Backlight / brightness controls.
    Wmi wmi;
    const auto panels = wmi.ok() ? wmi.panels() : std::vector<Wmi::Panel> {};
    for (auto& d : out)
    {
        if (d.internal)
        {
            const auto inst = devicePathToInstance (d.key);
            for (auto& p : panels)
                if (! inst.empty() && lower (p.instance).rfind (inst, 0) == 0)
                {
                    d.wmiInstance = p.instance;
                    d.backlight = p.brightness;
                }
            if (d.wmiInstance.empty() && panels.size() == 1)   // single panel: the built-in one
            {
                d.wmiInstance = panels[0].instance;
                d.backlight = panels[0].brightness;
            }
        }
        else
        {
            const auto [ok, pct] = ddcState (d.gdiName);
            d.ddcci = ok;
            d.backlight = pct;
        }
    }
    return out;
}

std::string displayTopologyKey()
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (! queryPaths (paths, modes))
        return {};
    std::string key;
    for (auto& p : paths)
    {
        const auto cs = colorState (p);
        key += std::to_string (p.targetInfo.id) + ":" + std::to_string (p.targetInfo.refreshRate.Numerator) + ":"
             + std::to_string (cs.hdrEnabled) + std::to_string (cs.wcgEnabled) + ";";
    }
    for (auto& m : modes)
        if (m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
            key += std::to_string (m.sourceMode.width) + "x" + std::to_string (m.sourceMode.height) + ";";
    return key;
}

bool setHdr (const DisplayInfo& d, bool on)
{
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE st {};
    st.header = { DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE, sizeof (st), d.adapterId, d.targetId };
    st.enableAdvancedColor = on ? 1u : 0u;
    const LONG r = DisplayConfigSetDeviceInfo (&st.header);
    log::info ("HDR %s on %s -> %ld", on ? "on" : "off", toUtf8 (d.name).c_str(), r);
    return r == ERROR_SUCCESS;
}

int getBacklight (const DisplayInfo& d)
{
    if (d.internal)
    {
        Wmi wmi;
        if (! wmi.ok()) return -1;
        for (auto& p : wmi.panels())
            if (d.wmiInstance.empty() || lower (p.instance) == lower (d.wmiInstance))
                return p.brightness;
        return -1;
    }
    return ddcState (d.gdiName).second;
}

bool setBacklight (const DisplayInfo& d, int percent)
{
    if (! d.internal && ! d.ddcci) return false;
    return setBacklightRaw (d.internal, d.wmiInstance, d.gdiName, percent);
}

bool setBacklightRaw (bool internalPanel, const std::wstring& wmiInstance, const std::wstring& gdiName, int percent)
{
    if (internalPanel)
    {
        Wmi wmi;
        return wmi.ok() && wmi.setBrightness (wmiInstance, percent);
    }
    return ddcSetBrightness (gdiName, percent);
}

GammaRamp identityRamp()
{
    GammaRamp r {};
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i)
            r[(size_t) c][(size_t) i] = (WORD) (i * 257);
    return r;
}

bool getGammaRamp (const std::wstring& gdiName, GammaRamp& out)
{
    HDC dc = CreateDCW (L"DISPLAY", gdiName.c_str(), nullptr, nullptr);
    if (dc == nullptr) return false;
    const BOOL ok = GetDeviceGammaRamp (dc, out.data());
    DeleteDC (dc);
    return ok != FALSE;
}

bool setGammaRamp (const std::wstring& gdiName, const GammaRamp& ramp)
{
    HDC dc = CreateDCW (L"DISPLAY", gdiName.c_str(), nullptr, nullptr);
    if (dc == nullptr) return false;
    auto copy = ramp;
    const BOOL ok = SetDeviceGammaRamp (dc, copy.data());
    DeleteDC (dc);
    return ok != FALSE;
}

bool nightLightOn()
{
    HKEY key = nullptr;
    const wchar_t* path = L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\"
                          L"default$windows.data.bluelightreduction.bluelightreductionstate\\"
                          L"windows.data.bluelightreduction.bluelightreductionstate";
    if (RegOpenKeyExW (HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    BYTE data[64] {};
    DWORD size = sizeof (data), type = 0;
    const LONG r = RegQueryValueExW (key, L"Data", nullptr, &type, data, &size);
    RegCloseKey (key);
    // Byte 18 is 0x15 while Night Light is active and 0x13 while it is off.
    return r == ERROR_SUCCESS && size > 18 && data[18] == 0x15;
}

} // namespace cinemax::win
