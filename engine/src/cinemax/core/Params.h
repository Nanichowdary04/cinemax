// DSP parameter model, presets and the lock-free hand-off to the audio thread.
#pragma once

#include <array>
#include <atomic>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace cinemax
{
enum class PresetId { Neutral, Cinema, Imax, Theatre, DeepBass, Gaming, Music, ConcertHall, Dialogue, Night, DolbyStyle, Custom };
enum class SurroundMode { Auto, Headphones, Speakers };

struct EqBand
{
    float freq = 1000.0f;
    float gainDb = 0.0f;
    float q = 1.41f;
};

constexpr int kNumEqBands = 10;
constexpr std::array<float, kNumEqBands> kDefaultEqFreqs { 31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
                                                           1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f };

/** User-facing processing parameters (mirrors DspParams in ui/src/bridge/types.ts).
    Must stay trivially copyable: it is handed to the audio thread by value. */
struct DspParams
{
    PresetId preset = PresetId::Neutral;
    float preampDb = 0.0f;

    bool eqEnabled = true;
    std::array<EqBand, kNumEqBands> eqBands {};
    float bassDb = 0.0f, midDb = 0.0f, trebleDb = 0.0f;

    bool bassEnabled = false;
    float bassBoostDb = 0.0f, bassFreq = 90.0f, subBassDb = 0.0f, bassHarmonics = 0.0f;
    bool bassProtect = true;

    bool dialogueEnabled = false;
    float dialogueAmount = 0.0f, backgroundDb = 0.0f;

    // Smart EQ (Dolby-style "intelligent equalizer"): steers the tonal balance to a target
    bool ieqEnabled = false;
    int ieqProfile = 0;          // 0 balanced, 1 warm, 2 detailed
    float ieqAmount = 0.6f;      // 0..1

    bool surroundEnabled = false;
    SurroundMode surroundMode = SurroundMode::Auto;
    float surroundIntensity = 0.5f, stereoWidth = 0.5f, roomAmount = 0.0f;
    float heightAmount = 0.0f;   // 0..1 overhead layer (virtual ceiling speakers)
    float roomSize = 0.25f;      // 0 = small theatre (RT60 ~0.45 s) .. 1 = large concert hall (~2.1 s)
    // Immersive up-mix: stereo split per frequency into a 7.1.4 bed (front, real centre,
    // surrounds, backs, four ceiling speakers) before the virtual-speaker rendering.
    bool immersive = false;

    // Volume-aware tone: restores the bass / top a lower playback volume takes away.
    bool volumeModeler = false;

    bool dynamicsEnabled = false;
    float compressorAmount = 0.0f;
    bool loudnessEnabled = false;
    float loudnessTargetLufs = -18.0f;
    float limiterCeilingDb = -1.0f;

    DspParams();
};

static_assert (std::is_trivially_copyable_v<DspParams>);

/** The output is a TV (or a soundbar / AV receiver on HDMI or optical). Per device, kept
    apart from the sound presets so changing preset never loses the TV setup. */
struct TvSetup
{
    bool isTv = false;
    int speakers = 1;           // 1 TV speakers, 2 soundbar, 3 AV receiver / home theatre
    float sizeInch = 55.0f;     // screen diagonal (24..120)
    float distanceM = 2.5f;     // seat distance (0.8..8 m)
    float lipSyncMs = 0.0f;     // extra audio delay to match the TV picture (0..300 ms)
    bool letTvDecode = false;   // Dolby passthrough: CINEMAX steps aside while this is the output
};

/** Everything the audio thread needs besides the user parameters. */
struct ProcessingParams
{
    DspParams user;
    bool bypass = false;          // routed but unprocessed (A/B)
    bool headphones = false;      // output is headphone-like (binaural spatial mode)
    float speakerHighPassHz = 0;  // device-aware protection high-pass (0 = none)
    bool smartTone = false;       // automatic bass/treble from the content analyser
    int hrtfSet = -1;             // measured "real ears" set (HrtfLibrary index); -1 = head model
    int auditionSet = -2;         // ear-fit test: -2 = off, -1 = head model, >= 0 = measured set
    unsigned auditionId = 0;      // changes every time a test sequence is (re)started
    TvSetup tv;                   // TV output: speaker tuning, room geometry, lip-sync
    float listenVolumeDb = 0.0f;  // playback volume applied after CINEMAX (phone media volume), dB <= 0
};

/** Head orientation from the webcam tracker (degrees; yaw > 0 = head turned right,
    pitch > 0 = head tilted up). Written by the tracker thread, read by the audio thread. */
struct HeadPose
{
    std::atomic<float> yaw { 0.0f };
    std::atomic<float> pitch { 0.0f };
    std::atomic<bool> active { false };
};

static_assert (std::is_trivially_copyable_v<ProcessingParams>);

//==============================================================================
struct PresetInfo
{
    PresetId id;
    const char* key;      // "deep-bass"
    const char* name;     // "DEEP BASS"
    const char* tagline;
};

const std::vector<PresetInfo>& presetCatalog();
const char* presetKey (PresetId id);
std::optional<PresetId> presetFromKey (const std::string& key);

/** Returns the parameters of a factory preset (Custom returns neutral). */
DspParams makePreset (PresetId id);

/** Clamps every field into its documented range. */
void clampParams (DspParams& p);

const char* surroundModeKey (SurroundMode m);
std::optional<SurroundMode> surroundModeFromKey (const std::string& key);

//==============================================================================
/** Wait-free single-producer / single-consumer triple buffer.
    Writer: message thread. Reader: audio thread. */
template <typename T>
class TripleBuffer
{
    static_assert (std::is_trivially_copyable_v<T>);

public:
    void write (const T& value) noexcept
    {
        slots[back] = value;
        const int prev = middle.exchange (back | dirtyBit, std::memory_order_acq_rel);
        back = prev & indexMask;
    }

    /** Returns true and fills 'out' if a new value was published since the last read. */
    bool read (T& out) noexcept
    {
        if ((middle.load (std::memory_order_acquire) & dirtyBit) == 0)
            return false;

        const int prev = middle.exchange (front, std::memory_order_acq_rel);
        front = prev & indexMask;
        out = slots[front];
        return true;
    }

private:
    static constexpr int dirtyBit = 4, indexMask = 3;
    T slots[3] {};
    std::atomic<int> middle { 1 };
    int front = 0;  // reader only
    int back = 2;   // writer only
};

} // namespace cinemax
