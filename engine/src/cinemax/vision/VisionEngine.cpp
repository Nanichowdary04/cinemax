#include "VisionEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <sstream>

namespace cinemax::vision
{
namespace
{
    std::wstring lower (std::wstring s)
    {
        for (auto& c : s) c = (wchar_t) std::towlower (c);
        return s;
    }

    juce::String J (const std::wstring& w) { return juce::String (w.c_str()); }
    std::wstring W (const juce::String& s) { return std::wstring (s.toWideCharPointer()); }

    juce::File visionFile() { return juce::File (J (win::paths::dataDir())).getChildFile ("vision.json"); }

    juce::var paramsToVar (const PictureParams& p)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("preset", picturePresetKey (p.preset));
        o->setProperty ("brightness", p.brightness);
        o->setProperty ("contrast", p.contrast);
        o->setProperty ("blackLevel", p.blackLevel);
        o->setProperty ("gamma", p.gamma);
        o->setProperty ("saturation", p.saturation);
        o->setProperty ("vibrance", p.vibrance);
        o->setProperty ("temperatureK", p.temperatureK);
        o->setProperty ("blueLight", p.blueLight);
        o->setProperty ("gain", juce::Array<juce::var> { p.gain[0], p.gain[1], p.gain[2] });
        o->setProperty ("offset", juce::Array<juce::var> { p.offset[0], p.offset[1], p.offset[2] });
        o->setProperty ("sharpness", p.sharpness);
        o->setProperty ("skinProtect", p.skinProtect);
        return juce::var (o);
    }

    PictureParams mergeParams (PictureParams p, const juce::var& v)
    {
        auto* o = v.getDynamicObject();
        if (o == nullptr) return p;
        auto num = [o] (const char* k, float& f) { if (o->hasProperty (k)) f = (float) (double) o->getProperty (k); };
        num ("brightness", p.brightness);
        num ("contrast", p.contrast);
        num ("blackLevel", p.blackLevel);
        num ("gamma", p.gamma);
        num ("saturation", p.saturation);
        num ("vibrance", p.vibrance);
        num ("temperatureK", p.temperatureK);
        num ("blueLight", p.blueLight);
        auto arr3 = [o] (const char* k, std::array<float, 3>& a) {
            if (auto* x = o->getProperty (k).getArray())
                for (int i = 0; i < 3 && i < x->size(); ++i) a[(size_t) i] = (float) (double) (*x)[i];
        };
        arr3 ("gain", p.gain);
        arr3 ("offset", p.offset);
        if (o->hasProperty ("sharpness")) p.sharpness = (int) o->getProperty ("sharpness");
        if (o->hasProperty ("skinProtect")) p.skinProtect = (bool) o->getProperty ("skinProtect");
        if (o->hasProperty ("preset"))
            if (auto id = picturePresetFromKey (o->getProperty ("preset").toString().toStdString())) p.preset = *id;
        clampPicture (p);
        return p;
    }

    std::string signature (const PictureParams& p, const DisplayView& d)
    {
        std::ostringstream s;
        s.precision (4);
        s << (int) d.backend << '|' << d.info.hdrEnabled << '|' << p.brightness << ',' << p.contrast << ',' << p.blackLevel << ','
          << p.gamma << ',' << p.saturation << ',' << p.vibrance << ',' << p.temperatureK << ',' << p.blueLight << ','
          << p.gain[0] << ',' << p.gain[1] << ',' << p.gain[2] << ',' << p.offset[0] << ',' << p.offset[1] << ',' << p.offset[2] << ','
          << p.sharpness << ',' << p.skinProtect;
        return s.str();
    }

    bool shellTransient (const std::wstring& exeLower)
    {
        // Start menu, search, lock screen, task switcher, notification centre: keep the last scene.
        static const wchar_t* names[] = { L"searchhost.exe", L"startmenuexperiencehost.exe", L"shellexperiencehost.exe",
                                          L"lockapp.exe", L"searchapp.exe", L"textinputhost.exe", L"applicationframehost.exe" };
        for (auto* n : names)
            if (exeLower == n) return true;
        return false;
    }
}

const char* sceneKey (Scene s)
{
    switch (s)
    {
        case Scene::Movie: return "movie";
        case Scene::Game:  return "game";
        case Scene::Work:  return "work";
        case Scene::Music: return "music";
        case Scene::None:  break;
    }
    return "none";
}

const char* backendKey (Backend b)
{
    switch (b)
    {
        case Backend::Intel:     return "intel";
        case Backend::GammaRamp: return "gamma";
        case Backend::None:      break;
    }
    return "none";
}

//==============================================================================
VisionEngine::VisionEngine() = default;
VisionEngine::~VisionEngine() { shutdown(); }

void VisionEngine::initialise()
{
    load();
    intelOk = intel.available();

    // A previous run that died before CinemaxGuard could restore: put the display back first.
    if (auto st = win::loadVisionState(); st && st->engaged)
    {
        win::log::warn ("Vision: stale display changes found - restoring before start");
        win::log::info ("%s", win::restoreVision (*st).c_str());
        win::clearVisionState();
    }
    state = {};
    state.pid = GetCurrentProcessId();

    nightLight = win::nightLightOn();
    topology = win::displayTopologyKey();
    detectDisplays (true);
    applyAll (true);

    autoTimer.fn = [this] { autoTick(); };
    autoTimer.startTimer (1000);
    topoTimer.fn = [this] { topologyTick(); };
    topoTimer.startTimer (2000);
    saveTimer.fn = [this] { saveTimer.stopTimer(); saveNow(); };

    win::log::info ("Vision: %d display(s), Intel display pipe %s", (int) displays.size(), intelOk ? "available" : "not available");
}

void VisionEngine::shutdown()
{
    if (shutDown) return;
    shutDown = true;
    autoTimer.stopTimer();
    topoTimer.stopTimer();
    saveTimer.stopTimer();
    for (auto& d : displays)
        unapply (d);
    state.engaged = false;
    win::clearVisionState();
    saveNow();
    intel.close();
    win::log::info ("Vision: displays restored on exit");
}

//==============================================================================
void VisionEngine::detectDisplays (bool force)
{
    auto list = win::listDisplays();
    std::vector<DisplayView> next;
    for (auto& info : list)
    {
        DisplayView d;
        d.info = info;
        if (auto* old = find (info.key))
        {
            d.baseBacklight = old->baseBacklight;
            d.lastSetBacklight = old->lastSetBacklight;
            d.lastOffset = old->lastOffset;
            if (info.backlight >= 0 && old->lastSetBacklight >= 0 && std::abs (info.backlight - old->lastSetBacklight) > 1)
            {
                // The user changed brightness (keys / Windows slider): that is their new level.
                d.baseBacklight = std::clamp (info.backlight - old->lastOffset, 0, 100);
                d.lastSetBacklight = info.backlight;
            }
            d.autoPreset = old->autoPreset;
            d.activePreset = old->activePreset;
            d.params = old->params;
            d.analysis = old->analysis;
            d.processing = old->processing;
            d.limitedByWindows = old->limitedByWindows;
        }
        else
        {
            d.autoPreset = settings.defaultPreset;
        }
        if (d.baseBacklight < 0 && info.backlight >= 0)
        {
            const auto it = savedBacklight.find (info.key);
            if (it != savedBacklight.end() && it->second.base >= 0 && it->second.lastSet >= 0)
            {
                if (std::abs (info.backlight - it->second.lastSet) <= 1)
                {
                    // Still showing what CINEMAX set (it ended without restoring): keep the user's level.
                    d.baseBacklight = it->second.base;
                    d.lastSetBacklight = it->second.lastSet;
                    d.lastOffset = it->second.lastSet - it->second.base;
                }
                else
                {
                    // Restored on exit / by the guard, or changed by the user since: as it is now.
                    d.baseBacklight = info.backlight;
                }
            }
            else
            {
                d.baseBacklight = info.backlight;
            }
        }
        if (auto it = profiles.find (info.key); it != profiles.end())
            d.profile = it->second;

        if (intelOk)
            if (auto c = intel.caps (info.targetId); c && (c->has3DLut || c->hasSharpness))
            {
                d.backend = Backend::Intel;
                d.hasLut = c->has3DLut;
                d.lutSize = c->lutSize;
                d.hasSharpness = c->hasSharpness;
            }
        if (d.backend == Backend::None)
        {
            win::GammaRamp r;
            if (win::getGammaRamp (info.gdiName, r))
                d.backend = Backend::GammaRamp;
        }
        next.push_back (std::move (d));
    }
    displays = std::move (next);
    if (force)
        appliedSig.clear();
}

DisplayView* VisionEngine::find (const std::wstring& key)
{
    for (auto& d : displays)
        if (d.info.key == key) return &d;
    return nullptr;
}

PicturePreset VisionEngine::presetFor (const DisplayView& d) const
{
    if (d.profile.manual || ! settings.autoPicture)
        return d.profile.manualPreset;
    return d.autoPreset;
}

void VisionEngine::applyAll (bool force)
{
    for (auto& d : displays)
        apply (d, force);
    writeState();
}

void VisionEngine::apply (DisplayView& d, bool force)
{
    const auto pr = presetFor (d);
    PictureParams p = pr == PicturePreset::Custom ? (d.profile.hasCustom ? d.profile.custom : PictureParams {}) : makePicturePreset (pr);
    p.preset = pr;
    clampPicture (p);
    d.activePreset = pr;
    d.params = p;
    d.analysis = analysePicture (p);

    const bool want = settings.enabled && ! comparing && ! shutDown;
    const std::string sig = want ? signature (p, d) + '|' + picturePresetKey (pr) + '|' + std::to_string (settings.presetBacklight)
                                   + '|' + std::to_string (d.baseBacklight)
                                 : std::string ("off");
    if (! force && appliedSig[d.info.key] == sig)
        return;
    appliedSig[d.info.key] = sig;

    if (! want)
    {
        unapply (d);
        d.status = comparing ? "Original picture (comparing)" : "Picture processing is off";
        return;
    }

    auto& st = stateFor (d);

    if (d.backend == Backend::Intel)
    {
        bool lutOk = true;
        if (d.hasLut)
        {
            if (d.info.hdrEnabled || isNeutralColour (p))
            {
                if (st.lutApplied) intel.restore3DLut (d.info.targetId);
                st.lutApplied = false;
            }
            else
            {
                lutOk = intel.set3DLut (d.info.targetId, build3DLut (p, d.lutSize), d.lutSize);
                st.lutApplied = lutOk;
            }
        }
        if (d.hasSharpness)
        {
            if (! origSharp.count (d.info.key))
                if (auto s = intel.getSharpness (d.info.targetId))
                {
                    origSharp[d.info.key] = *s;
                    st.sharpOrigEnable = s->enable;
                    st.sharpOrigFilter = s->filter;
                    st.sharpOrigIntensity = s->intensity;
                }
            if (intel.setSharpnessLevel (d.info.targetId, p.sharpness))
                st.sharpnessChanged = true;
        }
        d.processing = st.lutApplied || (d.hasSharpness && p.sharpness > 0);
        d.limitedByWindows = false;
        if (! lutOk)
            d.status = "The graphics driver refused the colour table";
        else if (d.info.hdrEnabled)
            d.status = "HDR is on: Windows colour-manages HDR; CINEMAX applies sharpness only";
        else
            d.status = "Intel display pipe: 3D colour table + hardware sharpness";
    }
    else if (d.backend == Backend::GammaRamp)
    {
        applyGammaFallback (d);
    }
    else
    {
        d.status = "No picture control available for this display";
    }
    applyBacklight (d, settings.presetBacklight);
    applyTv (d);
}

void VisionEngine::applyTv (DisplayView& d)
{
    d.tv.isTv = ! d.info.internal && d.info.hdmi;
    if (! d.tv.isTv || ! intelOk)
        return;
    const auto caps = intel.getTvSettings (d.info.targetId);
    d.tv.available = caps && (caps->canRange || caps->canContentType || caps->canLowLatency);
    if (! d.tv.available)
        return;
    d.tv.canRange = caps->canRange;
    d.tv.canContentType = caps->canContentType;
    d.tv.canLowLatency = caps->canLowLatency;

    const bool on = settings.enabled && ! shutDown;
    const auto& p = d.profile;
    // range: 0 automatic -> driver default; 1 full -> 2 (IGCL full); 2 limited -> 1 (IGCL limited)
    const int range = ! on || p.tvRange == 0 ? 0 : p.tvRange == 1 ? 2 : 1;
    int content = 0;
    if (on && p.tvContentAuto)
        content = scene == Scene::Movie ? 3 : scene == Scene::Game ? 4 : 2;
    const int lowLatency = on && p.tvGameMode && scene == Scene::Game ? 2 : 0;

    if (range == d.tv.sentRange && content == d.tv.sentContent && lowLatency == d.tv.sentLowLatency
        && appliedSig.count (d.info.key + L"|tv") != 0)
        return;
    if (intel.setTvSettings (d.info.targetId, range, content, lowLatency))
    {
        d.tv.sentRange = range;
        d.tv.sentContent = content;
        d.tv.sentLowLatency = lowLatency;
        appliedSig[d.info.key + L"|tv"] = "sent";
        stateFor (d).tvSignalChanged = range != 0 || content != 0 || lowLatency != 0;
        win::log::info ("Vision TV %s: range %d, content %d, low latency %d", win::toUtf8 (d.info.name).c_str(), range, content, lowLatency);
    }
}

void VisionEngine::setTv (const std::wstring& key, const juce::var& v)
{
    auto* d = find (key);
    if (d == nullptr || ! v.isObject()) return;
    const auto r = v.getProperty ("range", juce::var());
    if (r.isString())
        d->profile.tvRange = r.toString() == "full" ? 1 : r.toString() == "limited" ? 2 : 0;
    if (v.hasProperty ("contentAuto")) d->profile.tvContentAuto = (bool) v["contentAuto"];
    if (v.hasProperty ("gameMode")) d->profile.tvGameMode = (bool) v["gameMode"];
    profiles[key] = d->profile;
    applyTv (*d);
    writeState();
    scheduleSave();
    changed();
}

win::VisionDisplayState& VisionEngine::stateFor (const DisplayView& d)
{
    for (auto& s : state.displays)
        if (s.targetId == d.info.targetId) return s;
    state.displays.push_back ({});
    state.displays.back().targetId = d.info.targetId;
    state.displays.back().gdiName = d.info.gdiName;
    return state.displays.back();
}

void VisionEngine::applyBacklight (DisplayView& d, bool usePresetOffset)
{
    if (d.info.backlight < 0 || d.baseBacklight < 0)
        return;
    // The user may have changed brightness since (keyboard keys, Windows slider): read it
    // first, so a mode switch never snaps their choice back.
    if (const int cur = win::getBacklight (d.info); cur >= 0)
    {
        if (d.lastSetBacklight >= 0 && std::abs (cur - d.lastSetBacklight) > 1)
            d.baseBacklight = std::clamp (cur - d.lastOffset, 0, 100);
        d.info.backlight = cur;
    }
    const int offset = usePresetOffset ? presetBacklightOffset (d.activePreset) : 0;
    const int target = std::clamp (d.baseBacklight + offset, 5, 100);
    if (target != d.info.backlight && ! win::setBacklight (d.info, target))
        return;
    d.info.backlight = target;
    d.lastSetBacklight = target;
    d.lastOffset = target - d.baseBacklight;
    const SavedBacklight now { d.baseBacklight, target };
    if (savedBacklight[d.info.key].base != now.base || savedBacklight[d.info.key].lastSet != now.lastSet)
    {
        savedBacklight[d.info.key] = now;
        if (! shutDown) scheduleSave();
    }
    auto& st = stateFor (d);
    st.backlightBase = d.baseBacklight;
    st.backlightChanged = target != d.baseBacklight;
    st.internalPanel = d.info.internal;
    st.wmiInstance = d.info.wmiInstance;
}

void VisionEngine::applyGammaFallback (DisplayView& d)
{
    auto& st = stateFor (d);

    if (! origRamp.count (d.info.key))
    {
        win::GammaRamp r = win::identityRamp();
        win::getGammaRamp (d.info.gdiName, r);
        origRamp[d.info.key] = r;
        st.gammaOrigHex = win::rampToHex (r);
    }
    const auto& orig = origRamp[d.info.key];

    if (isNeutralColour (d.params) || d.info.hdrEnabled)
    {
        if (st.gammaChanged) win::setGammaRamp (d.info.gdiName, orig);
        st.gammaChanged = false;
        d.processing = false;
        d.status = "Windows gamma ramp (fallback): neutral";
        return;
    }

    // Grey-axis curves composed on top of the existing ramp (keeps calibration).
    const auto curves = buildGreyCurves (d.params, 256);
    auto sampleOrig = [&] (int c, double x) {
        const double pos = std::clamp (x, 0.0, 1.0) * 255.0;
        const int i0 = (int) std::floor (pos), i1 = std::min (255, i0 + 1);
        const double f = pos - i0;
        return orig[(size_t) c][(size_t) i0] * (1.0 - f) + orig[(size_t) c][(size_t) i1] * f;
    };
    for (int step = 10; step >= 1; --step)
    {
        const double mix = step / 10.0;
        win::GammaRamp ramp;
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i)
            {
                const double x = i / 255.0;
                const double y = x + (curves[(size_t) c][(size_t) i] - x) * mix;
                ramp[(size_t) c][(size_t) i] = (WORD) std::lround (std::clamp (sampleOrig (c, y), 0.0, 65535.0));
            }
        if (win::setGammaRamp (d.info.gdiName, ramp))
        {
            st.gammaChanged = true;
            d.processing = true;
            d.limitedByWindows = step < 10;
            d.status = d.limitedByWindows ? "Windows gamma ramp (fallback): softened to " + std::to_string (step * 10) + "% - Windows limits strong changes"
                                          : "Windows gamma ramp (fallback): tone, temperature and white balance (no saturation or sharpness)";
            return;
        }
    }
    d.processing = false;
    d.status = "Windows refused the gamma ramp for this display";
}

void VisionEngine::unapply (DisplayView& d)
{
    for (auto& st : state.displays)
    {
        if (st.targetId != d.info.targetId) continue;
        if (st.lutApplied) intel.restore3DLut (d.info.targetId);
        if (st.sharpnessChanged)
        {
            if (auto it = origSharp.find (d.info.key); it != origSharp.end())
                intel.setSharpness (d.info.targetId, it->second);
            else
                intel.setSharpnessLevel (d.info.targetId, 0);
        }
        if (st.gammaChanged)
            if (auto it = origRamp.find (d.info.key); it != origRamp.end())
                win::setGammaRamp (d.info.gdiName, it->second);
        if (st.tvSignalChanged)
        {
            intel.setTvSettings (d.info.targetId, 0, 0, 0);
            d.tv.sentRange = d.tv.sentContent = d.tv.sentLowLatency = 0;
        }
        st.lutApplied = st.sharpnessChanged = st.gammaChanged = st.tvSignalChanged = false;
    }
    applyBacklight (d, false);   // the user's own brightness
    d.processing = false;
    appliedSig[d.info.key] = "off";
}

void VisionEngine::writeState()
{
    bool any = false;
    for (auto& s : state.displays)
        any = any || s.lutApplied || s.sharpnessChanged || s.gammaChanged || s.backlightChanged || s.tvSignalChanged;
    state.engaged = any;
    if (any) win::saveVisionState (state);
    else win::clearVisionState();
}

//==============================================================================
void VisionEngine::autoTick()
{
    if (shutDown || ! settings.enabled || ! settings.autoPicture)
        return;
    auto fg = win::foregroundApp();
    if (! fg) return;
    const auto exe = lower (fg->exe);
    if (fg->pid == GetCurrentProcessId() || exe == L"cinemax.exe" || shellTransient (exe))
        return;   // looking at CINEMAX itself or a transient shell surface: keep the scene

    foregroundExe = fg->exe;
    const auto cls = win::classifyApp (*fg);
    Scene sc = Scene::None;
    switch (cls.category)
    {
        case win::AppCategory::Video:   sc = Scene::Movie; break;
        case win::AppCategory::Game:    sc = Scene::Game; break;
        case win::AppCategory::Work:    sc = Scene::Work; break;
        case win::AppCategory::Music:   sc = Scene::Music; break;
        case win::AppCategory::Browser: sc = cls.browserVideo && settings.browserVideoAsCinema ? Scene::Movie : Scene::None; break;
        case win::AppCategory::None:    break;
    }

    std::optional<PicturePreset> rule;
    for (auto& r : settings.rules)
        if (r.exe == exe) rule = r.preset;

    // Stable for 2 s before switching (alt-tab, notifications).
    if (sc == pendingScene && exe == pendingExe) ++sceneStreak;
    else { pendingScene = sc; pendingExe = exe; sceneStreak = 1; }
    if (sceneStreak < 2) return;

    if (sc == scene && sceneMonitor == fg->monitorGdi && sceneRule == rule && sceneApp == cls.label)
        return;

    const bool sceneKindChanged = sc != scene;
    scene = sc;
    sceneApp = cls.label;
    sceneMonitor = fg->monitorGdi;
    sceneRule = rule;

    const PicturePreset target = rule ? *rule
                               : sc == Scene::Movie ? PicturePreset::Cinema
                               : sc == Scene::Game  ? PicturePreset::Gaming
                               : sc == Scene::Work  ? PicturePreset::Work
                                                    : settings.defaultPreset;
    for (auto& d : displays)
        if (! d.profile.manual && (d.info.gdiName == sceneMonitor || displays.size() == 1) && d.autoPreset != target)
        {
            d.autoPreset = target;
            apply (d, false);
        }
    for (auto& d : displays)
        applyTv (d);
    writeState();

    if (sceneKindChanged || rule)
        win::log::info ("Vision Auto: %s (%s) -> %s", sceneKey (scene), sceneApp.c_str(), picturePresetKey (target));
    if (onSceneChanged) onSceneChanged (scene, sceneApp);
    changed();
}

void VisionEngine::topologyTick()
{
    if (shutDown) return;
    const bool nl = win::nightLightOn();
    const auto key = win::displayTopologyKey();
    if (key != topology)
    {
        topology = key;
        win::log::info ("Vision: display configuration changed - re-detecting and re-applying");
        detectDisplays (true);
        applyAll (true);
        nightLight = nl;
        changed();
        return;
    }
    if (nl != nightLight)
    {
        nightLight = nl;
        changed();
    }
}

void VisionEngine::reapplyAll (const char* reason)
{
    if (shutDown) return;
    win::log::info ("Vision: re-applying picture (%s)", reason);
    topology = win::displayTopologyKey();
    detectDisplays (true);
    applyAll (true);
    changed();
}

void VisionEngine::refresh()
{
    if (shutDown) return;
    detectDisplays (false);
    for (auto& d : displays)
        apply (d, false);   // recomputes analysis; reloads hardware only if something changed
    nightLight = win::nightLightOn();
    changed();
}

//==============================================================================
VisionSnapshot VisionEngine::snapshot() const
{
    VisionSnapshot s;
    s.settings = settings;
    s.displays = displays;
    s.scene = scene;
    s.sceneApp = sceneApp;
    s.foregroundExe = foregroundExe;
    s.comparing = comparing;
    s.nightLight = nightLight;
    s.intelAvailable = intelOk;
    return s;
}

void VisionEngine::setEnabled (bool on)
{
    settings.enabled = on;
    applyAll (false);
    scheduleSave();
    changed();
}

void VisionEngine::setSettings (const juce::var& v)
{
    auto* o = v.getDynamicObject();
    if (o == nullptr) return;
    if (o->hasProperty ("autoPicture")) settings.autoPicture = (bool) o->getProperty ("autoPicture");
    if (o->hasProperty ("linkAudio")) settings.linkAudio = (bool) o->getProperty ("linkAudio");
    if (o->hasProperty ("browserVideoAsCinema")) settings.browserVideoAsCinema = (bool) o->getProperty ("browserVideoAsCinema");
    if (o->hasProperty ("presetBacklight")) settings.presetBacklight = (bool) o->getProperty ("presetBacklight");
    if (o->hasProperty ("defaultPreset"))
        if (auto id = picturePresetFromKey (o->getProperty ("defaultPreset").toString().toStdString()); id && *id != PicturePreset::Custom)
            settings.defaultPreset = *id;
    sceneStreak = 0;
    scene = Scene::None;   // re-evaluate the foreground app with the new settings
    applyAll (false);
    scheduleSave();
    changed();
}

void VisionEngine::selectPreset (const std::wstring& key, PicturePreset p)
{
    auto* d = find (key);
    if (d == nullptr) return;
    if (p == PicturePreset::Custom && ! d->profile.hasCustom)
    {
        d->profile.custom = d->params;
        d->profile.custom.preset = PicturePreset::Custom;
        d->profile.hasCustom = true;
    }
    d->profile.manual = true;
    d->profile.manualPreset = p;
    profiles[key] = d->profile;
    apply (*d, false);
    writeState();
    scheduleSave();
    changed();
}

void VisionEngine::setAuto (const std::wstring& key)
{
    auto* d = find (key);
    if (d == nullptr) return;
    d->profile.manual = false;
    profiles[key] = d->profile;
    sceneStreak = 0;
    scene = Scene::None;
    apply (*d, false);
    writeState();
    scheduleSave();
    changed();
}

void VisionEngine::setParams (const std::wstring& key, const juce::var& partial)
{
    auto* d = find (key);
    if (d == nullptr) return;
    auto p = mergeParams (d->params, partial);
    p.preset = PicturePreset::Custom;
    d->profile.custom = p;
    d->profile.hasCustom = true;
    d->profile.manual = true;
    d->profile.manualPreset = PicturePreset::Custom;
    profiles[key] = d->profile;
    apply (*d, false);
    writeState();
    scheduleSave();
    changed();
}

bool VisionEngine::setBacklight (const std::wstring& key, int percent)
{
    auto* d = find (key);
    if (d == nullptr || d->info.backlight < 0) return false;
    const int pct = std::clamp (percent, 0, 100);
    if (! win::setBacklight (d->info, pct)) return false;
    const bool offsetOn = settings.enabled && ! comparing && settings.presetBacklight;
    d->info.backlight = pct;
    d->lastSetBacklight = pct;
    d->baseBacklight = std::clamp (pct - (offsetOn ? presetBacklightOffset (d->activePreset) : 0), 0, 100);
    d->lastOffset = pct - d->baseBacklight;
    savedBacklight[key] = { d->baseBacklight, pct };
    scheduleSave();
    auto& st = stateFor (*d);
    st.backlightBase = d->baseBacklight;
    st.backlightChanged = pct != d->baseBacklight;
    st.internalPanel = d->info.internal;
    st.wmiInstance = d->info.wmiInstance;
    appliedSig[key].clear();   // the signature carries the level
    writeState();
    changed();
    return true;
}

void VisionEngine::resetDisplay (const std::wstring& key)
{
    auto* d = find (key);
    if (d == nullptr) return;
    d->profile = DisplayProfile {};
    profiles[key] = d->profile;
    d->autoPreset = settings.defaultPreset;
    sceneStreak = 0;
    scene = Scene::None;
    apply (*d, false);
    writeState();
    scheduleSave();
    changed();
}

void VisionEngine::saveProfiles()
{
    saveNow();
    if (onNotice) onNotice ("info", "Picture profiles saved.");
}

void VisionEngine::setCompare (bool on)
{
    if (comparing == on) return;
    comparing = on;
    applyAll (false);
    changed();
}

void VisionEngine::restoreOriginal()
{
    settings.enabled = false;
    comparing = false;
    for (auto& d : displays)
    {
        unapply (d);   // colour table, sharpness, gamma and brightness back to the user's own
        d.status = "Original display restored";
    }
    writeState();
    saveNow();
    if (onNotice) onNotice ("info", "Original display settings restored. Picture processing is off.");
    changed();
}

bool VisionEngine::setHdr (const std::wstring& key, bool on)
{
    auto* d = find (key);
    if (d == nullptr) return false;
    if (on && ! d->info.hdrSupported)
    {
        if (onNotice) onNotice ("warn", "This display does not support HDR - CINEMAX will not force it.");
        return false;
    }
    const bool ok = win::setHdr (d->info, on);
    if (! ok && onNotice) onNotice ("warn", "Windows did not change the HDR setting.");
    reapplyAll (on ? "HDR turned on" : "HDR turned off");
    return ok;
}

void VisionEngine::addRule (const std::wstring& exe, PicturePreset p)
{
    const auto e = lower (exe);
    if (e.empty()) return;
    settings.rules.erase (std::remove_if (settings.rules.begin(), settings.rules.end(), [&] (const AppRule& r) { return r.exe == e; }),
                          settings.rules.end());
    settings.rules.push_back ({ e, p });
    sceneStreak = 0;
    scene = Scene::None;
    scheduleSave();
    changed();
}

void VisionEngine::removeRule (const std::wstring& exe)
{
    const auto e = lower (exe);
    settings.rules.erase (std::remove_if (settings.rules.begin(), settings.rules.end(), [&] (const AppRule& r) { return r.exe == e; }),
                          settings.rules.end());
    sceneStreak = 0;
    scene = Scene::None;
    scheduleSave();
    changed();
}

//==============================================================================
void VisionEngine::load()
{
    const auto file = visionFile();
    if (! file.existsAsFile()) return;
    const auto v = juce::JSON::parse (file);
    auto* root = v.getDynamicObject();
    if (root == nullptr) return;

    if (auto* s = root->getProperty ("settings").getDynamicObject())
    {
        if (s->hasProperty ("enabled")) settings.enabled = (bool) s->getProperty ("enabled");
        if (s->hasProperty ("autoPicture")) settings.autoPicture = (bool) s->getProperty ("autoPicture");
        if (s->hasProperty ("linkAudio")) settings.linkAudio = (bool) s->getProperty ("linkAudio");
        if (s->hasProperty ("browserVideoAsCinema")) settings.browserVideoAsCinema = (bool) s->getProperty ("browserVideoAsCinema");
        if (s->hasProperty ("presetBacklight")) settings.presetBacklight = (bool) s->getProperty ("presetBacklight");
        if (auto id = picturePresetFromKey (s->getProperty ("defaultPreset").toString().toStdString())) settings.defaultPreset = *id;
    }
    if (auto* rules = root->getProperty ("rules").getArray())
        for (auto& r : *rules)
            if (auto id = picturePresetFromKey (r["preset"].toString().toStdString()); id && r["exe"].toString().isNotEmpty())
                settings.rules.push_back ({ lower (W (r["exe"].toString())), *id });
    if (auto* bl = root->getProperty ("backlight").getDynamicObject())
        for (auto& prop : bl->getProperties())
            savedBacklight[W (prop.name.toString())] = { (int) prop.value["base"], (int) prop.value["lastSet"] };
    if (auto* ds = root->getProperty ("displays").getDynamicObject())
        for (auto& prop : ds->getProperties())
        {
            DisplayProfile p;
            const auto& o = prop.value;
            p.manual = (bool) o["manual"];
            if (auto id = picturePresetFromKey (o["manualPreset"].toString().toStdString())) p.manualPreset = *id;
            p.hasCustom = (bool) o["hasCustom"];
            if (p.hasCustom) p.custom = mergeParams (PictureParams {}, o["custom"]);
            p.custom.preset = PicturePreset::Custom;
            p.tvRange = std::clamp ((int) o.getProperty ("tvRange", 0), 0, 2);
            p.tvContentAuto = (bool) o.getProperty ("tvContentAuto", true);
            p.tvGameMode = (bool) o.getProperty ("tvGameMode", true);
            profiles[W (prop.name.toString())] = p;
        }
}

void VisionEngine::scheduleSave()
{
    saveTimer.startTimer (800);
}

void VisionEngine::saveNow()
{
    auto* root = new juce::DynamicObject();
    auto* s = new juce::DynamicObject();
    s->setProperty ("enabled", settings.enabled);
    s->setProperty ("autoPicture", settings.autoPicture);
    s->setProperty ("linkAudio", settings.linkAudio);
    s->setProperty ("browserVideoAsCinema", settings.browserVideoAsCinema);
    s->setProperty ("presetBacklight", settings.presetBacklight);
    s->setProperty ("defaultPreset", picturePresetKey (settings.defaultPreset));
    root->setProperty ("settings", juce::var (s));

    juce::Array<juce::var> rules;
    for (auto& r : settings.rules)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("exe", J (r.exe));
        o->setProperty ("preset", picturePresetKey (r.preset));
        rules.add (juce::var (o));
    }
    root->setProperty ("rules", rules);

    auto* ds = new juce::DynamicObject();
    for (auto& [key, p] : profiles)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("manual", p.manual);
        o->setProperty ("manualPreset", picturePresetKey (p.manualPreset));
        o->setProperty ("hasCustom", p.hasCustom);
        if (p.hasCustom) o->setProperty ("custom", paramsToVar (p.custom));
        o->setProperty ("tvRange", p.tvRange);
        o->setProperty ("tvContentAuto", p.tvContentAuto);
        o->setProperty ("tvGameMode", p.tvGameMode);
        ds->setProperty (juce::Identifier (J (key)), juce::var (o));
    }
    root->setProperty ("displays", juce::var (ds));

    auto* bl = new juce::DynamicObject();
    for (auto& [key, b] : savedBacklight)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("base", b.base);
        o->setProperty ("lastSet", b.lastSet);
        bl->setProperty (juce::Identifier (J (key)), juce::var (o));
    }
    root->setProperty ("backlight", juce::var (bl));

    const auto text = juce::JSON::toString (juce::var (root));
    win::writeFileAtomically (W (visionFile().getFullPathName()), text.toStdString());
}

void VisionEngine::changed()
{
    if (onStateChanged) onStateChanged();
}

//==============================================================================
juce::var toVar (const VisionSnapshot& s)
{
    auto* root = new juce::DynamicObject();
    auto* st = new juce::DynamicObject();
    st->setProperty ("enabled", s.settings.enabled);
    st->setProperty ("autoPicture", s.settings.autoPicture);
    st->setProperty ("linkAudio", s.settings.linkAudio);
    st->setProperty ("browserVideoAsCinema", s.settings.browserVideoAsCinema);
    st->setProperty ("presetBacklight", s.settings.presetBacklight);
    st->setProperty ("defaultPreset", picturePresetKey (s.settings.defaultPreset));
    root->setProperty ("settings", juce::var (st));

    juce::Array<juce::var> rules;
    for (auto& r : s.settings.rules)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("exe", J (r.exe));
        o->setProperty ("preset", picturePresetKey (r.preset));
        rules.add (juce::var (o));
    }
    root->setProperty ("rules", rules);

    juce::Array<juce::var> presets;
    for (auto& p : picturePresetCatalog())
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", p.key);
        o->setProperty ("name", p.name);
        o->setProperty ("tagline", p.tagline);
        presets.add (juce::var (o));
    }
    root->setProperty ("presets", presets);

    juce::Array<juce::var> displays;
    for (auto& d : s.displays)
    {
        auto* o = new juce::DynamicObject();
        const auto& i = d.info;
        o->setProperty ("key", J (i.key));
        o->setProperty ("name", J (i.name));
        o->setProperty ("adapter", J (i.adapterName));
        o->setProperty ("internal", i.internal);
        o->setProperty ("width", i.width);
        o->setProperty ("height", i.height);
        o->setProperty ("refreshHz", std::round (i.refreshHz * 100.0) / 100.0);
        o->setProperty ("bitsPerColor", i.bitsPerColor);
        o->setProperty ("hdrSupported", i.hdrSupported);
        o->setProperty ("hdrEnabled", i.hdrEnabled);
        o->setProperty ("wideColorSupported", i.wideColorSupported);
        o->setProperty ("wideColorEnabled", i.wideColorEnabled);
        o->setProperty ("maxNits", std::round (i.maxNits));
        o->setProperty ("minNits", std::round (i.minNits * 100.0) / 100.0);
        o->setProperty ("sdrWhiteNits", std::round (i.sdrWhiteNits));
        o->setProperty ("gamutPercent", std::round (i.gamutAreaPercentOfSrgb));
        o->setProperty ("ddcci", i.ddcci);
        o->setProperty ("backlight", i.backlight);
        o->setProperty ("baseBacklight", d.baseBacklight);
        o->setProperty ("backlightOffset", s.settings.presetBacklight && s.settings.enabled && ! s.comparing ? presetBacklightOffset (d.activePreset) : 0);
        o->setProperty ("backend", backendKey (d.backend));
        o->setProperty ("hasLut", d.hasLut);
        o->setProperty ("lutSize", d.lutSize);
        o->setProperty ("hasSharpness", d.hasSharpness);
        o->setProperty ("mode", d.profile.manual ? "manual" : "auto");
        o->setProperty ("activePreset", picturePresetKey (d.activePreset));
        o->setProperty ("hasCustom", d.profile.hasCustom);
        o->setProperty ("params", paramsToVar (d.params));
        o->setProperty ("processing", d.processing);
        o->setProperty ("limitedByWindows", d.limitedByWindows);
        o->setProperty ("status", juce::String (d.status));
        o->setProperty ("hdmi", i.hdmi);
        {
            static const char* rangeKeys[] { "auto", "full", "limited" };
            static const char* contentKeys[] { "default", "off", "desktop", "film", "game" };
            auto* tv = new juce::DynamicObject();
            tv->setProperty ("isTv", d.tv.isTv);
            tv->setProperty ("available", d.tv.available);
            tv->setProperty ("canRange", d.tv.canRange);
            tv->setProperty ("canContentType", d.tv.canContentType);
            tv->setProperty ("canGameMode", d.tv.canLowLatency);
            tv->setProperty ("range", rangeKeys[std::clamp (d.profile.tvRange, 0, 2)]);
            tv->setProperty ("contentAuto", d.profile.tvContentAuto);
            tv->setProperty ("gameMode", d.profile.tvGameMode);
            tv->setProperty ("sentContent", contentKeys[std::clamp (d.tv.sentContent, 0, 4)]);
            tv->setProperty ("gameModeActive", d.tv.sentLowLatency == 2);
            o->setProperty ("tv", juce::var (tv));
        }

        auto* a = new juce::DynamicObject();
        a->setProperty ("shadowSteps", d.analysis.shadowSteps);
        a->setProperty ("shadowStepsKept", d.analysis.shadowStepsKept);
        a->setProperty ("highlightSteps", d.analysis.highlightSteps);
        a->setProperty ("highlightStepsKept", d.analysis.highlightStepsKept);
        a->setProperty ("white", juce::Array<juce::var> { d.analysis.white[0], d.analysis.white[1], d.analysis.white[2] });
        a->setProperty ("black", juce::Array<juce::var> { d.analysis.black[0], d.analysis.black[1], d.analysis.black[2] });
        a->setProperty ("gamutLimitedPercent", std::round (d.analysis.gamutLimitedPercent * 10.0) / 10.0);
        a->setProperty ("monotonic", d.analysis.monotonic);
        o->setProperty ("analysis", juce::var (a));
        displays.add (juce::var (o));
    }
    root->setProperty ("displays", displays);
    root->setProperty ("scene", sceneKey (s.scene));
    root->setProperty ("sceneApp", juce::String (s.sceneApp));
    root->setProperty ("foregroundExe", J (s.foregroundExe));
    root->setProperty ("comparing", s.comparing);
    root->setProperty ("nightLight", s.nightLight);
    root->setProperty ("intelAvailable", s.intelAvailable);
    return juce::var (root);
}

} // namespace cinemax::vision
