// CINEMAX Vision Engine: Smart-TV-style picture processing for every display.
// Lives on the JUCE message thread next to the audio Engine and owns:
//  - display detection and per-monitor picture profiles (vision.json)
//  - the processing back-end per display: Intel display pipe (3D LUT + hardware
//    sharpness) or the Windows gamma ramp as a fallback
//  - Auto picture mode (foreground app -> scene -> preset, user rules win)
//  - crash-safe restore bookkeeping (vision-state.ini, read by CinemaxGuard)
// See docs/VISION-ENGINE.md for the architecture and its limits.
#pragma once

#include "Picture.h"

#include "cinemax/win/Displays.h"
#include "cinemax/win/ForegroundApp.h"
#include "cinemax/win/IntelPipe.h"
#include "cinemax/win/VisionState.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <map>

namespace cinemax::vision
{
enum class Scene { None, Movie, Game, Work, Music };
const char* sceneKey (Scene s);

struct AppRule
{
    std::wstring exe;          // lower-case image name, e.g. "vlc.exe"
    PicturePreset preset = PicturePreset::Cinema;
};

struct VisionSettings
{
    bool enabled = true;
    bool autoPicture = true;
    bool linkAudio = true;                  // scenes also switch the sound (when sound is on Auto)
    bool browserVideoAsCinema = true;       // browser + video site/film title -> CINEMA
    bool presetBacklight = true;            // picture modes move the backlight from the user's level (like a TV)
    PicturePreset defaultPreset = PicturePreset::Work;   // nothing specific on screen
    std::vector<AppRule> rules;
};

struct DisplayProfile
{
    bool manual = false;                    // user chose a preset / edited: Auto off for this display
    PicturePreset manualPreset = PicturePreset::Cinema;
    PictureParams custom;                   // CUSTOM values
    bool hasCustom = false;
    // TV over HDMI
    int tvRange = 0;                        // 0 automatic (driver), 1 full 0-255, 2 limited 16-235
    bool tvContentAuto = true;              // tell the TV what is on (film / game / desktop)
    bool tvGameMode = true;                 // ask the TV for its low-latency game mode while gaming
};

struct TvPictureView
{
    bool isTv = false;                      // external HDMI display
    bool available = false;                 // driver + TV accept HDMI signalling
    bool canRange = false, canContentType = false, canLowLatency = false;
    int sentRange = 0, sentContent = 0, sentLowLatency = 0;   // what was last sent (IntelTvSettings values)
};

enum class Backend { None, Intel, GammaRamp };
const char* backendKey (Backend b);

struct DisplayView
{
    win::DisplayInfo info;
    Backend backend = Backend::None;
    bool hasLut = false, hasSharpness = false;
    int lutSize = 0;
    DisplayProfile profile;
    PicturePreset autoPreset = PicturePreset::Work;   // last preset Auto chose for this display
    PicturePreset activePreset = PicturePreset::Work;
    PictureParams params;
    PictureAnalysis analysis;
    bool processing = false;           // a non-neutral transform is loaded right now
    bool limitedByWindows = false;     // gamma-ramp fallback had to be softened
    int baseBacklight = -1;            // the user's own backlight level (modes add their offset)
    int lastSetBacklight = -1;         // what CINEMAX last set (detects the user changing it)
    int lastOffset = 0;                // mode offset included in lastSetBacklight
    std::string status;
    TvPictureView tv;
};

struct VisionSnapshot
{
    VisionSettings settings;
    std::vector<DisplayView> displays;
    Scene scene = Scene::None;
    std::string sceneApp;              // "VS Code", "YouTube: movie"
    std::wstring foregroundExe;
    bool comparing = false;
    bool nightLight = false;
    bool intelAvailable = false;
};

class VisionEngine
{
public:
    VisionEngine();
    ~VisionEngine();

    void initialise();
    /** Restores every display to how it was and stops (clean exit / logoff). */
    void shutdown();

    VisionSnapshot snapshot() const;

    void setEnabled (bool on);
    void setSettings (const juce::var& partial);
    void selectPreset (const std::wstring& displayKey, PicturePreset p);
    void setAuto (const std::wstring& displayKey);
    void setParams (const std::wstring& displayKey, const juce::var& partial);
    bool setBacklight (const std::wstring& displayKey, int percent);
    void resetDisplay (const std::wstring& displayKey);
    void saveProfiles();
    void setCompare (bool on);
    void restoreOriginal();
    bool setHdr (const std::wstring& displayKey, bool on);
    void addRule (const std::wstring& exe, PicturePreset p);
    /** TV picture over HDMI: range ("auto" | "full" | "limited"), contentAuto, gameMode. */
    void setTv (const std::wstring& displayKey, const juce::var& partial);
    void removeRule (const std::wstring& exe);
    /** Display/power/session events: re-detect displays and re-load the hardware state. */
    void reapplyAll (const char* reason);
    /** Re-reads displays (backlight, HDR, Night Light) - called by the UI while visible. */
    void refresh();

    std::function<void()> onStateChanged;
    std::function<void (const char* level, const std::string&)> onNotice;
    std::function<void (Scene, const std::string& label)> onSceneChanged;

private:
    struct Timer : juce::Timer
    {
        std::function<void()> fn;
        void timerCallback() override { if (fn) fn(); }
    };

    void detectDisplays (bool force);
    DisplayView* find (const std::wstring& key);
    PicturePreset presetFor (const DisplayView& d) const;
    void apply (DisplayView& d, bool force);
    void applyAll (bool force);
    void unapply (DisplayView& d);
    void applyGammaFallback (DisplayView& d);
    void applyBacklight (DisplayView& d, bool usePresetOffset);
    void applyTv (DisplayView& d);
    win::VisionDisplayState& stateFor (const DisplayView& d);
    void autoTick();
    void topologyTick();
    void writeState();
    void load();
    void scheduleSave();
    void saveNow();
    void changed();

    VisionSettings settings;
    std::map<std::wstring, DisplayProfile> profiles;     // by display key (persisted)
    std::vector<DisplayView> displays;
    std::string topology;

    win::IntelPipe intel;
    bool intelOk = false;
    win::VisionState state;                                // what is changed on the hardware
    std::map<std::wstring, std::string> appliedSig;        // display key -> signature of what is loaded
    std::map<std::wstring, win::IntelSharpness> origSharp; // first-seen sharpness per display
    std::map<std::wstring, win::GammaRamp> origRamp;       // first-seen gamma ramp per display
    struct SavedBacklight { int base = -1, lastSet = -1; };
    std::map<std::wstring, SavedBacklight> savedBacklight;   // persisted: the user's level survives restarts

    Scene scene = Scene::None, pendingScene = Scene::None;
    std::string sceneApp;
    std::wstring sceneMonitor, pendingExe, foregroundExe;
    std::optional<PicturePreset> sceneRule;
    int sceneStreak = 0;
    bool comparing = false;
    bool nightLight = false;
    bool shutDown = false;

    Timer autoTimer, topoTimer, saveTimer;
};

/** JSON for the UI (shape matches ui/src/bridge/types.ts VisionState). */
juce::var toVar (const VisionSnapshot& s);

} // namespace cinemax::vision
