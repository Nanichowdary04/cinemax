#include "Engine.h"
#include "../dsp/Hrtf.h"

#include "cinemax/win/AudioSessions.h"
#include "cinemax/win/RoutingState.h"

#include <mmdeviceapi.h>

namespace cinemax
{
namespace
{
    bool sameId (const std::wstring& a, const std::wstring& b)
    {
        return ! a.empty() && ! b.empty() && _wcsicmp (a.c_str(), b.c_str()) == 0;
    }

    std::string utf8 (const std::wstring& w) { return win::toUtf8 (w); }

    juce::uint32 nowMs() { return juce::Time::getMillisecondCounter(); }
}

const char* statusKey (EngineStatus s)
{
    switch (s)
    {
        case EngineStatus::Off:             return "off";
        case EngineStatus::Starting:        return "starting";
        case EngineStatus::Running:         return "running";
        case EngineStatus::NoVirtualDevice: return "no-virtual-device";
        case EngineStatus::NoOutputDevice:  return "no-output-device";
        case EngineStatus::Error:           return "error";
        case EngineStatus::Passthrough:     return "passthrough";
    }
    return "error";
}

const char* detectedContentKey (DetectedContent c)
{
    switch (c)
    {
        case DetectedContent::Music:   return "music";
        case DetectedContent::Movie:   return "movie";
        case DetectedContent::Game:    return "game";
        case DetectedContent::Unknown: break;
    }
    return "unknown";
}

//==============================================================================
Engine::Engine()
{
    reconcileTimer.fn = [this] { reconcile (pendingReason); };
    saveTimer.fn = [this] { saveNow(); };
    autoTimer.fn = [this] { autoTick(); };

    std::weak_ptr<int> token = alive;
    pipeline.onStreamError = [this, token] (long hr, std::string message)
    {
        // Audio thread, after its loop has exited: hop to the message thread.
        juce::MessageManager::callAsync ([this, token, hr, message]
        {
            if (token.lock())
                onPipelineError (hr, message);
        });
    };

    params = makePreset (PresetId::Neutral);
    customParams = params;
}

Engine::~Engine()
{
    shutdown();
}

void Engine::initialise()
{
    loadHrtfSets();
    loadTvSetups();
    pipeline.setHeadPose (&headPose);
    settings = store.loadSettings();
    settings.launchOnStartup = win::isLaunchOnStartupEnabled();   // registry is the source of truth

    // Recover routing left behind by a crash / power loss of a previous session.
    if (auto st = win::loadRoutingState(); st && st->engaged)
    {
        const bool otherInstance = st->pid != GetCurrentProcessId() && win::isProcessAlive (st->pid, L"CINEMAX.exe");
        if (! otherInstance)
        {
            const auto r = win::restoreRouting (*st);
            win::log::warn ("Stale routing found at startup: %s", r.message.c_str());
            win::setLogonRecoveryEntry (false);
            if (r.changed)
                notice ("warning", "CINEMAX did not shut down cleanly last time. " + r.message);
        }
    }

    if (! monitor.start ([this] (const DeviceMonitor::Event& e) { onDeviceEvent (e); }))
        win::log::error ("Device monitoring could not be started");

    refreshEndpoints();

    if (auto out = chooseOutput())
        switchProfileTo (*out);

    autoTimer.startTimer (1000);

    if (settings.startEngineOnLaunch)
    {
        setEnabled (true);
    }
    else
    {
        setStatus (EngineStatus::Off, {});
        if (auto d = win::findEndpoint (endpoints, defaultRenderId); d && d->isVirtual)
            notice ("warning", "Windows is sending audio to \"" + utf8 (d->name)
                               + "\", a virtual device. Turn CINEMAX on, or use Settings > Restore default audio routing.");
        changed();
    }
}

void Engine::shutdown()
{
    if (shutDown)
        return;
    shutDown = true;

    reconcileTimer.stopTimer();
    saveTimer.stopTimer();
    autoTimer.stopTimer();
    monitor.stop();
    stopPipeline();
    disengageRouting();
    saveNow();
    alive.reset();
    win::log::info ("Engine shut down cleanly");
}

//==============================================================================
void Engine::setEnabled (bool shouldBeOn)
{
    enabled = shouldBeOn;
    startFailures = 0;
    failedOutputs.clear();
    reassertTimes.clear();
    reconcile (shouldBeOn ? "enabled" : "disabled");
}

void Engine::setBypass (bool shouldBypass)
{
    bypass = shouldBypass;
    publishParams();
    changed();
}

DspParams Engine::setParams (const juce::var& partial)
{
    bool presetGiven = false;
    auto p = mergeParams (params, partial, &presetGiven);
    if (! presetGiven)
        p.preset = PresetId::Custom;

    params = p;
    if (params.preset == PresetId::Custom)
    {
        customParams = params;
        hasCustom = true;
        if (settings.listeningMode != ListeningMode::Manual)
        {
            settings.listeningMode = ListeningMode::Manual;   // hand-tuned: stop automatic preset changes
            changed();
        }
    }

    profileDirty = true;
    publishParams();
    scheduleSave();
    return params;
}

DspParams Engine::setEqBand (int index, const juce::var& band)
{
    if (index < 0 || index >= kNumEqBands)
        return params;

    auto p = params;
    p.eqBands[(size_t) index] = mergeBand (p.eqBands[(size_t) index], band);
    p.preset = PresetId::Custom;
    clampParams (p);

    params = p;
    customParams = params;
    hasCustom = true;
    profileDirty = true;
    if (settings.listeningMode != ListeningMode::Manual)
    {
        settings.listeningMode = ListeningMode::Manual;
        changed();
    }
    publishParams();
    scheduleSave();
    return params;
}

void Engine::applyPresetInternal (PresetId id)
{
    if (id == PresetId::Custom)
    {
        if (hasCustom)
            params = customParams;
        params.preset = PresetId::Custom;
    }
    else
    {
        params = makePreset (id);
    }

    profileDirty = true;
    publishParams();
    scheduleSave();
}

DspParams Engine::applyPreset (PresetId id)
{
    // A preset picked by hand selects the matching mode, otherwise "manual".
    settings.listeningMode = id == settings.musicPreset ? ListeningMode::Music
                           : id == settings.moviePreset ? ListeningMode::Movies
                                                        : ListeningMode::Manual;
    applyPresetInternal (id);
    changed();
    return params;
}

PresetId Engine::presetForMode() const
{
    switch (settings.listeningMode)
    {
        case ListeningMode::Music:  return settings.musicPreset;
        case ListeningMode::Movies: return settings.moviePreset;
        case ListeningMode::Auto:
            if (detected == DetectedContent::Game) return settings.gamePreset;
            if (detected == DetectedContent::Movie) return settings.moviePreset;
            if (detected == DetectedContent::Music) return settings.musicPreset;
            // Nothing recognised yet: keep whichever mode preset is already playing
            // (switching to Auto during a film must not jump to the music preset).
            if (params.preset == settings.moviePreset || params.preset == settings.musicPreset)
                return params.preset;
            return settings.musicPreset;
        case ListeningMode::Manual: break;
    }
    return params.preset;
}

void Engine::setListeningMode (ListeningMode mode)
{
    settings.listeningMode = mode;
    if (mode != ListeningMode::Manual)
        applyPresetInternal (presetForMode());

    saveNow();
    changed();
}

void Engine::setSceneHint (SceneHint hint, const std::string& label)
{
    if (hint == sceneHint && label == sceneLabel) return;
    sceneHint = hint;
    sceneLabel = label;
    // React within a second instead of waiting for sound analysis.
    if (enabled && pipeline.isRunning())
        autoTick();
}

void Engine::autoTick()
{
    if (shutDown || ! enabled || ! pipeline.isRunning())
        return;

    ++autoTicks;
    const float p = meterBus.movieProbability.load();
    const float conf = meterBus.contentConfidence.load();

    DetectedContent next = detected;
    std::string via = detectedVia;

    // 1. Which app / site is playing (strong evidence), checked every second.
    const auto hint = win::detectAppHint (win::listRenderSessions (sourceId));
    const auto hintKind = hint.hint == win::ContentHint::Movie ? DetectedContent::Movie
                        : hint.hint == win::ContentHint::Music ? DetectedContent::Music
                                                               : DetectedContent::Unknown;

    if (sceneHint == SceneHint::Game)
    {
        // A game is in front: its sound is game sound, whatever it sounds like.
        hintStreak = 0;
        lastHint = DetectedContent::Unknown;
        movieVotes = musicVotes = 0;
        next = DetectedContent::Game;
        via = sceneLabel.empty() ? std::string ("game") : sceneLabel;
    }
    else if (hintKind != DetectedContent::Unknown)
    {
        hintStreak = (hintKind == lastHint && hint.source == lastHintSource) ? hintStreak + 1 : 1;
        lastHint = hintKind;
        lastHintSource = hint.source;
        movieVotes = musicVotes = 0;

        if (hintStreak >= 2)   // stable for 2 s (ignores a momentary notification sound)
        {
            next = hintKind;
            via = hint.source;
        }
    }
    else if (sceneHint == SceneHint::Movie || sceneHint == SceneHint::Music)
    {
        // No audio app recognised, but a video player / music app is in front.
        next = sceneHint == SceneHint::Movie ? DetectedContent::Movie : DetectedContent::Music;
        via = sceneLabel;
    }
    else
    {
        hintStreak = 0;
        lastHint = DetectedContent::Unknown;
        lastHintSource.clear();
        if (detected == DetectedContent::Game) { next = DetectedContent::Unknown; via.clear(); }

        // 2. No recognisable app: analyse the sound itself (slower, heuristic).
        if (conf >= 0.6f && meterBus.active.load())
        {
            // Films contain songs: once a film is detected, only a long stretch of clear
            // music (12 s) switches back. Music -> film needs 3 s.
            const bool inFilm = detected == DetectedContent::Movie;
            const float musicThreshold = inFilm ? 0.30f : 0.38f;
            const int musicNeeded = inFilm ? 12 : 3;

            if (p > 0.62f)                { ++movieVotes; musicVotes = 0; }
            else if (p < musicThreshold)  { ++musicVotes; movieVotes = 0; }
            else                          { movieVotes = 0; if (! inFilm) musicVotes = 0; }

            if (movieVotes >= 3)           { next = DetectedContent::Movie; via = "sound analysis"; }
            if (musicVotes >= musicNeeded) { next = DetectedContent::Music; via = "sound analysis"; }
        }
    }

    if (settings.listeningMode == ListeningMode::Auto && autoTicks % 10 == 0)
        win::log::info ("Auto: app hint=%s, p(movie)=%.2f conf=%.2f speech=%.2f beat=%.2f -> %s (%s)",
                        hint.source.empty() ? "none" : hint.source.c_str(), p, conf,
                        meterBus.analysisSpeech.load(), meterBus.analysisBeat.load(),
                        detectedContentKey (detected), detectedVia.c_str());

    if (next == detected)
    {
        if (via != detectedVia) { detectedVia = via; changed(); }
        return;
    }

    detected = next;
    detectedVia = via;
    win::log::info ("Content detected: %s via %s (p(movie)=%.2f)", detectedContentKey (detected), detectedVia.c_str(), p);

    if (settings.listeningMode == ListeningMode::Auto)
    {
        const auto id = presetForMode();
        if (id != params.preset)
        {
            applyPresetInternal (id);
            for (auto& info : presetCatalog())
                if (info.id == id)
                    notice ("info", std::string ("Auto: ") + (detected == DetectedContent::Game ? "game" : detected == DetectedContent::Movie ? "film or video" : "music")
                                    + " detected (" + detectedVia + ") - switched to " + info.name + ".");
        }
    }

    changed();
}

DspParams Engine::resetProfile()
{
    params = makePreset (settings.newDevicePreset);
    customParams = params;
    hasCustom = false;
    profileDirty = false;

    if (! outputId.empty())
        store.removeProfile (outputId);

    publishParams();
    notice ("info", "Profile for " + utf8 (outputName) + " reset to its default sound.");
    changed();
    return params;
}

void Engine::selectOutputDevice (const std::wstring& id)
{
    auto dev = win::findEndpoint (endpoints, id);
    if (! dev || dev->isVirtual)
    {
        notice ("warning", "That device can't be used as the CINEMAX output.");
        changed();
        return;
    }

    if (dev->kind == win::DeviceKind::BluetoothHandsFree)
        notice ("warning", "Hands-free mode is narrow-band call audio. For music and movies use the \"Headphones\" entry of the same device.");

    userOutputOverride = id;
    failedOutputs.erase (id);
    pushHistory (id);
    reconcile ("user-selected-output");
}

void Engine::selectSourceDevice (const std::wstring& id)
{
    settings.preferredVirtualDeviceId = id;
    saveNow();
    reconcile ("user-selected-source");
}

Settings Engine::setSettings (const juce::var& partial)
{
    const auto old = settings;
    settings = mergeSettings (settings, partial, false);

    if (settings.launchOnStartup != old.launchOnStartup)
    {
        if (! win::setLaunchOnStartup (settings.launchOnStartup, win::paths::exePath()))
        {
            settings.launchOnStartup = old.launchOnStartup;
            notice ("error", "Couldn't update the Windows startup entry.");
        }
    }

    if (settings.compensateWindowsVolume != old.compensateWindowsVolume)
        volume.setCompensate (settings.compensateWindowsVolume);

    if (settings.smartTone != old.smartTone || settings.earProfile != old.earProfile)
        publishParams();

    // A different preset for the active mode takes effect immediately.
    if ((settings.musicPreset != old.musicPreset || settings.moviePreset != old.moviePreset
         || settings.listeningMode != old.listeningMode)
        && settings.listeningMode != ListeningMode::Manual)
        applyPresetInternal (presetForMode());

    const bool restart = settings.bufferMs != old.bufferMs
                      || settings.highQualityResampling != old.highQualityResampling
                      || settings.preferredVirtualDeviceId != old.preferredVirtualDeviceId;

    saveNow();

    if (restart && enabled)
        reconcile ("settings-changed");
    else
        changed();

    return settings;
}

void Engine::resetSettings()
{
    Settings fresh;
    fresh.outputHistory = settings.outputHistory;
    fresh.lastOutputId = settings.lastOutputId;

    if (settings.launchOnStartup)
        win::setLaunchOnStartup (false, win::paths::exePath());

    settings = fresh;
    volume.setCompensate (settings.compensateWindowsVolume);
    saveNow();
    notice ("info", "Settings reset to defaults.");
    reconcile ("settings-reset");
}

void Engine::restoreDefaultRouting()
{
    enabled = false;
    stopPipeline();
    disengageRouting();

    // Also repair a virtual default that CINEMAX did not set (e.g. set manually).
    refreshEndpoints();
    if (auto d = win::findEndpoint (endpoints, defaultRenderId); d && d->isVirtual)
    {
        win::RoutingState st;
        st.engaged = true;
        st.pid = GetCurrentProcessId();
        st.sourceId = d->id;
        st.restoreToId = outputId.empty() ? settings.lastOutputId : outputId;
        const auto r = win::restoreRouting (st);
        notice (r.ok ? "info" : "error", r.message);
    }
    else
    {
        notice ("info", "Windows audio routing is back to normal.");
    }

    reconcile ("restore-routing");
}

//==============================================================================
void Engine::scheduleReconcile (int ms, const char* reason)
{
    pendingReason = reason;
    reconcileTimer.startTimer (ms);
}

void Engine::refreshEndpoints()
{
    endpoints = win::enumerateRenderEndpoints (true);
    defaultRenderId = win::getDefaultRenderId (1 /* eMultimedia */);

    // What a TV / soundbar / receiver can decode comes from its EDID: ask once per device.
    for (auto& e : endpoints)
        if ((e.kind == win::DeviceKind::Hdmi || e.kind == win::DeviceKind::Digital) && dolbyCache.find (e.id) == dolbyCache.end())
        {
            const auto d = win::probeDolby (e.id);
            dolbyCache[e.id] = d;
            win::log::info ("%s: Dolby Digital %s, Dolby Digital Plus %s, Atmos %s", utf8 (e.name).c_str(),
                            d.dolbyDigital ? "yes" : "no", d.dolbyDigitalPlus ? "yes" : "no", d.atmos ? "yes" : "no");
        }
}

//==============================================================================
// TV outputs
//==============================================================================
TvSetup Engine::tvFor (const win::EndpointInfo& e) const
{
    if (auto it = tvSetups.find (e.id); it != tvSetups.end())
        return it->second;
    TvSetup t;
    t.isTv = e.kind == win::DeviceKind::Hdmi || win::looksLikeTv (e.name);   // HDMI audio, or a Bluetooth TV by its name
    t.speakers = e.kind == win::DeviceKind::Digital ? 3 : 1;     // optical usually feeds a receiver / soundbar
    return t;
}

void Engine::loadTvSetups()
{
    const juce::File f (toJuce (win::paths::dataDir() + L"\\tv.json"));
    if (! f.existsAsFile())
        return;
    const auto root = juce::JSON::parse (f.loadFileAsString());
    if (auto* d = root.getProperty ("devices", juce::var()).getDynamicObject())
        for (auto& prop : d->getProperties())
            tvSetups[toWide (prop.name.toString())] = mergeTv (TvSetup {}, prop.value);
}

void Engine::saveTvSetups() const
{
    auto* devices = new juce::DynamicObject();
    for (auto& [id, t] : tvSetups)
        devices->setProperty (juce::Identifier (toJuce (id)), toVar (t));
    auto* root = new juce::DynamicObject();
    root->setProperty ("devices", juce::var (devices));
    win::writeFileAtomically (win::paths::dataDir() + L"\\tv.json", juce::JSON::toString (juce::var (root)).toStdString());
}

TvSetup Engine::setTvSetup (const std::wstring& deviceId, const juce::var& partial)
{
    auto e = win::findEndpoint (endpoints, deviceId);
    TvSetup base;
    if (e) base = tvFor (*e);
    else if (auto it = tvSetups.find (deviceId); it != tvSetups.end()) base = it->second;
    const TvSetup t = mergeTv (base, partial);
    const bool passthroughChanged = t.letTvDecode != base.letTvDecode || t.isTv != base.isTv;
    tvSetups[deviceId] = t;
    saveTvSetups();

    if (sameId (deviceId, outputId))
    {
        publishParams();
        if (passthroughChanged && enabled)
            scheduleReconcile (50, "tv-setup");
    }
    changed();
    return t;
}

std::optional<win::EndpointInfo> Engine::chooseSource() const
{
    if (! settings.preferredVirtualDeviceId.empty())
        if (auto e = win::findEndpoint (endpoints, settings.preferredVirtualDeviceId); e && e->isVirtual)
            return e;

    auto candidates = win::virtualCandidates (endpoints);
    if (! candidates.empty())
        return candidates.front();
    return std::nullopt;
}

std::optional<win::EndpointInfo> Engine::chooseOutput() const
{
    const auto now = nowMs();
    auto usable = [&] (const win::EndpointInfo& e, bool allowHandsFree)
    {
        if (! win::isUsableOutput (e, allowHandsFree))
            return false;
        const auto it = failedOutputs.find (e.id);
        return it == failedOutputs.end() || now >= it->second;
    };

    // 1. The device the user picked (hands-free only when explicitly chosen).
    if (! userOutputOverride.empty())
        if (auto e = win::findEndpoint (endpoints, userOutputOverride); e && usable (*e, true))
            return e;

    // 2. Without auto-switching, stay on the current device while it exists.
    if (! settings.autoSwitchDevices && ! outputId.empty())
        if (auto e = win::findEndpoint (endpoints, outputId); e && usable (*e, false))
            return e;

    // 3. Most recently used devices first (handles Bluetooth reconnects).
    for (auto& id : settings.outputHistory)
        if (auto e = win::findEndpoint (endpoints, id); e && usable (*e, false))
            return e;

    // 4. The Windows default, if it is a real device.
    if (auto e = win::findEndpoint (endpoints, defaultRenderId); e && usable (*e, false))
        return e;

    // 5. Best physical device.
    std::vector<win::EndpointInfo> filtered;
    for (auto& e : endpoints)
        if (usable (e, false))
            filtered.push_back (e);
    return win::pickPhysicalOutput (filtered, {});
}

//==============================================================================
void Engine::reconcile (const char* reason)
{
    reconcileTimer.stopTimer();
    if (shutDown)
        return;

    win::log::info ("Reconcile (%s)", reason);
    refreshEndpoints();

    if (! enabled)
    {
        stopPipeline();
        disengageRouting();
        setStatus (EngineStatus::Off, {});
        if (auto out = chooseOutput(); out && ! sameId (out->id, outputId))
            switchProfileTo (*out);
        changed();
        return;
    }

    auto src = chooseSource();
    if (! src)
    {
        stopPipeline();
        disengageRouting();
        setStatus (EngineStatus::NoVirtualDevice,
                   "No virtual audio device found. Install VB-CABLE (vb-audio.com/Cable), restart Windows if asked, then turn CINEMAX on.");
        changed();
        return;
    }

    auto out = chooseOutput();
    if (! out)
    {
        stopPipeline();
        disengageRouting();
        setStatus (EngineStatus::NoOutputDevice, "No playback device is available. Connect speakers or headphones.");
        changed();
        return;
    }

    if (! sameId (out->id, outputId))
        switchProfileTo (*out);

    // A Dolby-capable TV / receiver the user wants to decode the sound itself: step aside so
    // apps can send it the original Dolby bitstream. CINEMAX resumes when the output changes.
    if (const auto tv = tvFor (*out); tv.isTv && tv.letTvDecode
        && (out->kind == win::DeviceKind::Hdmi || out->kind == win::DeviceKind::Digital))
    {
        stopPipeline();
        disengageRouting();
        setStatus (EngineStatus::Passthrough, "\"" + utf8 (out->name) + "\" decodes the sound itself (Dolby passthrough). "
                   "CINEMAX is resting while it is the output.");
        changed();
        return;
    }

    if (! engageRouting (*src, *out))
    {
        stopPipeline();
        setStatus (EngineStatus::Error, "Couldn't make \"" + utf8 (src->name) + "\" the Windows default output.");
        changed();
        return;
    }

    const auto cfg = pipeline.config();
    const bool needsRestart = ! pipeline.isRunning() || formatDirty
                           || ! sameId (cfg.sourceId, src->id) || ! sameId (cfg.outputId, out->id)
                           || cfg.bufferMs != settings.bufferMs
                           || cfg.highQualityResampling != settings.highQualityResampling;

    if (needsRestart)
        startPipeline (*src, *out);
    else
        setStatus (EngineStatus::Running, {});

    changed();
}

bool Engine::engageRouting (const win::EndpointInfo& source, const win::EndpointInfo& output)
{
    juce::ignoreUnused (output);
    const bool firstEngage = ! routingEngaged;

    sourceId = source.id;
    routingEngaged = true;
    writeRoutingState();               // record BEFORE changing anything (crash safety)
    if (firstEngage)
        win::setLogonRecoveryEntry (true);

    const auto consoleId = win::getDefaultRenderId (0);
    const auto mediaId = win::getDefaultRenderId (1);

    if (! sameId (consoleId, source.id) || ! sameId (mediaId, source.id))
    {
        lastSelfSetId = source.id;
        lastSelfSetTime = nowMs();

        const HRESULT hr = win::setDefaultRenderEndpoint (source.id, false);
        if (FAILED (hr))
        {
            win::log::error ("SetDefaultEndpoint failed: %s", win::hresultToString (hr).c_str());
            if (firstEngage)
            {
                routingEngaged = false;
                writeRoutingState();
                win::setLogonRecoveryEntry (false);
            }
            return false;
        }

        defaultRenderId = source.id;
        win::log::info ("Windows default output -> %s", utf8 (source.name).c_str());
    }

    return true;
}

void Engine::disengageRouting()
{
    if (! routingEngaged)
        return;

    volume.stop();

    win::RoutingState st;
    st.engaged = true;
    st.pid = GetCurrentProcessId();
    st.sourceId = sourceId;
    st.restoreToId = outputId.empty() ? settings.lastOutputId : outputId;

    lastSelfSetId = st.restoreToId;
    lastSelfSetTime = nowMs();

    const auto r = win::restoreRouting (st);
    win::log::info ("Routing restored: %s", r.message.c_str());
    if (! r.ok)
        notice ("error", r.message + " Open Windows sound settings to pick an output.");

    routingEngaged = false;
    writeRoutingState();
    win::setLogonRecoveryEntry (false);
}

void Engine::writeRoutingState()
{
    win::RoutingState st;
    st.engaged = routingEngaged;
    st.pid = GetCurrentProcessId();
    st.sourceId = sourceId;
    st.restoreToId = outputId.empty() ? settings.lastOutputId : outputId;
    win::saveRoutingState (st);
}

//==============================================================================
void Engine::startPipeline (const win::EndpointInfo& source, const win::EndpointInfo& output)
{
    stopPipeline();
    formatDirty = false;

    setStatus (EngineStatus::Starting, "Starting audio engine...");

    const auto pp = makeProcessingParams();
    paramBuffer.write (pp);

    audio::PipelineConfig cfg;
    cfg.sourceId = source.id;
    cfg.outputId = output.id;
    cfg.bufferMs = settings.bufferMs;
    cfg.highQualityResampling = settings.highQualityResampling;

    const auto r = pipeline.start (cfg, pp);

    if (r.ok)
    {
        startFailures = 0;
        volume.start (source.id, output.id, settings.compensateWindowsVolume);

        const auto f = pipeline.format();
        win::log::info ("Pipeline running: %s (%.0f Hz, %d ch) -> %s (%.0f Hz, %d ch), buffer %.1f ms, latency ~%.1f ms",
                        utf8 (source.name).c_str(), f.inputRate, f.inputChannels,
                        utf8 (output.name).c_str(), f.outputRate, f.outputChannels, f.bufferMs, f.latencyMs);
        setStatus (EngineStatus::Running, {});
        return;
    }

    ++startFailures;
    win::log::error ("Pipeline start failed (%d): %s", startFailures, r.message.c_str());

    if (startFailures <= 3)
    {
        setStatus (EngineStatus::Starting, "Couldn't open " + utf8 (output.name) + " yet (" + r.message + "). Retrying...");
        scheduleReconcile (800 * startFailures, "start-retry");
    }
    else
    {
        // Give this device a rest and fall back to another one.
        failedOutputs[output.id] = nowMs() + 30000;
        if (sameId (userOutputOverride, output.id))
            userOutputOverride.clear();
        startFailures = 0;
        setStatus (EngineStatus::Error, r.message);
        notice ("warning", "Couldn't play through " + utf8 (output.name) + ". Trying another device.");
        scheduleReconcile (200, "fallback");
    }
}

void Engine::stopPipeline()
{
    volume.stop();
    pipeline.stop();
}

void Engine::onPipelineError (long hr, const std::string& message)
{
    juce::ignoreUnused (hr);
    if (! enabled || shutDown)
        return;

    win::log::warn ("Pipeline error: %s", message.c_str());
    formatDirty = true;
    setStatus (EngineStatus::Starting, "Audio device changed - reconnecting...");
    changed();
    scheduleReconcile (400, "stream-error");
}

void Engine::onDeviceEvent (const DeviceMonitor::Event& e)
{
    if (shutDown)
        return;

    using K = DeviceMonitor::Kind;
    const auto now = nowMs();

    switch (e.kind)
    {
        case K::DefaultChanged:
        {
            if (e.role == 2 /* eCommunications */)
                break;

            if (! routingEngaged)
            {
                scheduleReconcile (300, "default-changed");
                break;
            }

            if (sameId (e.deviceId, sourceId))
                break;   // our own change

            if (sameId (e.deviceId, lastSelfSetId) && now - lastSelfSetTime < 2500)
                break;

            // The user (Windows flyout) or Windows itself (device reconnect) picked a real
            // device: make it the CINEMAX output and keep routing through CINEMAX.
            refreshEndpoints();
            if (auto dev = win::findEndpoint (endpoints, e.deviceId); dev && ! dev->isVirtual)
            {
                reassertTimes.push_back (now);
                while (! reassertTimes.empty() && now - reassertTimes.front() > 10000)
                    reassertTimes.pop_front();

                if (reassertTimes.size() > 6)
                {
                    notice ("warning", "Another app keeps changing the default playback device, so CINEMAX turned off to avoid a tug-of-war.");
                    setEnabled (false);
                    return;
                }

                if (win::isUsableOutput (*dev, true))
                {
                    userOutputOverride = dev->id;
                    pushHistory (dev->id);
                }
            }
            scheduleReconcile (250, "default-changed");
            break;
        }

        case K::Added:
        case K::StateChanged:
        {
            if (e.kind == K::StateChanged && e.state != DEVICE_STATE_ACTIVE)
            {
                scheduleReconcile (300, "device-inactive");
                break;
            }

            refreshEndpoints();
            if (settings.autoSwitchDevices)
            {
                if (auto dev = win::findEndpoint (endpoints, e.deviceId); dev && win::isUsableOutput (*dev, false))
                {
                    const bool preferable = win::isHeadphoneLike (dev->kind) || win::isBluetooth (dev->kind)
                                         || dev->kind == win::DeviceKind::Usb;
                    if (preferable && ! sameId (dev->id, outputId))
                    {
                        userOutputOverride = dev->id;
                        pushHistory (dev->id);
                        failedOutputs.erase (dev->id);
                    }
                }
            }
            // Bluetooth endpoints need a moment to settle after connecting.
            scheduleReconcile (600, "device-added");
            break;
        }

        case K::Removed:
            scheduleReconcile (300, "device-removed");
            break;

        case K::FormatChanged:
            if (sameId (e.deviceId, sourceId) || sameId (e.deviceId, outputId))
            {
                formatDirty = true;
                scheduleReconcile (300, "format-changed");
            }
            break;
    }
}

//==============================================================================
void Engine::switchProfileTo (const win::EndpointInfo& output)
{
    const bool hadOutput = ! outputId.empty();
    const std::wstring oldName = outputName;
    const bool oldGone = hadOutput && ! win::findEndpoint (endpoints, outputId).has_value();

    if (hadOutput)
        saveNow();   // persist edits made for the previous device

    bool loaded = false;
    if (auto prof = store.loadProfile (output.id))
    {
        // An untouched factory preset is stored by value; reload it from the current
        // definition so tuning improvements reach existing users. Anything the user
        // edited is saved as CUSTOM and is kept exactly as stored.
        params = prof->active.preset == PresetId::Custom ? prof->active : makePreset (prof->active.preset);
        customParams = prof->custom;
        hasCustom = prof->hasCustom;
        loaded = true;
    }
    else
    {
        // New device: the user's chosen "sound for new devices" (CINEMA by default,
        // FLAT if they prefer). Presets are protected (limiter, bass protection).
        params = makePreset (settings.newDevicePreset);
        customParams = params;
        hasCustom = false;
    }

    outputId = output.id;
    outputName = output.name;
    outputKind = output.kind;
    outputBuiltIn = output.isBuiltIn;
    profileDirty = false;

    // Music / Movies / Auto are global choices: the new device follows them too.
    if (settings.listeningMode != ListeningMode::Manual)
    {
        params = makePreset (presetForMode());
        loaded = false;
    }

    settings.lastOutputId = output.id;
    pushHistory (output.id);
    publishParams();
    scheduleSave();

    if (hadOutput && enabled)
    {
        std::string msg;
        if (oldGone)
            msg = utf8 (oldName) + " disconnected. ";
        std::string presetName = "default";
        for (auto& p : presetCatalog())
            if (p.id == params.preset)
                presetName = p.name;
        msg += "Now playing through " + utf8 (output.name)
             + (loaded ? " (saved profile loaded)." : " (" + presetName + " preset).");
        notice ("info", msg);
    }
}

ProcessingParams Engine::makeProcessingParams() const
{
    ProcessingParams pp;
    pp.user = params;
    pp.bypass = bypass;
    const auto outEp = win::findEndpoint (endpoints, outputId);
    const bool outIsTv = outEp && tvFor (*outEp).isTv;
    pp.headphones = win::isHeadphoneLike (outputKind) && ! outIsTv;

    if (outputKind == win::DeviceKind::Speakers && outputBuiltIn)
        pp.speakerHighPassHz = 80.0f;
    else if (outputKind == win::DeviceKind::BluetoothSpeaker)
        pp.speakerHighPassHz = 45.0f;
    else
        pp.speakerHighPassHz = 0.0f;

    pp.smartTone = settings.smartTone;
    pp.hrtfSet = resolveEarProfile();
    if (auto e = win::findEndpoint (endpoints, outputId))
        pp.tv = tvFor (*e);
    pp.auditionSet = auditionSet;
    pp.auditionId = auditionId;
    return pp;
}

//==============================================================================
// Measured HRTFs ("real ears")
//==============================================================================
namespace
{
    // The default for "auto": a dummy head is the most neutral starting point for most people.
    constexpr const char* kDefaultEarKeys[] { "ku100", "kemar" };
}

void Engine::loadHrtfSets()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                         .getParentDirectory().getChildFile ("hrtf");
    std::vector<std::shared_ptr<const dsp::HrtfSet>> sets;
    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.chrtf");
    files.sort();
    for (auto& f : files)
    {
        juce::MemoryBlock mb;
        if (! f.loadFileAsData (mb))
            continue;
        std::string error;
        if (auto s = dsp::parseHrtf (mb.getData(), mb.getSize(), error))
            sets.push_back (s);
        else
            win::log::warn ("HRTF %s ignored: %s", f.getFileName().toRawUTF8(), error.c_str());
    }
    win::log::info ("Real-ear sets loaded: %d (from %s)", (int) sets.size(), dir.getFullPathName().toRawUTF8());
    dsp::HrtfLibrary::install (std::move (sets));
}

int Engine::resolveEarProfile() const
{
    if (settings.earProfile == "model")
        return -1;
    if (settings.earProfile != "auto")
        if (const int i = dsp::HrtfLibrary::indexOf (settings.earProfile); i >= 0)
            return i;
    for (auto* k : kDefaultEarKeys)
        if (const int i = dsp::HrtfLibrary::indexOf (k); i >= 0)
            return i;
    return dsp::HrtfLibrary::count() > 0 ? 0 : -1;
}

bool Engine::startEarTest (const std::string& key)
{
    int set = -1;
    if (key != "model")
    {
        set = dsp::HrtfLibrary::indexOf (key);
        if (set < 0)
            return false;
    }
    auditionSet = set;
    ++auditionId;
    publishParams();
    return true;
}

bool Engine::headTrackingUseful (std::string* why) const
{
    auto fail = [why] (const char* text) { if (why) *why = text; return false; };
    if (! settings.headTracking)
        return fail ("Off");
    if (! enabled || ! pipeline.isRunning())
        return fail ("Starts when CINEMAX is on.");
    if (! params.surroundEnabled)
        return fail ("Starts when Surround is on.");
    const bool hp = params.surroundMode == SurroundMode::Headphones
                 || (params.surroundMode == SurroundMode::Auto && win::isHeadphoneLike (outputKind)
                     && ! [this] { auto e = win::findEndpoint (endpoints, outputId); return e && tvFor (*e).isTv; }());
    if (! hp)
        return fail ("Starts when the sound goes to headphones or earbuds.");
    if (resolveEarProfile() < 0)
        return fail ("Needs real ears (the head model doesn't follow your head).");
    return true;
}

void Engine::stopEarTest()
{
    auditionSet = -2;
    publishParams();
}

void Engine::publishParams()
{
    paramBuffer.write (makeProcessingParams());
}

void Engine::scheduleSave()
{
    saveTimer.startTimer (1000);
}

void Engine::saveNow()
{
    saveTimer.stopTimer();
    store.saveSettings (settings);

    if (profileDirty && ! outputId.empty())
    {
        DeviceProfile prof;
        prof.active = params;
        prof.custom = customParams;
        prof.hasCustom = hasCustom;
        store.saveProfile (outputId, outputName, prof);
        profileDirty = false;
    }
}

void Engine::pushHistory (const std::wstring& id)
{
    auto& h = settings.outputHistory;
    h.erase (std::remove_if (h.begin(), h.end(), [&] (const std::wstring& x) { return sameId (x, id); }), h.end());
    h.insert (h.begin(), id);
    if (h.size() > 12)
        h.resize (12);
}

//==============================================================================
EngineSnapshot Engine::snapshot() const
{
    EngineSnapshot s;
    s.enabled = enabled;
    s.status = status;
    s.message = statusMessage;
    s.bypass = bypass;
    s.params = params;
    s.settings = settings;
    s.detected = detected;
    s.detectedVia = detectedVia;
    s.earProfiles.push_back ({ "model", "Head model (classic CINEMAX)", "Calculated from a sphere-shaped head; no measurement" });
    for (auto& set : dsp::HrtfLibrary::sets())
        s.earProfiles.push_back ({ set->key, set->name, set->info });
    {
        const int idx = resolveEarProfile();
        const auto sets = dsp::HrtfLibrary::sets();
        s.activeEarProfile = idx >= 0 && idx < (int) sets.size() ? sets[(size_t) idx]->key : std::string ("model");
    }
    for (auto& e : endpoints)
        if (! e.isVirtual && (e.kind == win::DeviceKind::Hdmi || e.kind == win::DeviceKind::Digital
                              || tvSetups.count (e.id) != 0 || win::looksLikeTv (e.name)))
        {
            TvDeviceView tv;
            tv.info = e;
            tv.setup = tvFor (e);
            if (auto it = dolbyCache.find (e.id); it != dolbyCache.end())
                tv.dolby = it->second;
            tv.isOutput = sameId (e.id, outputId);
            s.tvDevices.push_back (tv);
        }

    const bool running = pipeline.isRunning();
    if (running)
        s.format = pipeline.format();

    const auto src = routingEngaged ? win::findEndpoint (endpoints, sourceId) : chooseSource();

    for (auto& e : endpoints)
    {
        DeviceView v;
        v.info = e;
        v.isWindowsDefault = sameId (e.id, defaultRenderId);
        v.isOutput = running && sameId (e.id, outputId);
        v.isSource = routingEngaged && sameId (e.id, sourceId);
        v.hasProfile = store.hasProfile (e.id);
        v.headphoneLike = win::isHeadphoneLike (e.kind) && ! tvFor (e).isTv;
        s.devices.push_back (v);

        if (sameId (e.id, outputId))
            s.output = v;
        if (src && sameId (e.id, src->id))
            s.source = v;
    }

    return s;
}

void Engine::setStatus (EngineStatus s, std::string message)
{
    status = s;
    statusMessage = std::move (message);
}

void Engine::notice (const char* level, const std::string& message)
{
    win::log::info ("[notice:%s] %s", level, message.c_str());
    if (onNotice)
        onNotice (level, message);
}

void Engine::changed()
{
    if (onStateChanged)
        onStateChanged();
}

} // namespace cinemax
