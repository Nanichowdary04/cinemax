// CINEMAX engine controller. Lives on the JUCE message thread and owns:
//  - Windows routing (default output -> virtual device, with crash-safe restore)
//  - device policy (auto-switch, Bluetooth reconnect, fallback, hands-free avoidance)
//  - per-device profiles and settings
//  - the real-time WASAPI pipeline
// The UI talks only to this class; it never touches audio or Windows APIs directly.
#pragma once

#include "DeviceMonitor.h"
#include "Metering.h"
#include "Params.h"
#include "Settings.h"
#include "../audio/VolumeMirror.h"
#include "../audio/WasapiPipeline.h"

#include "cinemax/win/Endpoints.h"

#include <juce_events/juce_events.h>

#include <deque>
#include <map>

namespace cinemax
{
enum class EngineStatus { Off, Starting, Running, NoVirtualDevice, NoOutputDevice, Error, Passthrough };
const char* statusKey (EngineStatus s);

struct DeviceView
{
    win::EndpointInfo info;
    bool isWindowsDefault = false;
    bool isOutput = false;
    bool isSource = false;
    bool hasProfile = false;
    bool headphoneLike = false;
};

enum class DetectedContent { Unknown, Music, Movie, Game };

/** What is on screen, from the Vision Engine's foreground-app detection. */
enum class SceneHint { None, Movie, Game, Work, Music };
const char* detectedContentKey (DetectedContent c);

/** An HDMI / optical output (TV, soundbar, AV receiver) for the TV screen. */
struct TvDeviceView
{
    win::EndpointInfo info;
    TvSetup setup;
    win::DolbySupport dolby;
    bool isOutput = false;       // CINEMAX is playing to it right now (or resting for it)
};

struct EarProfileView
{
    std::string key;     // "model" or a measured set key
    std::string name;
    std::string info;    // where the measurement comes from
};

struct EngineSnapshot
{
    DetectedContent detected = DetectedContent::Unknown;
    std::string detectedVia;   // "VLC", "Netflix", "YouTube Music" or "sound analysis"
    bool enabled = false;
    EngineStatus status = EngineStatus::Off;
    std::string message;
    bool bypass = false;
    std::optional<DeviceView> source, output;
    audio::PipelineFormat format;
    std::vector<DeviceView> devices;
    DspParams params;
    Settings settings;
    std::vector<EarProfileView> earProfiles;   // head model first, then the measured sets
    std::string activeEarProfile;              // resolved key in use ("model" or a set key)
    std::vector<TvDeviceView> tvDevices;       // HDMI / optical outputs
};

class Engine
{
public:
    Engine();
    ~Engine();

    /** Loads settings, recovers stale routing, starts device monitoring. */
    void initialise();

    /** Clean shutdown: stops audio and hands the Windows default output back. */
    void shutdown();

    //==============================================================================
    void setEnabled (bool shouldBeOn);
    bool isEnabled() const noexcept { return enabled; }
    void setBypass (bool shouldBypass);

    DspParams setParams (const juce::var& partial);
    DspParams setEqBand (int index, const juce::var& band);
    DspParams applyPreset (PresetId id);
    DspParams resetProfile();

    /** Music / Movies apply the configured preset; Auto follows the content analyser. */
    void setListeningMode (ListeningMode mode);

    /** Picture + sound scenes: the foreground app (game / video / work) as seen by Vision. */
    void setSceneHint (SceneHint hint, const std::string& label);

    void selectOutputDevice (const std::wstring& id);
    void selectSourceDevice (const std::wstring& id);

    Settings setSettings (const juce::var& partial);
    void resetSettings();
    void restoreDefaultRouting();

    void setVisualizerActive (bool active) noexcept { meterBus.analysisEnabled.store (active); }

    /** Ear-fit test: plays a short sound walk round the head through one ear profile
        ("model" or a set key). Needs the engine on. Returns false if the key is unknown. */
    bool startEarTest (const std::string& key);
    void stopEarTest();

    /** Written by the webcam head tracker (any thread). */
    HeadPose& headPoseState() noexcept { return headPose; }

    /** Head tracking is switched on and would be heard right now: engine running, surround on,
        headphone rendering through real ears. Otherwise 'why' says what is missing. */
    bool headTrackingUseful (std::string* why = nullptr) const;
    bool headTrackingFlip() const noexcept { return settings.headTrackingFlip; }

    /** TV setup for an HDMI / optical output (kept per device, apart from the presets). */
    TvSetup setTvSetup (const std::wstring& deviceId, const juce::var& partial);

    EngineSnapshot snapshot() const;
    bool snapshotIsRunning() const noexcept { return pipeline.isRunning(); }
    MeterBus& meters() noexcept { return meterBus; }
    const DspParams& currentParams() const noexcept { return params; }

    //==============================================================================
    std::function<void()> onStateChanged;                                   // message thread
    std::function<void (const char* level, const std::string&)> onNotice;   // message thread

private:
    struct CallbackTimer : juce::Timer
    {
        std::function<void()> fn;
        void timerCallback() override { stopTimer(); if (fn) fn(); }
    };

    struct RepeatingTimer : juce::Timer
    {
        std::function<void()> fn;
        void timerCallback() override { if (fn) fn(); }
    };

    void applyPresetInternal (PresetId id);
    PresetId presetForMode() const;
    void autoTick();

    void reconcile (const char* reason);
    void scheduleReconcile (int ms, const char* reason);
    void refreshEndpoints();
    std::optional<win::EndpointInfo> chooseSource() const;
    std::optional<win::EndpointInfo> chooseOutput() const;

    bool engageRouting (const win::EndpointInfo& source, const win::EndpointInfo& output);
    void disengageRouting();
    void writeRoutingState();

    void startPipeline (const win::EndpointInfo& source, const win::EndpointInfo& output);
    void stopPipeline();
    void onPipelineError (long hr, const std::string& message);
    void onDeviceEvent (const DeviceMonitor::Event& e);

    void switchProfileTo (const win::EndpointInfo& output);
    void loadHrtfSets();
    TvSetup tvFor (const win::EndpointInfo& e) const;
    void loadTvSetups();
    void saveTvSetups() const;
    int resolveEarProfile() const;           // HrtfLibrary index, -1 = head model
    void publishParams();
    ProcessingParams makeProcessingParams() const;
    void scheduleSave();
    void saveNow();
    void pushHistory (const std::wstring& id);

    void setStatus (EngineStatus s, std::string message);
    void notice (const char* level, const std::string& message);
    void changed();

    //==============================================================================
    ProfileStore store;
    Settings settings;
    DspParams params, customParams;
    bool hasCustom = false;
    bool enabled = false, bypass = false;

    EngineStatus status = EngineStatus::Off;
    std::string statusMessage;

    std::vector<win::EndpointInfo> endpoints;
    std::wstring defaultRenderId;
    std::wstring sourceId, outputId, outputName;
    win::DeviceKind outputKind = win::DeviceKind::Unknown;
    bool outputBuiltIn = false;
    std::wstring userOutputOverride;
    std::map<std::wstring, juce::uint32> failedOutputs;   // id -> retry-after (ms counter)

    bool routingEngaged = false;
    std::wstring lastSelfSetId;
    juce::uint32 lastSelfSetTime = 0;
    std::deque<juce::uint32> reassertTimes;

    int startFailures = 0;
    bool formatDirty = false;
    bool profileDirty = false;
    bool shutDown = false;

    TripleBuffer<ProcessingParams> paramBuffer;
    HeadPose headPose;
    std::map<std::wstring, TvSetup> tvSetups;              // by endpoint id (tv.json)
    std::map<std::wstring, win::DolbySupport> dolbyCache;  // probed once per device
    int auditionSet = -2;
    unsigned auditionId = 0;
    MeterBus meterBus;
    std::atomic<float> inputGain { 1.0f };
    std::atomic<float> deviceVolumeDb { 0.0f };   // the real output's volume, for the volume-aware tone
    audio::WasapiPipeline pipeline { paramBuffer, meterBus, inputGain, &deviceVolumeDb };
    audio::VolumeMirror volume { inputGain, &deviceVolumeDb };
    DeviceMonitor monitor;

    CallbackTimer reconcileTimer, saveTimer;
    RepeatingTimer autoTimer;
    DetectedContent detected = DetectedContent::Unknown;
    std::string detectedVia;
    int movieVotes = 0, musicVotes = 0;
    int autoTicks = 0;
    DetectedContent lastHint = DetectedContent::Unknown;
    std::string lastHintSource;
    SceneHint sceneHint = SceneHint::None;
    std::string sceneLabel;
    int hintStreak = 0;
    const char* pendingReason = "";
    std::shared_ptr<int> alive = std::make_shared<int> (0);
};

} // namespace cinemax
