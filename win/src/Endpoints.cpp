#include "cinemax/win/Endpoints.h"

#include <initguid.h>   // define the PROPERTYKEY / GUID constants used below in this TU
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>

using Microsoft::WRL::ComPtr;

//==============================================================================
// IPolicyConfig - undocumented but stable since Windows 7; used by the Windows
// Sound control panel, SoundSwitch, EarTrumpet, AudioDeviceCmdlets etc.
// Only SetDefaultEndpoint is called; the preceding entries just keep the vtable
// layout correct.
struct DECLSPEC_UUID ("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat (PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat (PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat (PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat (PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod (PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod (PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode (PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode (PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue (PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue (PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint (PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility (PCWSTR, INT) = 0;
};

class DECLSPEC_UUID ("870af99c-171d-4f9e-af0d-e63df40c2bc9") CPolicyConfigClient;

namespace cinemax::win
{
//==============================================================================
const char* kindId (DeviceKind k)
{
    switch (k)
    {
        case DeviceKind::Speakers:            return "speakers";
        case DeviceKind::Headphones:          return "headphones";
        case DeviceKind::Headset:             return "headset";
        case DeviceKind::BluetoothHeadphones: return "bluetooth-headphones";
        case DeviceKind::BluetoothEarbuds:    return "bluetooth-earbuds";
        case DeviceKind::BluetoothSpeaker:    return "bluetooth-speaker";
        case DeviceKind::BluetoothHandsFree:  return "bluetooth-handsfree";
        case DeviceKind::Usb:                 return "usb";
        case DeviceKind::Hdmi:                return "hdmi";
        case DeviceKind::Digital:             return "digital";
        case DeviceKind::LineOut:             return "line-out";
        case DeviceKind::Virtual:             return "virtual";
        case DeviceKind::Unknown:             break;
    }
    return "unknown";
}

const char* kindLabel (DeviceKind k)
{
    switch (k)
    {
        case DeviceKind::Speakers:            return "Speakers";
        case DeviceKind::Headphones:          return "Wired headphones";
        case DeviceKind::Headset:             return "Wired headset";
        case DeviceKind::BluetoothHeadphones: return "Bluetooth headphones";
        case DeviceKind::BluetoothEarbuds:    return "Bluetooth earbuds";
        case DeviceKind::BluetoothSpeaker:    return "Bluetooth speaker";
        case DeviceKind::BluetoothHandsFree:  return "Bluetooth hands-free (call mode)";
        case DeviceKind::Usb:                 return "USB audio";
        case DeviceKind::Hdmi:                return "HDMI / display audio";
        case DeviceKind::Digital:             return "Digital output";
        case DeviceKind::LineOut:             return "Line out";
        case DeviceKind::Virtual:             return "Virtual audio device";
        case DeviceKind::Unknown:             break;
    }
    return "Audio output";
}

bool isHeadphoneLike (DeviceKind k)
{
    return k == DeviceKind::Headphones || k == DeviceKind::Headset
        || k == DeviceKind::BluetoothHeadphones || k == DeviceKind::BluetoothEarbuds
        || k == DeviceKind::BluetoothHandsFree;
}

bool looksLikeTv (const std::wstring& name)
{
    std::wstring n = L" ";
    for (wchar_t c : name)
        n += (wchar_t) std::towlower (c);
    n += L" ";
    for (auto& c : n)
        if (! std::iswalnum (c) && c != L'[' && c != L']') c = L' ';
    static const wchar_t* tokens[] {
        L"[tv]", L" tv ", L" television ", L" bravia ", L" webos ", L" smart tv", L" android tv", L" google tv",
        L" fire tv", L" roku ", L" tcl ", L" hisense ", L" mi tv", L" vu tv", L" qled ", L" oled tv", L" crystal uhd ",
    };
    for (auto* t : tokens)
        if (n.find (t) != std::wstring::npos)
            return true;
    return false;
}

bool isBluetooth (DeviceKind k)
{
    return k == DeviceKind::BluetoothHeadphones || k == DeviceKind::BluetoothEarbuds
        || k == DeviceKind::BluetoothSpeaker || k == DeviceKind::BluetoothHandsFree;
}

//==============================================================================
namespace
{
    std::wstring lower (std::wstring s)
    {
        std::transform (s.begin(), s.end(), s.begin(), [] (wchar_t c) { return (wchar_t) std::towlower (c); });
        return s;
    }

    bool containsAny (const std::wstring& haystackLower, std::initializer_list<const wchar_t*> needles)
    {
        for (auto* n : needles)
            if (haystackLower.find (n) != std::wstring::npos)
                return true;
        return false;
    }

    std::wstring readString (IPropertyStore* store, const PROPERTYKEY& key)
    {
        PROPVARIANT pv;
        PropVariantInit (&pv);
        std::wstring result;

        if (SUCCEEDED (store->GetValue (key, &pv)))
        {
            if (pv.vt == VT_LPWSTR && pv.pwszVal != nullptr)
                result = pv.pwszVal;
        }

        PropVariantClear (&pv);
        return result;
    }

    int readUInt (IPropertyStore* store, const PROPERTYKEY& key, int fallback)
    {
        PROPVARIANT pv;
        PropVariantInit (&pv);
        int result = fallback;

        if (SUCCEEDED (store->GetValue (key, &pv)))
        {
            if (pv.vt == VT_UI4)      result = (int) pv.ulVal;
            else if (pv.vt == VT_I4)  result = (int) pv.lVal;
        }

        PropVariantClear (&pv);
        return result;
    }

    void readDeviceFormat (IPropertyStore* store, int& sampleRate, int& channels)
    {
        PROPVARIANT pv;
        PropVariantInit (&pv);

        if (SUCCEEDED (store->GetValue (PKEY_AudioEngine_DeviceFormat, &pv)))
        {
            if (pv.vt == VT_BLOB && pv.blob.cbSize >= sizeof (WAVEFORMATEX) && pv.blob.pBlobData != nullptr)
            {
                auto* wf = reinterpret_cast<const WAVEFORMATEX*> (pv.blob.pBlobData);
                sampleRate = (int) wf->nSamplesPerSec;
                channels = (int) wf->nChannels;
            }
        }

        PropVariantClear (&pv);
    }

    DeviceKind classify (EndpointInfo& e)
    {
        const auto enumL = lower (e.enumerator);
        const auto nameL = lower (e.name + L" " + e.adapter);

        // Virtual / software devices: root-enumerated drivers (VB-CABLE, Voicemeeter,
        // DroidCam, Steam Streaming ...) or well-known virtual cable names.
        if (enumL == L"root" || enumL == L"swd\\virtual"
             || containsAny (nameL, { L"vb-audio", L"cable input", L"virtual cable", L"voicemeeter",
                                      L"virtual audio", L"steam streaming", L"vac " }))
        {
            e.isVirtual = true;
            return DeviceKind::Virtual;
        }

        if (enumL == L"bthhfenum" || containsAny (nameL, { L"hands-free", L"handsfree" }))
            return DeviceKind::BluetoothHandsFree;

        if (enumL == L"bthenum" || enumL == L"bthledevice" || enumL.find (L"bth") == 0)
        {
            if (e.formFactor == 1 /* Speakers */)
                return DeviceKind::BluetoothSpeaker;

            if (containsAny (nameL, { L"buds", L"tws", L"pods", L"earbud", L"earphone", L"in-ear", L"airbass", L"airdopes" }))
                return DeviceKind::BluetoothEarbuds;

            return DeviceKind::BluetoothHeadphones;
        }

        if (enumL == L"usb" || enumL == L"usbaudio" || enumL.find (L"usb") == 0)
            return DeviceKind::Usb;

        switch (e.formFactor)
        {
            case 1:  // Speakers
                e.isBuiltIn = (enumL == L"hdaudio" || enumL == L"intelaudio" || enumL == L"acpi"
                               || enumL == L"sst" || enumL == L"amdaudio");
                return DeviceKind::Speakers;
            case 2:  return DeviceKind::LineOut;
            case 3:  return DeviceKind::Headphones;
            case 5:  return DeviceKind::Headset;
            case 7:
            case 8:  return DeviceKind::Digital;
            case 9:  return DeviceKind::Hdmi;
            default: break;
        }

        return DeviceKind::Unknown;
    }

    HRESULT createEnumerator (ComPtr<IMMDeviceEnumerator>& out)
    {
        return CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&out));
    }
}

//==============================================================================
std::vector<EndpointInfo> enumerateRenderEndpoints (bool activeOnly)
{
    std::vector<EndpointInfo> result;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED (createEnumerator (enumerator)))
        return result;

    ComPtr<IMMDeviceCollection> collection;
    const DWORD mask = activeOnly ? DEVICE_STATE_ACTIVE
                                  : (DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED | DEVICE_STATE_DISABLED);
    if (FAILED (enumerator->EnumAudioEndpoints (eRender, mask, &collection)))
        return result;

    UINT count = 0;
    collection->GetCount (&count);

    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        if (FAILED (collection->Item (i, &device)))
            continue;

        EndpointInfo e;

        LPWSTR id = nullptr;
        if (SUCCEEDED (device->GetId (&id)) && id != nullptr)
        {
            e.id = id;
            CoTaskMemFree (id);
        }

        DWORD state = 0;
        device->GetState (&state);
        e.state = state;

        ComPtr<IPropertyStore> store;
        if (SUCCEEDED (device->OpenPropertyStore (STGM_READ, &store)))
        {
            e.name        = readString (store.Get(), PKEY_Device_FriendlyName);
            e.description = readString (store.Get(), PKEY_Device_DeviceDesc);
            e.adapter     = readString (store.Get(), PKEY_DeviceInterface_FriendlyName);
            e.enumerator  = readString (store.Get(), PKEY_Device_EnumeratorName);
            e.formFactor  = readUInt (store.Get(), PKEY_AudioEndpoint_FormFactor, 10);
            readDeviceFormat (store.Get(), e.sampleRate, e.channels);
        }

        if (e.name.empty())
            e.name = e.description.empty() ? L"Audio output" : e.description;

        e.kind = classify (e);
        result.push_back (std::move (e));
    }

    return result;
}

std::optional<EndpointInfo> findEndpoint (const std::vector<EndpointInfo>& list, const std::wstring& id)
{
    for (auto& e : list)
        if (_wcsicmp (e.id.c_str(), id.c_str()) == 0)
            return e;

    return std::nullopt;
}

std::wstring getDefaultRenderId (int role)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED (createEnumerator (enumerator)))
        return {};

    ComPtr<IMMDevice> device;
    if (FAILED (enumerator->GetDefaultAudioEndpoint (eRender, (ERole) role, &device)))
        return {};

    LPWSTR id = nullptr;
    std::wstring result;
    if (SUCCEEDED (device->GetId (&id)) && id != nullptr)
    {
        result = id;
        CoTaskMemFree (id);
    }
    return result;
}

bool isEndpointActive (const std::wstring& id)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (id.empty() || FAILED (createEnumerator (enumerator)))
        return false;

    ComPtr<IMMDevice> device;
    if (FAILED (enumerator->GetDevice (id.c_str(), &device)))
        return false;

    DWORD state = 0;
    return SUCCEEDED (device->GetState (&state)) && state == DEVICE_STATE_ACTIVE;
}

HRESULT setDefaultRenderEndpoint (const std::wstring& id, bool includeCommunications)
{
    ComPtr<IPolicyConfig> policy;
    HRESULT hr = CoCreateInstance (__uuidof (CPolicyConfigClient), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&policy));
    if (FAILED (hr))
        return hr;

    hr = policy->SetDefaultEndpoint (id.c_str(), eConsole);
    if (SUCCEEDED (hr))
        hr = policy->SetDefaultEndpoint (id.c_str(), eMultimedia);
    if (SUCCEEDED (hr) && includeCommunications)
        hr = policy->SetDefaultEndpoint (id.c_str(), eCommunications);

    return hr;
}

std::vector<EndpointInfo> virtualCandidates (const std::vector<EndpointInfo>& all)
{
    std::vector<EndpointInfo> result;
    for (auto& e : all)
        if (e.isVirtual && e.state == DEVICE_STATE_ACTIVE)
            result.push_back (e);

    auto score = [] (const EndpointInfo& e)
    {
        const auto n = lower (e.name + L" " + e.adapter);
        if (n.find (L"cable input") != std::wstring::npos || n.find (L"vb-audio virtual cable") != std::wstring::npos) return 0;
        if (n.find (L"cable") != std::wstring::npos)        return 1;
        if (n.find (L"voicemeeter") != std::wstring::npos)  return 2;
        if (n.find (L"virtual") != std::wstring::npos)      return 3;
        if (n.find (L"steam streaming") != std::wstring::npos) return 5;
        return 4;
    };

    std::stable_sort (result.begin(), result.end(),
                      [&] (const EndpointInfo& a, const EndpointInfo& b) { return score (a) < score (b); });
    return result;
}

bool isUsableOutput (const EndpointInfo& e, bool allowHandsFree)
{
    if (e.isVirtual || e.state != DEVICE_STATE_ACTIVE)
        return false;
    if (e.kind == DeviceKind::BluetoothHandsFree && ! allowHandsFree)
        return false;
    return true;
}

std::optional<EndpointInfo> pickPhysicalOutput (const std::vector<EndpointInfo>& all,
                                                const std::vector<std::wstring>& preferenceOrder)
{
    for (auto& id : preferenceOrder)
        if (auto e = findEndpoint (all, id))
            if (isUsableOutput (*e, false))
                return e;

    auto firstOf = [&] (auto predicate) -> std::optional<EndpointInfo>
    {
        for (auto& e : all)
            if (isUsableOutput (e, false) && predicate (e))
                return e;
        return std::nullopt;
    };

    if (auto e = firstOf ([] (const EndpointInfo& x) { return x.kind == DeviceKind::Speakers && x.isBuiltIn; })) return e;
    if (auto e = firstOf ([] (const EndpointInfo& x) { return x.kind == DeviceKind::Speakers; }))                 return e;
    if (auto e = firstOf ([] (const EndpointInfo& x) { return isHeadphoneLike (x.kind); }))                       return e;
    if (auto e = firstOf ([] (const EndpointInfo& x) { return x.kind == DeviceKind::Usb; }))                      return e;
    return firstOf ([] (const EndpointInfo&) { return true; });
}

} // namespace cinemax::win
