#pragma once

#include "Params.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <string>
#include <vector>

namespace cinemax
{
/** What the user is listening to. Manual = they picked/edited a preset themselves. */
enum class ListeningMode { Manual, Music, Movies, Auto };
const char* listeningModeKey (ListeningMode m);
std::optional<ListeningMode> listeningModeFromKey (const std::string& key);

struct Settings
{
    ListeningMode listeningMode = ListeningMode::Auto;
    PresetId musicPreset = PresetId::ConcertHall;   // used by Music mode
    PresetId moviePreset = PresetId::Cinema;        // used by Movies mode
    PresetId gamePreset = PresetId::Gaming;         // Auto, while a game is in front (Vision scenes)
    bool smartTone = true;                          // automatic bass / treble per song or film

    // Headphone 3D sound ("real ears")
    std::string earProfile = "auto";    // "auto" = default measured set, "model" = head model, or a set key
    bool headTracking = false;          // webcam head tracking: the sound stays on the screen
    bool headTrackingFlip = false;      // camera delivers a mirrored picture: turn the other way

    bool launchOnStartup = false;
    bool minimizeToTray = true;
    bool startEngineOnLaunch = true;          // opening CINEMAX turns the sound on
    PresetId newDevicePreset = PresetId::Cinema;  // sound used for a device without a saved profile
    bool autoSwitchDevices = true;
    int bufferMs = 20;                       // 10 | 20 | 40 | 80
    bool compensateWindowsVolume = true;
    std::wstring preferredVirtualDeviceId;   // empty = automatic
    bool highQualityResampling = false;

    // Internal (not exposed in the UI contract)
    std::vector<std::wstring> outputHistory; // most recent first
    std::wstring lastOutputId;
};

/** Per-device profile file contents. */
struct DeviceProfile
{
    DspParams active;
    DspParams custom;
    bool hasCustom = false;
};

//==============================================================================
// JSON conversion (shapes match ui/src/bridge/types.ts)
juce::var toVar (const DspParams& p);
juce::var toVar (const EqBand& b);
DspParams mergeParams (const DspParams& base, const juce::var& partial, bool* presetWasGiven = nullptr);
EqBand mergeBand (const EqBand& base, const juce::var& partial);

juce::var toVar (const Settings& s, bool includeInternal);

juce::var toVar (const TvSetup& t);
TvSetup mergeTv (const TvSetup& base, const juce::var& partial);   // clamps every field
Settings mergeSettings (const Settings& base, const juce::var& partial, bool includeInternal);

juce::String toJuce (const std::wstring& w);
std::wstring toWide (const juce::String& s);

//==============================================================================
/** Stores settings.json and one JSON file per output device in %LOCALAPPDATA%\CINEMAX. */
class ProfileStore
{
public:
    Settings loadSettings() const;
    bool saveSettings (const Settings& s) const;

    std::optional<DeviceProfile> loadProfile (const std::wstring& deviceId) const;
    bool saveProfile (const std::wstring& deviceId, const std::wstring& deviceName, const DeviceProfile& profile) const;
    bool hasProfile (const std::wstring& deviceId) const;
    void removeProfile (const std::wstring& deviceId) const;

private:
    juce::File profileFile (const std::wstring& deviceId) const;
};

} // namespace cinemax
