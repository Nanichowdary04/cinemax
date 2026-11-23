#include "Params.h"

#include <algorithm>
#include <cmath>

namespace cinemax
{
DspParams::DspParams()
{
    for (int i = 0; i < kNumEqBands; ++i)
        eqBands[(size_t) i] = { kDefaultEqFreqs[(size_t) i], 0.0f, 1.41f };
}

//==============================================================================
const std::vector<PresetInfo>& presetCatalog()
{
    static const std::vector<PresetInfo> catalog {
        { PresetId::Cinema,   "cinema",    "CINEMA",     "Cinematic movie sound: deep controlled bass, clear voices, wide stage." },
        { PresetId::Imax,     "imax",      "IMAX-STYLE", "Wide, enveloping large-screen style simulation with powerful low end." },
        { PresetId::Theatre,  "theatre",   "THEATRE",    "Balanced theatre-style sound with natural room ambience." },
        { PresetId::DeepBass, "deep-bass", "DEEP BASS",  "Strong but protected bass with harmonic reinforcement for small drivers." },
        { PresetId::Gaming,   "gaming",    "GAMING",     "Precise spatial positioning and footstep clarity, minimal room colour." },
        { PresetId::Music,    "music",     "MUSIC",      "Balanced musical reproduction with gentle width and full dynamics." },
        { PresetId::ConcertHall, "concert-hall", "CONCERT HALL", "Music as if played live in a concert hall: natural hall reverb, wide stage, full dynamics." },
        { PresetId::Dialogue, "dialogue",  "DIALOGUE",   "Voices forward and intelligible; background sound reduced." },
        { PresetId::Night,    "night",     "NIGHT MODE", "Controlled dynamic range for quiet late-night listening." },
        { PresetId::DolbyStyle, "dolby-style", "DOLBY-STYLE", "Atmos-style 7.1.4: stereo split into 11 virtual speakers with a real centre for voices and 4 overhead, speech-aware dialogue, volume-aware bass, smart EQ and leveller." },
        { PresetId::Custom,   "custom",    "CUSTOM",     "Your own settings for this device." },
        { PresetId::Neutral,  "neutral",   "FLAT",       "Neutral, unprocessed reference (limiter only)." },
    };
    return catalog;
}

const char* presetKey (PresetId id)
{
    for (auto& p : presetCatalog())
        if (p.id == id)
            return p.key;
    return "neutral";
}

std::optional<PresetId> presetFromKey (const std::string& key)
{
    for (auto& p : presetCatalog())
        if (key == p.key)
            return p.id;
    return std::nullopt;
}

const char* surroundModeKey (SurroundMode m)
{
    switch (m)
    {
        case SurroundMode::Headphones: return "headphones";
        case SurroundMode::Speakers:   return "speakers";
        case SurroundMode::Auto:       break;
    }
    return "auto";
}

std::optional<SurroundMode> surroundModeFromKey (const std::string& key)
{
    if (key == "auto")       return SurroundMode::Auto;
    if (key == "headphones") return SurroundMode::Headphones;
    if (key == "speakers")   return SurroundMode::Speakers;
    return std::nullopt;
}

//==============================================================================
namespace
{
    void setEq (DspParams& p, std::array<float, kNumEqBands> gains)
    {
        for (int i = 0; i < kNumEqBands; ++i)
            p.eqBands[(size_t) i] = { kDefaultEqFreqs[(size_t) i], gains[(size_t) i], 1.41f };
        p.eqEnabled = true;
    }
}

DspParams makePreset (PresetId id)
{
    DspParams p;   // neutral
    p.preset = id;

    switch (id)
    {
        case PresetId::Neutral:
        case PresetId::Custom:
            p.preset = id;
            break;

        case PresetId::Cinema:
            setEq (p, { 2.0f, 3.0f, 1.5f, -0.5f, -1.0f, 0.0f, 1.0f, 1.5f, 1.5f, 1.0f });
            p.preampDb = -1.5f;
            p.bassEnabled = true;  p.bassBoostDb = 4.0f; p.bassFreq = 90.0f; p.subBassDb = 3.0f; p.bassHarmonics = 0.35f;
            p.dialogueEnabled = true; p.dialogueAmount = 0.45f; p.backgroundDb = 0.0f;
            p.surroundEnabled = true; p.surroundIntensity = 0.65f; p.stereoWidth = 0.65f; p.roomAmount = 0.25f; p.heightAmount = 0.35f; p.roomSize = 0.3f;
            p.dynamicsEnabled = true; p.compressorAmount = 0.35f; p.loudnessEnabled = true; p.loudnessTargetLufs = -18.0f;
            p.limiterCeilingDb = -1.0f;
            break;

        case PresetId::Imax:
            setEq (p, { 3.0f, 4.0f, 2.0f, 0.0f, -1.0f, -0.5f, 0.5f, 2.0f, 2.5f, 2.0f });
            p.preampDb = -2.5f;
            p.bassEnabled = true;  p.bassBoostDb = 6.0f; p.bassFreq = 80.0f; p.subBassDb = 5.0f; p.bassHarmonics = 0.4f;
            p.dialogueEnabled = true; p.dialogueAmount = 0.35f;
            p.surroundEnabled = true; p.surroundIntensity = 0.9f; p.stereoWidth = 0.85f; p.roomAmount = 0.4f; p.heightAmount = 0.6f; p.roomSize = 0.5f;
            p.dynamicsEnabled = true; p.compressorAmount = 0.3f; p.loudnessEnabled = true; p.loudnessTargetLufs = -17.0f;
            p.limiterCeilingDb = -1.0f;
            break;

        case PresetId::DolbyStyle:
            // What Dolby's processing does, rebuilt from CINEMAX's own DSP (no Dolby code):
            //  - up-mixer: stereo split per frequency into a 7.1.4 bed - voices on a real
            //    centre, ambience around and above you, matrix surround decoded;
            //  - virtualiser: 11 virtual speakers in a fairly dry room (externalised by
            //    reflections from each speaker's direction, not a long reverb);
            //  - dialogue enhancer acting on speech in the centre only;
            //  - volume modeler, intelligent EQ, volume leveller.
            // The EQ is gentler than before: the smart EQ does the steering.
            setEq (p, { 1.5f, 2.0f, 1.0f, -0.5f, -1.0f, 0.0f, 1.0f, 1.5f, 1.5f, 1.0f });
            p.preampDb = -1.0f;   // the leveller sets the final level; the limiter guards the peaks
            p.bassEnabled = true;  p.bassBoostDb = 4.0f; p.bassFreq = 80.0f; p.subBassDb = 3.0f; p.bassHarmonics = 0.35f;
            p.dialogueEnabled = true; p.dialogueAmount = 0.5f; p.backgroundDb = -1.5f;
            p.surroundEnabled = true; p.surroundIntensity = 1.0f; p.stereoWidth = 0.75f; p.roomAmount = 0.3f; p.heightAmount = 0.8f; p.roomSize = 0.3f;
            p.immersive = true;
            p.volumeModeler = true;
            p.ieqEnabled = true; p.ieqProfile = 0; p.ieqAmount = 0.7f;
            p.dynamicsEnabled = true; p.compressorAmount = 0.25f; p.loudnessEnabled = true; p.loudnessTargetLufs = -16.0f;
            p.limiterCeilingDb = -1.0f;
            break;

        case PresetId::Theatre:
            setEq (p, { 1.0f, 1.5f, 1.0f, 0.0f, 0.0f, 0.0f, 0.5f, 1.0f, 0.5f, 0.0f });
            p.preampDb = -1.0f;
            p.bassEnabled = true;  p.bassBoostDb = 3.0f; p.bassFreq = 100.0f; p.subBassDb = 1.5f; p.bassHarmonics = 0.25f;
            p.dialogueEnabled = true; p.dialogueAmount = 0.4f;
            p.surroundEnabled = true; p.surroundIntensity = 0.55f; p.stereoWidth = 0.6f; p.roomAmount = 0.45f; p.heightAmount = 0.3f; p.roomSize = 0.4f;
            p.dynamicsEnabled = true; p.compressorAmount = 0.3f; p.loudnessEnabled = true; p.loudnessTargetLufs = -18.0f;
            break;

        case PresetId::DeepBass:
            setEq (p, { 4.0f, 5.0f, 3.0f, 0.0f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 0.5f });
            p.preampDb = -3.0f;
            p.bassEnabled = true;  p.bassBoostDb = 8.0f; p.bassFreq = 100.0f; p.subBassDb = 6.0f; p.bassHarmonics = 0.6f;
            p.bassProtect = true;
            p.surroundEnabled = true; p.surroundIntensity = 0.4f; p.stereoWidth = 0.55f; p.roomAmount = 0.1f; p.heightAmount = 0.15f;
            p.dynamicsEnabled = true; p.compressorAmount = 0.35f; p.loudnessEnabled = true; p.loudnessTargetLufs = -18.0f;
            break;

        case PresetId::Gaming:
            setEq (p, { 0.0f, 1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 2.0f, 3.0f, 2.5f, 1.0f });
            p.preampDb = -1.5f;
            p.bassEnabled = true;  p.bassBoostDb = 2.0f; p.bassFreq = 90.0f; p.subBassDb = 1.0f; p.bassHarmonics = 0.2f;
            p.dialogueEnabled = true; p.dialogueAmount = 0.3f;
            p.surroundEnabled = true; p.surroundIntensity = 0.8f; p.stereoWidth = 0.7f; p.roomAmount = 0.1f; p.heightAmount = 0.2f;
            p.dynamicsEnabled = true; p.compressorAmount = 0.25f; p.loudnessEnabled = true; p.loudnessTargetLufs = -16.0f;
            break;

        case PresetId::Music:
            setEq (p, { 1.0f, 1.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 1.0f, 1.5f });
            p.preampDb = -1.0f;
            p.bassEnabled = true;  p.bassBoostDb = 2.5f; p.bassFreq = 90.0f; p.subBassDb = 1.5f; p.bassHarmonics = 0.2f;
            p.surroundEnabled = true; p.surroundIntensity = 0.3f; p.stereoWidth = 0.6f; p.roomAmount = 0.15f;
            p.dynamicsEnabled = false;
            break;

        case PresetId::ConcertHall:
            // Live-hall listening for music: warm low-mids, a touch of air, the band
            // placed on a wide stage in a large hall, dynamics left intact.
            setEq (p, { 0.0f, 1.0f, 1.0f, 0.5f, 0.0f, 0.0f, -0.5f, 0.0f, 0.5f, 1.0f });
            p.preampDb = -1.0f;   // level-matched to the input (measured with cinemax-cli process)
            p.bassEnabled = true;  p.bassBoostDb = 1.5f; p.bassFreq = 80.0f; p.subBassDb = 0.0f; p.bassHarmonics = 0.1f;
            // Tuned down after listening feedback ("too much echo"): a mid-size hall
            // (~1.5 s), less reverb in the mix, lighter surround/height delays.
            p.surroundEnabled = true; p.surroundIntensity = 0.35f; p.stereoWidth = 0.65f;
            p.roomAmount = 0.4f; p.roomSize = 0.65f; p.heightAmount = 0.15f;
            p.dynamicsEnabled = false;
            p.limiterCeilingDb = -1.0f;
            break;

        case PresetId::Dialogue:
            setEq (p, { -2.0f, -1.0f, 0.0f, -1.5f, 0.0f, 1.0f, 2.5f, 3.0f, 1.0f, 0.0f });
            p.dialogueEnabled = true; p.dialogueAmount = 0.85f; p.backgroundDb = -6.0f;
            p.surroundEnabled = true; p.surroundIntensity = 0.25f; p.stereoWidth = 0.45f; p.roomAmount = 0.0f;
            p.dynamicsEnabled = true; p.compressorAmount = 0.5f; p.loudnessEnabled = true; p.loudnessTargetLufs = -16.0f;
            break;

        case PresetId::Night:
            setEq (p, { -3.0f, -2.0f, -1.0f, 0.0f, 0.0f, 0.5f, 1.5f, 1.5f, 0.5f, 0.0f });
            p.bassEnabled = true;  p.bassBoostDb = 0.0f; p.subBassDb = -2.0f; p.bassHarmonics = 0.15f;
            p.dialogueEnabled = true; p.dialogueAmount = 0.6f; p.backgroundDb = -3.0f;
            p.surroundEnabled = true; p.surroundIntensity = 0.35f; p.stereoWidth = 0.5f; p.roomAmount = 0.0f;
            p.dynamicsEnabled = true; p.compressorAmount = 0.85f; p.loudnessEnabled = true; p.loudnessTargetLufs = -20.0f;
            p.limiterCeilingDb = -3.0f;
            break;
    }

    clampParams (p);
    return p;
}

void clampParams (DspParams& p)
{
    auto c = [] (float& v, float lo, float hi, float fallback)
    {
        if (! std::isfinite (v)) v = fallback;
        v = std::clamp (v, lo, hi);
    };

    c (p.preampDb, -12.0f, 6.0f, 0.0f);
    for (auto& b : p.eqBands)
    {
        c (b.freq, 20.0f, 20000.0f, 1000.0f);
        c (b.gainDb, -12.0f, 12.0f, 0.0f);
        c (b.q, 0.3f, 8.0f, 1.41f);
    }
    c (p.bassDb, -12.0f, 12.0f, 0.0f);
    c (p.midDb, -12.0f, 12.0f, 0.0f);
    c (p.trebleDb, -12.0f, 12.0f, 0.0f);
    c (p.bassBoostDb, 0.0f, 12.0f, 0.0f);
    c (p.bassFreq, 40.0f, 160.0f, 90.0f);
    c (p.subBassDb, -6.0f, 9.0f, 0.0f);
    c (p.bassHarmonics, 0.0f, 1.0f, 0.0f);
    c (p.dialogueAmount, 0.0f, 1.0f, 0.0f);
    c (p.backgroundDb, -12.0f, 0.0f, 0.0f);
    c (p.surroundIntensity, 0.0f, 1.0f, 0.5f);
    c (p.stereoWidth, 0.0f, 1.0f, 0.5f);
    c (p.roomAmount, 0.0f, 1.0f, 0.0f);
    c (p.heightAmount, 0.0f, 1.0f, 0.0f);
    c (p.roomSize, 0.0f, 1.0f, 0.25f);
    c (p.compressorAmount, 0.0f, 1.0f, 0.0f);
    c (p.ieqAmount, 0.0f, 1.0f, 0.6f);
    p.ieqProfile = std::clamp (p.ieqProfile, 0, 2);
    c (p.loudnessTargetLufs, -24.0f, -10.0f, -18.0f);
    c (p.limiterCeilingDb, -6.0f, -0.1f, -1.0f);
}

} // namespace cinemax
