#include "cinemax/win/IntelPipe.h"

#include "igcl_api.h"   // third_party/igcl (Intel Software License, see LICENSE.txt there)

#include <algorithm>
#include <cmath>

namespace cinemax::win
{
namespace
{
    template <typename T> T sym (HMODULE m, const char* n) { return reinterpret_cast<T> (GetProcAddress (m, n)); }
    template <typename T> void prep (T& s) { ZeroMemory (&s, sizeof (s)); s.Size = sizeof (s); s.Version = 0; }
}

struct IntelPipe::Impl
{
    HMODULE lib = nullptr;
    bool tried = false;
    ctl_api_handle_t api = nullptr;

    ctl_pfnInit_t init = nullptr;
    ctl_pfnClose_t closeFn = nullptr;
    ctl_pfnEnumerateDevices_t enumDev = nullptr;
    ctl_pfnEnumerateDisplayOutputs_t enumOut = nullptr;
    ctl_pfnGetDisplayProperties_t props = nullptr;
    ctl_pfnPixelTransformationGetConfig_t getCfg = nullptr;
    ctl_pfnPixelTransformationSetConfig_t setCfg = nullptr;
    ctl_pfnGetSharpnessCaps_t sharpCaps = nullptr;
    ctl_pfnGetCurrentSharpness_t getSharp = nullptr;
    ctl_pfnSetCurrentSharpness_t setSharp = nullptr;
    ctl_pfnGetSetDisplaySettings_t dispSettings = nullptr;

    bool load()
    {
        if (tried) return api != nullptr;
        tried = true;
        lib = LoadLibraryExW (L"ControlLib.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (lib == nullptr) return false;
        init = sym<ctl_pfnInit_t> (lib, "ctlInit");
        closeFn = sym<ctl_pfnClose_t> (lib, "ctlClose");
        enumDev = sym<ctl_pfnEnumerateDevices_t> (lib, "ctlEnumerateDevices");
        enumOut = sym<ctl_pfnEnumerateDisplayOutputs_t> (lib, "ctlEnumerateDisplayOutputs");
        props = sym<ctl_pfnGetDisplayProperties_t> (lib, "ctlGetDisplayProperties");
        getCfg = sym<ctl_pfnPixelTransformationGetConfig_t> (lib, "ctlPixelTransformationGetConfig");
        setCfg = sym<ctl_pfnPixelTransformationSetConfig_t> (lib, "ctlPixelTransformationSetConfig");
        sharpCaps = sym<ctl_pfnGetSharpnessCaps_t> (lib, "ctlGetSharpnessCaps");
        getSharp = sym<ctl_pfnGetCurrentSharpness_t> (lib, "ctlGetCurrentSharpness");
        setSharp = sym<ctl_pfnSetCurrentSharpness_t> (lib, "ctlSetCurrentSharpness");
        dispSettings = sym<ctl_pfnGetSetDisplaySettings_t> (lib, "ctlGetSetDisplaySettings");
        if (! init || ! enumDev || ! enumOut || ! props || ! getCfg || ! setCfg)
        {
            log::warn ("ControlLib.dll present but incomplete - Intel picture path unavailable");
            return false;
        }
        ctl_init_args_t ia;
        prep (ia);
        ia.AppVersion = CTL_MAKE_VERSION (CTL_IMPL_MAJOR_VERSION, CTL_IMPL_MINOR_VERSION);
        const auto r = init (&ia, &api);
        if (r != CTL_RESULT_SUCCESS)
        {
            log::warn ("ctlInit failed (0x%x) - Intel picture path unavailable", (unsigned) r);
            api = nullptr;
            return false;
        }
        log::info ("Intel Graphics Control Library %u.%u ready", (unsigned) (ia.SupportedVersion >> 16), (unsigned) (ia.SupportedVersion & 0xffff));
        return true;
    }

    /** Active display output for a Windows target id (re-enumerated: hot-plug safe). */
    ctl_display_output_handle_t output (UINT32 targetId)
    {
        if (! load()) return nullptr;
        uint32_t nDev = 0;
        if (enumDev (api, &nDev, nullptr) != CTL_RESULT_SUCCESS || nDev == 0) return nullptr;
        std::vector<ctl_device_adapter_handle_t> devs (nDev);
        if (enumDev (api, &nDev, devs.data()) != CTL_RESULT_SUCCESS) return nullptr;
        for (auto dev : devs)
        {
            uint32_t nOut = 0;
            if (enumOut (dev, &nOut, nullptr) != CTL_RESULT_SUCCESS || nOut == 0) continue;
            std::vector<ctl_display_output_handle_t> outs (nOut);
            if (enumOut (dev, &nOut, outs.data()) != CTL_RESULT_SUCCESS) continue;
            for (auto o : outs)
            {
                ctl_display_properties_t dp;
                prep (dp);
                if (props (o, &dp) != CTL_RESULT_SUCCESS) continue;
                if ((dp.DisplayConfigFlags & CTL_DISPLAY_CONFIG_FLAG_DISPLAY_ACTIVE) == 0) continue;
                if (dp.Os_display_encoder_handle.WindowsDisplayEncoderID == targetId)
                    return o;
            }
        }
        return nullptr;
    }
};

IntelPipe::IntelPipe() : impl (std::make_unique<Impl>()) {}
IntelPipe::~IntelPipe() { close(); }

bool IntelPipe::available() { return impl->load(); }

std::optional<IntelOutputCaps> IntelPipe::caps (UINT32 targetId)
{
    auto o = impl->output (targetId);
    if (o == nullptr) return std::nullopt;

    IntelOutputCaps c;
    c.targetId = targetId;

    ctl_pixtx_pipe_get_config_t gc;
    prep (gc);
    gc.QueryType = CTL_PIXTX_CONFIG_QUERY_TYPE_CAPABILITY;
    prep (gc.InputPixelFormat); prep (gc.OutputPixelFormat);
    prep (gc.InputPixelFormat.ColorPrimaries); prep (gc.OutputPixelFormat.ColorPrimaries);
    if (impl->getCfg (o, &gc) == CTL_RESULT_SUCCESS && gc.NumBlocks > 0)
    {
        std::vector<ctl_pixtx_block_config_t> blocks (gc.NumBlocks);
        for (auto& b : blocks) { ZeroMemory (&b, sizeof (b)); b.Size = sizeof (b); }
        gc.pBlockConfigs = blocks.data();
        if (impl->getCfg (o, &gc) == CTL_RESULT_SUCCESS)
            for (auto& b : blocks)
                if (b.BlockType == CTL_PIXTX_BLOCK_TYPE_3D_LUT && ! c.has3DLut)
                {
                    c.has3DLut = true;
                    c.lutBlockId = b.BlockId;
                    c.lutSize = (int) b.Config.ThreeDLutConfig.NumSamplesPerChannel;
                }
    }

    if (impl->sharpCaps != nullptr)
    {
        ctl_sharpness_caps_t sc;
        prep (sc);
        if (impl->sharpCaps (o, &sc) == CTL_RESULT_SUCCESS && sc.NumFilterTypes > 0)
        {
            std::vector<ctl_sharpness_filter_properties_t> fp (sc.NumFilterTypes);
            sc.pFilterProperty = fp.data();
            if (impl->sharpCaps (o, &sc) == CTL_RESULT_SUCCESS)
            {
                c.hasSharpness = true;
                c.sharpFilter = fp[0].FilterType;
                c.sharpMin = fp[0].FilterDetails.min_possible_value;
                c.sharpMax = fp[0].FilterDetails.max_possible_value;
            }
        }
    }
    return c;
}

bool IntelPipe::set3DLut (UINT32 targetId, const std::vector<float>& rgb, int n)
{
    auto c = caps (targetId);
    auto o = impl->output (targetId);
    if (! c || ! c->has3DLut || o == nullptr || n != c->lutSize || rgb.size() != (size_t) n * n * n * 3)
        return false;

    std::vector<ctl_pixtx_3dlut_sample_t> samples ((size_t) n * n * n);
    for (size_t i = 0; i < samples.size(); ++i)
    {
        samples[i].Red = std::clamp ((double) rgb[i * 3 + 0], 0.0, 1.0);
        samples[i].Green = std::clamp ((double) rgb[i * 3 + 1], 0.0, 1.0);
        samples[i].Blue = std::clamp ((double) rgb[i * 3 + 2], 0.0, 1.0);
    }

    ctl_pixtx_block_config_t b;
    ZeroMemory (&b, sizeof (b));
    b.Size = sizeof (b);
    b.BlockId = c->lutBlockId;
    b.BlockType = CTL_PIXTX_BLOCK_TYPE_3D_LUT;
    prep (b.Config.ThreeDLutConfig);
    b.Config.ThreeDLutConfig.NumSamplesPerChannel = (uint32_t) n;
    b.Config.ThreeDLutConfig.pSampleValues = samples.data();

    ctl_pixtx_pipe_set_config_t sc;
    prep (sc);
    sc.OpertaionType = CTL_PIXTX_CONFIG_OPERTAION_TYPE_SET_CUSTOM;
    sc.Flags = 0;   // not persistent across power events: a crash can never leave it set after a reboot
    sc.NumBlocks = 1;
    sc.pBlockConfigs = &b;
    const auto r = impl->setCfg (o, &sc);
    if (r != CTL_RESULT_SUCCESS)
        log::warn ("Intel 3D LUT set failed on target %u: 0x%x", targetId, (unsigned) r);
    return r == CTL_RESULT_SUCCESS;
}

bool IntelPipe::restore3DLut (UINT32 targetId)
{
    auto c = caps (targetId);
    auto o = impl->output (targetId);
    if (! c || ! c->has3DLut || o == nullptr)
        return false;
    ctl_pixtx_block_config_t b;
    ZeroMemory (&b, sizeof (b));
    b.Size = sizeof (b);
    b.BlockId = c->lutBlockId;
    b.BlockType = CTL_PIXTX_BLOCK_TYPE_3D_LUT;
    ctl_pixtx_pipe_set_config_t sc;
    prep (sc);
    sc.OpertaionType = CTL_PIXTX_CONFIG_OPERTAION_TYPE_RESTORE_DEFAULT;
    sc.NumBlocks = 1;
    sc.pBlockConfigs = &b;
    return impl->setCfg (o, &sc) == CTL_RESULT_SUCCESS;
}

std::optional<IntelSharpness> IntelPipe::getSharpness (UINT32 targetId)
{
    auto o = impl->output (targetId);
    if (o == nullptr || impl->getSharp == nullptr) return std::nullopt;
    ctl_sharpness_settings_t s;
    prep (s);
    if (impl->getSharp (o, &s) != CTL_RESULT_SUCCESS) return std::nullopt;
    return IntelSharpness { s.Enable, s.FilterType, s.Intensity };
}

bool IntelPipe::setSharpness (UINT32 targetId, const IntelSharpness& v)
{
    auto o = impl->output (targetId);
    if (o == nullptr || impl->setSharp == nullptr) return false;
    ctl_sharpness_settings_t s;
    prep (s);
    s.Enable = v.enable;
    s.FilterType = v.filter;
    s.Intensity = v.intensity;
    return impl->setSharp (o, &s) == CTL_RESULT_SUCCESS;
}

std::optional<IntelTvSettings> IntelPipe::getTvSettings (UINT32 targetId)
{
    auto o = impl->output (targetId);
    if (o == nullptr || impl->dispSettings == nullptr) return std::nullopt;
    ctl_display_settings_t ds;
    prep (ds);
    ds.Set = false;
    if (impl->dispSettings (o, &ds) != CTL_RESULT_SUCCESS) return std::nullopt;
    auto can = [&] (uint32_t f) { return (ds.SupportedFlags & f) != 0 && (ds.ControllableFlags & f) != 0; };
    IntelTvSettings t;
    t.canRange = can (CTL_DISPLAY_SETTING_FLAG_QUANTIZATION_RANGE);
    t.canContentType = can (CTL_DISPLAY_SETTING_FLAG_CONTENT_TYPE);
    t.canLowLatency = can (CTL_DISPLAY_SETTING_FLAG_LOW_LATENCY);
    t.range = (int) ds.QuantizationRange;
    t.contentType = (int) ds.ContentType;
    t.lowLatency = (int) ds.LowLatency;
    return t;
}

bool IntelPipe::setTvSettings (UINT32 targetId, int range, int contentType, int lowLatency)
{
    auto caps = getTvSettings (targetId);
    auto o = impl->output (targetId);
    if (! caps || o == nullptr) return false;
    ctl_display_settings_t ds;
    prep (ds);
    ds.Set = true;
    ds.ValidFlags = 0;
    if (range >= 0 && caps->canRange)
    {
        ds.ValidFlags |= CTL_DISPLAY_SETTING_FLAG_QUANTIZATION_RANGE;
        ds.QuantizationRange = (ctl_display_setting_quantization_range_t) std::clamp (range, 0, 2);
    }
    if (contentType >= 0 && caps->canContentType)
    {
        ds.ValidFlags |= CTL_DISPLAY_SETTING_FLAG_CONTENT_TYPE;
        ds.ContentType = (ctl_display_setting_content_type_t) std::clamp (contentType, 0, 4);
    }
    if (lowLatency >= 0 && caps->canLowLatency)
    {
        ds.ValidFlags |= CTL_DISPLAY_SETTING_FLAG_LOW_LATENCY;
        ds.LowLatency = (ctl_display_setting_low_latency_t) std::clamp (lowLatency, 0, 2);
    }
    if (ds.ValidFlags == 0) return true;
    const auto r = impl->dispSettings (o, &ds);
    if (r != CTL_RESULT_SUCCESS)
        log::warn ("TV signalling for target %u failed (0x%x)", (unsigned) targetId, (unsigned) r);
    return r == CTL_RESULT_SUCCESS;
}

bool IntelPipe::setSharpnessLevel (UINT32 targetId, int level)
{
    auto c = caps (targetId);
    if (! c || ! c->hasSharpness) return false;
    IntelSharpness s;
    s.filter = c->sharpFilter;
    s.enable = level > 0;
    if (level > 0)
    {
        const float t = (std::clamp (level, 1, 5) - 1) / 4.0f;
        s.intensity = std::round (c->sharpMin + t * (c->sharpMax - c->sharpMin));
    }
    return setSharpness (targetId, s);
}

void IntelPipe::close()
{
    if (impl == nullptr) return;
    if (impl->api != nullptr && impl->closeFn != nullptr)
        impl->closeFn (impl->api);
    impl->api = nullptr;
    if (impl->lib != nullptr)
        FreeLibrary (impl->lib);
    impl->lib = nullptr;
    impl->tried = false;
}

} // namespace cinemax::win
