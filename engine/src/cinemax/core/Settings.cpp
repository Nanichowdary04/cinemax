#include "Settings.h"

#include "cinemax/win/Win32Common.h"

namespace cinemax
{
const char* listeningModeKey (ListeningMode m)
{
    switch (m)
    {
        case ListeningMode::Music:  return "music";
        case ListeningMode::Movies: return "movies";
        case ListeningMode::Auto:   return "auto";
        case ListeningMode::Manual: break;
    }
    return "manual";
}

std::optional<ListeningMode> listeningModeFromKey (const std::string& key)
{
    if (key == "manual") return ListeningMode::Manual;
    if (key == "music")  return ListeningMode::Music;
    if (key == "movies") return ListeningMode::Movies;
    if (key == "auto")   return ListeningMode::Auto;
    return std::nullopt;
}

juce::String toJuce (const std::wstring& w)  { return juce::String (w.c_str()); }
std::wstring toWide (const juce::String& s)  { return s.toWideCharPointer(); }

namespace
{
    template <typename T>
    void readNumber (const juce::var& obj, const char* key, T& out)
    {
        const auto v = obj.getProperty (key, juce::var());
        if (v.isDouble() || v.isInt() || v.isInt64())
        {
            const double d = (double) v;
            if (std::isfinite (d))
                out = (T) d;
        }
    }

    void readBool (const juce::var& obj, const char* key, bool& out)
    {
        const auto v = obj.getProperty (key, juce::var());
        if (v.isBool() || v.isInt())
            out = (bool) v;
    }

    void readString (const juce::var& obj, const char* key, std::wstring& out)
    {
        const auto v = obj.getProperty (key, juce::var());
        if (v.isString())
            out = toWide (v.toString());
    }
}

//==============================================================================
juce::var toVar (const EqBand& b)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("freq", b.freq);
    o->setProperty ("gainDb", b.gainDb);
    o->setProperty ("q", b.q);
    return juce::var (o);
}

juce::var toVar (const DspParams& p)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("preset", presetKey (p.preset));
    o->setProperty ("preampDb", p.preampDb);

    o->setProperty ("eqEnabled", p.eqEnabled);
    juce::Array<juce::var> bands;
    for (auto& b : p.eqBands)
        bands.add (toVar (b));
    o->setProperty ("eqBands", bands);
    o->setProperty ("bassDb", p.bassDb);
    o->setProperty ("midDb", p.midDb);
    o->setProperty ("trebleDb", p.trebleDb);

    o->setProperty ("bassEnabled", p.bassEnabled);
    o->setProperty ("bassBoostDb", p.bassBoostDb);
    o->setProperty ("bassFreq", p.bassFreq);
    o->setProperty ("subBassDb", p.subBassDb);
    o->setProperty ("bassHarmonics", p.bassHarmonics);
    o->setProperty ("bassProtect", p.bassProtect);

    o->setProperty ("dialogueEnabled", p.dialogueEnabled);
    o->setProperty ("dialogueAmount", p.dialogueAmount);
    o->setProperty ("backgroundDb", p.backgroundDb);

    static const char* ieqKeys[] { "balanced", "warm", "detailed" };
    o->setProperty ("ieqEnabled", p.ieqEnabled);
    o->setProperty ("ieqProfile", ieqKeys[std::clamp (p.ieqProfile, 0, 2)]);
    o->setProperty ("ieqAmount", p.ieqAmount);

    o->setProperty ("surroundEnabled", p.surroundEnabled);
    o->setProperty ("surroundMode", surroundModeKey (p.surroundMode));
    o->setProperty ("surroundIntensity", p.surroundIntensity);
    o->setProperty ("stereoWidth", p.stereoWidth);
    o->setProperty ("roomAmount", p.roomAmount);
    o->setProperty ("heightAmount", p.heightAmount);
    o->setProperty ("roomSize", p.roomSize);
    o->setProperty ("immersive", p.immersive);
    o->setProperty ("volumeModeler", p.volumeModeler);

    o->setProperty ("dynamicsEnabled", p.dynamicsEnabled);
    o->setProperty ("compressorAmount", p.compressorAmount);
    o->setProperty ("loudnessEnabled", p.loudnessEnabled);
    o->setProperty ("loudnessTargetLufs", p.loudnessTargetLufs);
    o->setProperty ("limiterCeilingDb", p.limiterCeilingDb);
    return juce::var (o);
}

EqBand mergeBand (const EqBand& base, const juce::var& v)
{
    EqBand b = base;
    readNumber (v, "freq", b.freq);
    readNumber (v, "gainDb", b.gainDb);
    readNumber (v, "q", b.q);
    return b;
}

DspParams mergeParams (const DspParams& base, const juce::var& v, bool* presetWasGiven)
{
    DspParams p = base;

    if (presetWasGiven != nullptr)
        *presetWasGiven = false;

    if (! v.isObject())
        return p;

    const auto preset = v.getProperty ("preset", juce::var());
    if (preset.isString())
        if (auto id = presetFromKey (preset.toString().toStdString()))
        {
            p.preset = *id;
            if (presetWasGiven != nullptr)
                *presetWasGiven = true;
        }

    readNumber (v, "preampDb", p.preampDb);
    readBool (v, "eqEnabled", p.eqEnabled);

    if (auto* bands = v.getProperty ("eqBands", juce::var()).getArray())
        for (int i = 0; i < std::min (bands->size(), kNumEqBands); ++i)
            p.eqBands[(size_t) i] = mergeBand (p.eqBands[(size_t) i], bands->getReference (i));

    readNumber (v, "bassDb", p.bassDb);
    readNumber (v, "midDb", p.midDb);
    readNumber (v, "trebleDb", p.trebleDb);

    readBool (v, "bassEnabled", p.bassEnabled);
    readNumber (v, "bassBoostDb", p.bassBoostDb);
    readNumber (v, "bassFreq", p.bassFreq);
    readNumber (v, "subBassDb", p.subBassDb);
    readNumber (v, "bassHarmonics", p.bassHarmonics);
    readBool (v, "bassProtect", p.bassProtect);

    readBool (v, "dialogueEnabled", p.dialogueEnabled);
    readNumber (v, "dialogueAmount", p.dialogueAmount);
    readBool (v, "ieqEnabled", p.ieqEnabled);
    readNumber (v, "ieqAmount", p.ieqAmount);
    {
        const auto ip = v.getProperty ("ieqProfile", juce::var());
        if (ip.isString())
        {
            const auto k = ip.toString();
            p.ieqProfile = k == "warm" ? 1 : k == "detailed" ? 2 : k == "balanced" ? 0 : p.ieqProfile;
        }
    }
    readNumber (v, "backgroundDb", p.backgroundDb);

    readBool (v, "surroundEnabled", p.surroundEnabled);
    const auto mode = v.getProperty ("surroundMode", juce::var());
    if (mode.isString())
        if (auto m = surroundModeFromKey (mode.toString().toStdString()))
            p.surroundMode = *m;
    readNumber (v, "surroundIntensity", p.surroundIntensity);
    readNumber (v, "stereoWidth", p.stereoWidth);
    readNumber (v, "roomAmount", p.roomAmount);
    readNumber (v, "heightAmount", p.heightAmount);
    readNumber (v, "roomSize", p.roomSize);
    readBool (v, "immersive", p.immersive);
    readBool (v, "volumeModeler", p.volumeModeler);

    readBool (v, "dynamicsEnabled", p.dynamicsEnabled);
    readNumber (v, "compressorAmount", p.compressorAmount);
    readBool (v, "loudnessEnabled", p.loudnessEnabled);
    readNumber (v, "loudnessTargetLufs", p.loudnessTargetLufs);
    readNumber (v, "limiterCeilingDb", p.limiterCeilingDb);

    clampParams (p);
    return p;
}

//==============================================================================
juce::var toVar (const TvSetup& t)
{
    static const char* speakerKeys[] { "tv", "soundbar", "receiver" };
    auto* o = new juce::DynamicObject();
    o->setProperty ("isTv", t.isTv);
    o->setProperty ("speakers", speakerKeys[std::clamp (t.speakers, 1, 3) - 1]);
    o->setProperty ("sizeInch", t.sizeInch);
    o->setProperty ("distanceM", t.distanceM);
    o->setProperty ("lipSyncMs", t.lipSyncMs);
    o->setProperty ("letTvDecode", t.letTvDecode);
    return juce::var (o);
}

TvSetup mergeTv (const TvSetup& base, const juce::var& v)
{
    TvSetup t = base;
    if (v.isObject())
    {
        readBool (v, "isTv", t.isTv);
        readBool (v, "letTvDecode", t.letTvDecode);
        readNumber (v, "sizeInch", t.sizeInch);
        readNumber (v, "distanceM", t.distanceM);
        readNumber (v, "lipSyncMs", t.lipSyncMs);
        const auto sp = v.getProperty ("speakers", juce::var());
        if (sp.isString())
        {
            const auto k = sp.toString();
            t.speakers = k == "soundbar" ? 2 : k == "receiver" ? 3 : k == "tv" ? 1 : t.speakers;
        }
    }
    auto fin = [] (float& x, float lo, float hi, float d) { if (! std::isfinite (x)) x = d; x = std::clamp (x, lo, hi); };
    fin (t.sizeInch, 24.0f, 120.0f, 55.0f);
    fin (t.distanceM, 0.8f, 8.0f, 2.5f);
    fin (t.lipSyncMs, 0.0f, 300.0f, 0.0f);
    t.speakers = std::clamp (t.speakers, 1, 3);
    return t;
}

juce::var toVar (const Settings& s, bool includeInternal)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("listeningMode", listeningModeKey (s.listeningMode));
    o->setProperty ("musicPreset", presetKey (s.musicPreset));
    o->setProperty ("moviePreset", presetKey (s.moviePreset));
    o->setProperty ("gamePreset", presetKey (s.gamePreset));
    o->setProperty ("smartTone", s.smartTone);
    o->setProperty ("earProfile", juce::String (s.earProfile));
    o->setProperty ("headTracking", s.headTracking);
    o->setProperty ("headTrackingFlip", s.headTrackingFlip);
    o->setProperty ("launchOnStartup", s.launchOnStartup);
    o->setProperty ("minimizeToTray", s.minimizeToTray);
    o->setProperty ("startEngineOnLaunch", s.startEngineOnLaunch);
    o->setProperty ("newDevicePreset", presetKey (s.newDevicePreset));
    o->setProperty ("autoSwitchDevices", s.autoSwitchDevices);
    o->setProperty ("bufferMs", s.bufferMs);
    o->setProperty ("compensateWindowsVolume", s.compensateWindowsVolume);
    o->setProperty ("preferredVirtualDeviceId", toJuce (s.preferredVirtualDeviceId));
    o->setProperty ("highQualityResampling", s.highQualityResampling);

    if (includeInternal)
    {
        o->setProperty ("settingsVersion", 2);
        juce::Array<juce::var> history;
        for (auto& id : s.outputHistory)
            history.add (toJuce (id));
        o->setProperty ("outputHistory", history);
        o->setProperty ("lastOutputId", toJuce (s.lastOutputId));
    }
    return juce::var (o);
}

Settings mergeSettings (const Settings& base, const juce::var& v, bool includeInternal)
{
    Settings s = base;
    if (! v.isObject())
        return s;

    {
        const auto lm = v.getProperty ("listeningMode", juce::var());
        if (lm.isString())
            if (auto m = listeningModeFromKey (lm.toString().toStdString()))
                s.listeningMode = *m;

        auto readPreset = [&] (const char* key, PresetId& out)
        {
            const auto pv = v.getProperty (key, juce::var());
            if (pv.isString())
                if (auto id = presetFromKey (pv.toString().toStdString()); id && *id != PresetId::Custom)
                    out = *id;
        };
        readPreset ("musicPreset", s.musicPreset);
        readPreset ("moviePreset", s.moviePreset);
        readPreset ("gamePreset", s.gamePreset);
        readBool (v, "smartTone", s.smartTone);
    }
    {
        const auto ep = v.getProperty ("earProfile", juce::var());
        if (ep.isString())
        {
            const auto k = ep.toString();
            if (k.isNotEmpty() && k.length() <= 40 && k.containsOnly ("abcdefghijklmnopqrstuvwxyz0123456789-_"))
                s.earProfile = k.toStdString();
        }
        readBool (v, "headTracking", s.headTracking);
        readBool (v, "headTrackingFlip", s.headTrackingFlip);
    }
    readBool (v, "launchOnStartup", s.launchOnStartup);
    readBool (v, "minimizeToTray", s.minimizeToTray);
    readBool (v, "startEngineOnLaunch", s.startEngineOnLaunch);
    {
        const auto np = v.getProperty ("newDevicePreset", juce::var());
        if (np.isString())
            if (auto id = presetFromKey (np.toString().toStdString()); id && *id != PresetId::Custom)
                s.newDevicePreset = *id;
    }
    readBool (v, "autoSwitchDevices", s.autoSwitchDevices);
    readNumber (v, "bufferMs", s.bufferMs);
    readBool (v, "compensateWindowsVolume", s.compensateWindowsVolume);
    readString (v, "preferredVirtualDeviceId", s.preferredVirtualDeviceId);
    readBool (v, "highQualityResampling", s.highQualityResampling);

    // Snap the buffer to the supported steps.
    const int steps[] { 10, 20, 40, 80 };
    int best = 20;
    for (int st : steps)
        if (std::abs (st - s.bufferMs) < std::abs (best - s.bufferMs))
            best = st;
    s.bufferMs = best;

    if (includeInternal)
    {
        if (auto* h = v.getProperty ("outputHistory", juce::var()).getArray())
        {
            s.outputHistory.clear();
            for (auto& id : *h)
                if (id.isString() && s.outputHistory.size() < 16)
                    s.outputHistory.push_back (toWide (id.toString()));
        }
        readString (v, "lastOutputId", s.lastOutputId);
    }
    return s;
}

//==============================================================================
Settings ProfileStore::loadSettings() const
{
    if (auto data = win::readFile (win::paths::settingsFile()))
    {
        const auto parsed = juce::JSON::parse (juce::String::fromUTF8 (data->data(), (int) data->size()));
        if (parsed.isObject())
        {
            auto s = mergeSettings (Settings {}, parsed, true);
            // v1 files stored the old "engine off at launch" default the user never chose.
            if ((int) parsed.getProperty ("settingsVersion", 1) < 2)
                s.startEngineOnLaunch = true;
            return s;
        }
    }
    return {};
}

bool ProfileStore::saveSettings (const Settings& s) const
{
    const auto json = juce::JSON::toString (toVar (s, true));
    return win::writeFileAtomically (win::paths::settingsFile(), json.toStdString());
}

juce::File ProfileStore::profileFile (const std::wstring& deviceId) const
{
    const auto dir = juce::File (toJuce (win::paths::profilesDir()));
    return dir.getChildFile (juce::String (win::stableHash (deviceId)) + ".json");
}

std::optional<DeviceProfile> ProfileStore::loadProfile (const std::wstring& deviceId) const
{
    const auto file = profileFile (deviceId);
    if (! file.existsAsFile())
        return std::nullopt;

    const auto parsed = juce::JSON::parse (file);
    if (! parsed.isObject())
        return std::nullopt;

    // Guard against hash collisions: the file records the full device id.
    if (toWide (parsed.getProperty ("deviceId", "").toString()) != deviceId)
        return std::nullopt;

    DeviceProfile prof;
    const DspParams neutral = makePreset (PresetId::Neutral);
    prof.active = mergeParams (neutral, parsed.getProperty ("active", juce::var()));
    const auto custom = parsed.getProperty ("custom", juce::var());
    prof.hasCustom = custom.isObject();
    prof.custom = prof.hasCustom ? mergeParams (neutral, custom) : prof.active;
    return prof;
}

bool ProfileStore::saveProfile (const std::wstring& deviceId, const std::wstring& deviceName, const DeviceProfile& prof) const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("deviceId", toJuce (deviceId));
    o->setProperty ("deviceName", toJuce (deviceName));
    o->setProperty ("savedAt", juce::Time::getCurrentTime().toISO8601 (true));
    o->setProperty ("active", toVar (prof.active));
    if (prof.hasCustom)
        o->setProperty ("custom", toVar (prof.custom));

    const auto json = juce::JSON::toString (juce::var (o));
    return win::writeFileAtomically (toWide (profileFile (deviceId).getFullPathName()), json.toStdString());
}

bool ProfileStore::hasProfile (const std::wstring& deviceId) const
{
    return profileFile (deviceId).existsAsFile();
}

void ProfileStore::removeProfile (const std::wstring& deviceId) const
{
    profileFile (deviceId).deleteFile();
}

} // namespace cinemax
