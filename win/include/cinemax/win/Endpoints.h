// Audio endpoint enumeration, classification and default-device control.
#pragma once

#include "Win32Common.h"

namespace cinemax::win
{
enum class DeviceKind
{
    Speakers,
    Headphones,
    Headset,
    BluetoothHeadphones,
    BluetoothEarbuds,
    BluetoothSpeaker,
    BluetoothHandsFree,
    Usb,
    Hdmi,
    Digital,
    LineOut,
    Virtual,
    Unknown
};

const char* kindId (DeviceKind k);     // "bluetooth-earbuds" (matches ui/src/bridge/types.ts)
const char* kindLabel (DeviceKind k);  // "Bluetooth earbuds"
bool isHeadphoneLike (DeviceKind k);
/** The device name looks like a TV ("[TV] Samsung 7 Series", "BRAVIA", "LG webOS TV", "Mi TV"...).
    Used to recognise TVs connected over Bluetooth, where Windows only says "speaker" or "headphones". */
bool looksLikeTv (const std::wstring& name);
bool isBluetooth (DeviceKind k);

struct EndpointInfo
{
    std::wstring id;           // IMMDevice id
    std::wstring name;         // "Headphones (TWS)"
    std::wstring description;  // "Headphones"
    std::wstring adapter;      // "TWS"
    std::wstring enumerator;   // "BTHENUM", "INTELAUDIO", "ROOT", "USB", ...
    int formFactor = 10;       // EndpointFormFactor
    DeviceKind kind = DeviceKind::Unknown;
    bool isVirtual = false;
    bool isBuiltIn = false;    // built-in laptop speakers (HD audio bus, speaker form factor)
    unsigned long state = 0;   // DEVICE_STATE_*
    int sampleRate = 0;        // shared-mode device format (from property store, no stream opened)
    int channels = 0;
};

/** Dolby bitstreams an HDMI / optical output can decode (from the TV's EDID via the driver). */
struct DolbySupport
{
    bool probed = false;
    bool dolbyDigital = false;       // AC-3 (5.1)
    bool dolbyDigitalPlus = false;   // E-AC-3 (streaming services)
    bool atmos = false;              // DD+ Atmos or TrueHD / MAT
};
DolbySupport probeDolby (const std::wstring& endpointId);

/** Enumerates render endpoints. COM must be initialised on the calling thread. */
std::vector<EndpointInfo> enumerateRenderEndpoints (bool activeOnly = true);

std::optional<EndpointInfo> findEndpoint (const std::vector<EndpointInfo>& list, const std::wstring& id);

/** Returns the current default render endpoint id for a role (0 = eConsole, 1 = eMultimedia, 2 = eCommunications). */
std::wstring getDefaultRenderId (int role);

bool isEndpointActive (const std::wstring& id);

/** Sets the Windows default playback device for the console + multimedia roles
    (and communications if requested) via the IPolicyConfig interface used by
    the Windows sound control panel. */
HRESULT setDefaultRenderEndpoint (const std::wstring& id, bool includeCommunications);

/** Virtual devices usable as the CINEMAX capture source, best candidate first. */
std::vector<EndpointInfo> virtualCandidates (const std::vector<EndpointInfo>& all);

/** Is this endpoint acceptable as a real output (not virtual)? Hands-free is allowed
    only when explicitly requested because it switches Bluetooth into narrow-band mode. */
bool isUsableOutput (const EndpointInfo& e, bool allowHandsFree);

/** Picks the best physical output: first active entry of preferenceOrder, then
    built-in speakers, then wired/USB/Bluetooth headphones, then anything usable. */
std::optional<EndpointInfo> pickPhysicalOutput (const std::vector<EndpointInfo>& all,
                                                const std::vector<std::wstring>& preferenceOrder);

} // namespace cinemax::win
