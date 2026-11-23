#include "WebBridge.h"
#include "cinemax/dsp/TvTuning.h"

#include "CinemaxBinaryData.h"

#include "cinemax/win/Win32Common.h"

#include <shellapi.h>

#include <cstdio>

namespace cinemax::app
{
namespace
{
    std::wstring wideArg (const juce::Array<juce::var>& args, int i)
    {
        return i < args.size() ? toWide (args[i].toString()) : std::wstring();
    }

    bool boolArg (const juce::Array<juce::var>& args, int i)
    {
        return i < args.size() && (bool) args[i];
    }

    void appendNumber (std::string& s, double v, int decimals)
    {
        char buf[32];
        if (! std::isfinite (v)) v = 0.0;
        std::snprintf (buf, sizeof (buf), "%.*f", decimals, v);
        s += buf;
    }
}

//==============================================================================
WebBridge::WebBridge (Engine& e, vision::VisionEngine& v) : engine (e), visionEngine (v)
{
    startTimerHz (30);
}

WebBridge::~WebBridge()
{
    stopTimer();
}

void WebBridge::attach (juce::WebBrowserComponent* b)
{
    browser = b;
    if (b == nullptr)
    {
        visualizerWanted = false;
        engine.setVisualizerActive (false);
    }
}

juce::var WebBridge::deviceToVar (const DeviceView& d)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("id", toJuce (d.info.id));
    o->setProperty ("name", toJuce (d.info.name));
    o->setProperty ("kind", win::kindId (d.info.kind));
    o->setProperty ("kindLabel", win::kindLabel (d.info.kind));
    o->setProperty ("isVirtual", d.info.isVirtual);
    o->setProperty ("isWindowsDefault", d.isWindowsDefault);
    o->setProperty ("isCinemaxOutput", d.isOutput);
    o->setProperty ("isCinemaxSource", d.isSource);
    o->setProperty ("hasProfile", d.hasProfile);
    o->setProperty ("headphoneLike", d.headphoneLike);
    o->setProperty ("sampleRate", d.info.sampleRate);
    o->setProperty ("channels", d.info.channels);
    return juce::var (o);
}

juce::var WebBridge::stateToVar() const
{
    const auto s = engine.snapshot();

    auto* eng = new juce::DynamicObject();
    eng->setProperty ("enabled", s.enabled);
    eng->setProperty ("status", statusKey (s.status));
    eng->setProperty ("message", juce::String (s.message));
    eng->setProperty ("bypass", s.bypass);
    eng->setProperty ("source", s.source ? deviceToVar (*s.source) : juce::var());
    eng->setProperty ("output", s.output ? deviceToVar (*s.output) : juce::var());
    eng->setProperty ("inputSampleRate", s.format.inputRate);
    eng->setProperty ("outputSampleRate", s.format.outputRate);
    eng->setProperty ("inputChannels", s.format.inputChannels);
    eng->setProperty ("outputChannels", s.format.outputChannels);
    eng->setProperty ("bufferMs", std::round (s.format.bufferMs * 10.0) / 10.0);
    eng->setProperty ("latencyMs", std::round (s.format.latencyMs * 10.0) / 10.0);
    eng->setProperty ("detectedContent", detectedContentKey (s.detected));
    eng->setProperty ("detectedVia", juce::String (s.detectedVia));

    juce::Array<juce::var> devices;
    for (auto& d : s.devices)
        devices.add (deviceToVar (d));

    juce::Array<juce::var> presets;
    for (auto& p : presetCatalog())
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", p.key);
        o->setProperty ("name", p.name);
        o->setProperty ("tagline", p.tagline);
        presets.add (juce::var (o));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("version", JUCE_APPLICATION_VERSION_STRING);
    root->setProperty ("engine", juce::var (eng));
    root->setProperty ("devices", devices);
    root->setProperty ("params", toVar (s.params));
    root->setProperty ("settings", toVar (s.settings, false));
    root->setProperty ("presets", presets);
    root->setProperty ("vision", vision::toVar (visionEngine.snapshot()));

    juce::Array<juce::var> ears;
    for (auto& e : s.earProfiles)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("key", juce::String (e.key));
        o->setProperty ("name", juce::String (e.name));
        o->setProperty ("info", juce::String (e.info));
        ears.add (juce::var (o));
    }
    auto* spatial = new juce::DynamicObject();
    spatial->setProperty ("earProfiles", ears);
    spatial->setProperty ("activeEarProfile", juce::String (s.activeEarProfile));
    spatial->setProperty ("headTracking", headTrackingToVar());

    juce::Array<juce::var> tvDevices;
    for (auto& t : s.tvDevices)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", toJuce (t.info.id));
        o->setProperty ("name", toJuce (t.info.name));
        o->setProperty ("kind", win::kindId (t.info.kind));
        o->setProperty ("isOutput", t.isOutput);
        o->setProperty ("setup", toVar (t.setup));
        o->setProperty ("speakerAngle", std::round (dsp::tvSpeakerHalfAngle (t.setup) * 2.0f));   // full angle between the speakers
        auto* d = new juce::DynamicObject();
        d->setProperty ("probed", t.dolby.probed);
        d->setProperty ("dolbyDigital", t.dolby.dolbyDigital);
        d->setProperty ("dolbyDigitalPlus", t.dolby.dolbyDigitalPlus);
        d->setProperty ("atmos", t.dolby.atmos);
        o->setProperty ("dolby", juce::var (d));
        tvDevices.add (juce::var (o));
    }
    auto* tv = new juce::DynamicObject();
    tv->setProperty ("devices", tvDevices);
    root->setProperty ("tv", juce::var (tv));
    root->setProperty ("spatial", juce::var (spatial));
    return juce::var (root);
}

juce::var WebBridge::headTrackingToVar() const
{
    auto* o = new juce::DynamicObject();
    std::string why;
    const bool useful = engine.headTrackingUseful (&why);
    if (headTracker != nullptr && headTracker->isActive())
    {
        const auto st = headTracker->status();
        o->setProperty ("status", juce::String (st.status));
        o->setProperty ("message", juce::String (st.message));
        o->setProperty ("yaw", std::round (st.yaw * 10.0f) / 10.0f);
        o->setProperty ("pitch", std::round (st.pitch * 10.0f) / 10.0f);
        o->setProperty ("fps", std::round (st.fps));
    }
    else
    {
        o->setProperty ("status", "off");
        o->setProperty ("message", juce::String (useful ? std::string ("Starting...") : why));
        o->setProperty ("yaw", 0.0);
        o->setProperty ("pitch", 0.0);
        o->setProperty ("fps", 0.0);
    }
    return juce::var (o);
}

//==============================================================================
juce::WebBrowserComponent::Options WebBridge::makeOptions()
{
    using Options = juce::WebBrowserComponent::Options;
    using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;
    using Args = juce::Array<juce::var>;

    const auto userData = juce::File (toJuce (win::paths::dataDir())).getChildFile ("WebView2");

    return Options {}
        .withBackend (Options::Backend::webview2)
        .withWinWebView2Options (Options::WinWebView2 {}
                                     .withUserDataFolder (userData)
                                     .withStatusBarDisabled()
                                     .withBuiltInErrorPageDisabled()
                                     .withBackgroundColour (juce::Colour (0xff0a0a0b)))
        .withNativeIntegrationEnabled()
        .withResourceProvider ([] (const juce::String& url) -> std::optional<juce::WebBrowserComponent::Resource>
        {
            if (url == "/" || url == "/index.html")
            {
                juce::WebBrowserComponent::Resource r;
                const auto* bytes = reinterpret_cast<const std::byte*> (CinemaxBinary::index_html);
                r.data.assign (bytes, bytes + CinemaxBinary::index_htmlSize);
                r.mimeType = "text/html";
                return r;
            }
            return std::nullopt;
        })
        .withNativeFunction ("getState", [this] (const Args&, Completion done) { done (stateToVar()); })
        .withNativeFunction ("setEnabled", [this] (const Args& a, Completion done)
        {
            engine.setEnabled (boolArg (a, 0));
            done (stateToVar());
        })
        .withNativeFunction ("setBypass", [this] (const Args& a, Completion done)
        {
            engine.setBypass (boolArg (a, 0));
            done (stateToVar());
        })
        .withNativeFunction ("setParams", [this] (const Args& a, Completion done)
        {
            done (toVar (engine.setParams (a.isEmpty() ? juce::var() : a[0])));
        })
        .withNativeFunction ("setEqBand", [this] (const Args& a, Completion done)
        {
            const int index = a.size() > 0 ? (int) a[0] : -1;
            done (toVar (engine.setEqBand (index, a.size() > 1 ? a[1] : juce::var())));
        })
        .withNativeFunction ("applyPreset", [this] (const Args& a, Completion done)
        {
            const auto id = presetFromKey (a.isEmpty() ? std::string() : a[0].toString().toStdString());
            done (toVar (id ? engine.applyPreset (*id) : engine.currentParams()));
        })
        .withNativeFunction ("resetProfile", [this] (const Args&, Completion done) { done (toVar (engine.resetProfile())); })
        .withNativeFunction ("setListeningMode", [this] (const Args& a, Completion done)
        {
            if (auto m = listeningModeFromKey (a.isEmpty() ? std::string() : a[0].toString().toStdString()))
                engine.setListeningMode (*m);
            done (stateToVar());
        })
        .withNativeFunction ("selectOutputDevice", [this] (const Args& a, Completion done)
        {
            engine.selectOutputDevice (wideArg (a, 0));
            done (stateToVar());
        })
        .withNativeFunction ("selectSourceDevice", [this] (const Args& a, Completion done)
        {
            engine.selectSourceDevice (wideArg (a, 0));
            done (stateToVar());
        })
        .withNativeFunction ("setSettings", [this] (const Args& a, Completion done)
        {
            done (toVar (engine.setSettings (a.isEmpty() ? juce::var() : a[0]), false));
        })
        .withNativeFunction ("startEarTest", [this] (const Args& a, Completion done)
        {
            const auto key = a.isEmpty() ? juce::String() : a[0].toString();
            done (engine.startEarTest (key.toStdString()));
        })
        .withNativeFunction ("setTvSetup", [this] (const Args& a, Completion done)
        {
            if (a.size() >= 2)
                engine.setTvSetup (wideArg (a, 0), a[1]);
            done (stateToVar());
        })
        .withNativeFunction ("visionSetTv", [this] (const Args& a, Completion done)
        {
            if (a.size() >= 2)
                visionEngine.setTv (wideArg (a, 0), a[1]);
            done (stateToVar());
        })
        .withNativeFunction ("getHeadTracking", [this] (const Args&, Completion done) { done (headTrackingToVar()); })
        .withNativeFunction ("stopEarTest", [this] (const Args&, Completion done)
        {
            engine.stopEarTest();
            done (true);
        })
        .withNativeFunction ("resetSettings", [this] (const Args&, Completion done)
        {
            engine.resetSettings();
            done (stateToVar());
        })
        .withNativeFunction ("restoreDefaultRouting", [this] (const Args&, Completion done)
        {
            engine.restoreDefaultRouting();
            done (stateToVar());
        })
        .withNativeFunction ("openWindowsSoundSettings", [this] (const Args&, Completion done)
        {
            const auto r = ShellExecuteW (nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            done ((INT_PTR) r > 32);
        })
        .withNativeFunction ("openExternalUrl", [] (const Args& a, Completion done)
        {
            // Only fixed, known pages may be opened (the UI never passes arbitrary URLs).
            const auto url = a.isEmpty() ? juce::String() : a[0].toString();
            const bool allowed = url == "https://vb-audio.com/Cable/";
            if (allowed)
                ShellExecuteW (nullptr, L"open", url.toWideCharPointer(), nullptr, nullptr, SW_SHOWNORMAL);
            done (allowed);
        })
        .withNativeFunction ("setVisualizerActive", [this] (const Args& a, Completion done)
        {
            visualizerWanted = boolArg (a, 0);
            engine.setVisualizerActive (visualizerWanted);
            done (true);
        })
        // ---- CINEMAX Vision (picture) -------------------------------------------------
        .withNativeFunction ("visionRefresh", [this] (const Args&, Completion done)
        {
            visionEngine.refresh();
            done (stateToVar());
        })
        .withNativeFunction ("visionSetEnabled", [this] (const Args& a, Completion done)
        {
            visionEngine.setEnabled (boolArg (a, 0));
            done (stateToVar());
        })
        .withNativeFunction ("visionSetSettings", [this] (const Args& a, Completion done)
        {
            visionEngine.setSettings (a.isEmpty() ? juce::var() : a[0]);
            done (stateToVar());
        })
        .withNativeFunction ("visionSelectPreset", [this] (const Args& a, Completion done)
        {
            const auto id = vision::picturePresetFromKey (a.size() > 1 ? a[1].toString().toStdString() : std::string());
            if (id) visionEngine.selectPreset (wideArg (a, 0), *id);
            done (stateToVar());
        })
        .withNativeFunction ("visionSetAuto", [this] (const Args& a, Completion done)
        {
            visionEngine.setAuto (wideArg (a, 0));
            done (stateToVar());
        })
        .withNativeFunction ("visionSetParams", [this] (const Args& a, Completion done)
        {
            visionEngine.setParams (wideArg (a, 0), a.size() > 1 ? a[1] : juce::var());
            done (stateToVar());
        })
        .withNativeFunction ("visionSetBacklight", [this] (const Args& a, Completion done)
        {
            const bool ok = visionEngine.setBacklight (wideArg (a, 0), a.size() > 1 ? (int) a[1] : 50);
            done (ok);
        })
        .withNativeFunction ("visionResetDisplay", [this] (const Args& a, Completion done)
        {
            visionEngine.resetDisplay (wideArg (a, 0));
            done (stateToVar());
        })
        .withNativeFunction ("visionSaveProfiles", [this] (const Args&, Completion done)
        {
            visionEngine.saveProfiles();
            done (true);
        })
        .withNativeFunction ("visionSetCompare", [this] (const Args& a, Completion done)
        {
            visionEngine.setCompare (boolArg (a, 0));
            done (true);
        })
        .withNativeFunction ("visionRestoreOriginal", [this] (const Args&, Completion done)
        {
            visionEngine.restoreOriginal();
            done (stateToVar());
        })
        .withNativeFunction ("visionSetHdr", [this] (const Args& a, Completion done)
        {
            visionEngine.setHdr (wideArg (a, 0), boolArg (a, 1));
            done (stateToVar());
        })
        .withNativeFunction ("visionAddRule", [this] (const Args& a, Completion done)
        {
            const auto id = vision::picturePresetFromKey (a.size() > 1 ? a[1].toString().toStdString() : std::string());
            if (id) visionEngine.addRule (wideArg (a, 0), *id);
            done (stateToVar());
        })
        .withNativeFunction ("visionRemoveRule", [this] (const Args& a, Completion done)
        {
            visionEngine.removeRule (wideArg (a, 0));
            done (stateToVar());
        })
        .withNativeFunction ("openWindowsDisplaySettings", [] (const Args& a, Completion done)
        {
            const bool nightLight = boolArg (a, 0);
            const auto r = ShellExecuteW (nullptr, L"open", nightLight ? L"ms-settings:nightlight" : L"ms-settings:display",
                                          nullptr, nullptr, SW_SHOWNORMAL);
            done ((INT_PTR) r > 32);
        });
}

//==============================================================================
void WebBridge::emitState()
{
    if (browser != nullptr)
        browser->emitEventIfBrowserIsVisible ("cinemaxState", stateToVar());
}

void WebBridge::emitNotice (const char* level, const std::string& message)
{
    if (browser == nullptr)
        return;

    auto* o = new juce::DynamicObject();
    o->setProperty ("level", level);
    o->setProperty ("message", juce::String (message));
    browser->emitEventIfBrowserIsVisible ("cinemaxNotice", juce::var (o));
}

void WebBridge::timerCallback()
{
    if (browser != nullptr && visualizerWanted && browser->isShowing() && engine.snapshotIsRunning())
        emitMeters();
}

void WebBridge::emitMeters()
{
    const auto f = analyzer.compute (engine.meters());

    // Compact JSON written by hand (fixed decimals) - ~3 KB per frame.
    std::string j;
    j.reserve (4096);
    j += "{\"active\":";
    j += f.active ? "true" : "false";

    j += ",\"spectrum\":[";
    for (size_t i = 0; i < f.spectrumDb.size(); ++i)
    {
        if (i) j += ',';
        appendNumber (j, f.spectrumDb[i], 1);
    }
    j += "],\"waveform\":[";
    for (size_t i = 0; i < f.waveform.size(); ++i)
    {
        if (i) j += ',';
        appendNumber (j, f.waveform[i], 3);
    }
    j += "],\"peakDb\":[";   appendNumber (j, f.peakDbL, 1); j += ','; appendNumber (j, f.peakDbR, 1);
    j += "],\"rmsDb\":[";    appendNumber (j, f.rmsDbL, 1);  j += ','; appendNumber (j, f.rmsDbR, 1);
    j += "],\"inputPeakDb\":";    appendNumber (j, f.inputPeakDb, 1);
    j += ",\"bass\":";            appendNumber (j, f.bass, 3);
    j += ",\"limiterGrDb\":";     appendNumber (j, f.limiterGrDb, 2);
    j += ",\"compressorGrDb\":";  appendNumber (j, f.compressorGrDb, 2);
    j += ",\"loudnessLufs\":";    appendNumber (j, f.loudnessLufs, 1);
    j += ",\"clip\":";            j += f.clip ? "true" : "false";
    j += ",\"limiting\":";        j += f.limiting ? "true" : "false";
    j += ",\"cpuPercent\":";      appendNumber (j, f.cpuPercent, 2);
    j += ",\"bufferFillMs\":";    appendNumber (j, f.bufferFillMs, 1);
    j += ",\"driftPpm\":";        appendNumber (j, f.driftPpm, 1);
    j += ",\"xruns\":";           j += std::to_string (f.xruns);
    j += ",\"movieProbability\":";  appendNumber (j, f.movieProbability, 3);
    j += ",\"contentConfidence\":"; appendNumber (j, f.contentConfidence, 3);
    j += ",\"smartBassDb\":";       appendNumber (j, f.smartBassDb, 2);
    j += ",\"smartTrebleDb\":";     appendNumber (j, f.smartTrebleDb, 2);
    j += '}';

    // Same mechanism JUCE uses for emitEventIfBrowserIsVisible (no quotes/backslashes in j).
    browser->evaluateJavascript ("window.__JUCE__.backend.emitByBackend(\"cinemaxMeters\", '" + juce::String (j) + "')");
}

} // namespace cinemax::app
