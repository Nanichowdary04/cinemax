// Measured HRTFs ("real ears") for headphone virtual surround.
//
// Sets are measurements of real heads (people and dummy heads) converted offline by
// tools/hrtf/convert_sofa.py into .chrtf files: minimum-phase HRIRs per ear plus a
// separate onset delay (the interaural time difference), resampled onto a regular
// grid (azimuth 0..360 clockwise, elevation -30..+90) and diffuse-field equalised so
// every set has the same overall tone and loudness.
//
// The renderer (HrirVoice) is a direct FIR per ear on a fractionally delayed copy of
// the input. Changing direction (head tracking, switching sets) crossfades between
// the old and new filters and glides the delay, so there are no clicks.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cinemax::dsp
{
/** One measured set as stored on disk (48 kHz). Immutable once loaded. */
struct HrtfSet
{
    std::string key;            // "ku100"
    std::string name;           // "Dummy head (Neumann KU100)"
    std::string info;           // source + licence line
    double sampleRate = 48000.0;
    int taps = 0;
    int numAz = 0, numEl = 0;
    float azStep = 5.0f, elMin = -30.0f, elStep = 15.0f;
    std::vector<float> ir;      // [el][az][ear][tap]
    std::vector<float> delay;   // [el][az][ear] onset delay in samples
};

/** Parses a .chrtf file. Returns nullptr and fills 'error' on failure. */
std::shared_ptr<const HrtfSet> parseHrtf (const void* data, size_t size, std::string& error);

/** A set resampled to the engine's rate, ready for the audio thread. */
struct PreparedHrtf
{
    std::shared_ptr<const HrtfSet> source;
    double fs = 48000.0;
    int taps = 0;
    int numAz = 0, numEl = 0;
    float azStep = 5.0f, elMin = -30.0f, elStep = 15.0f;
    std::vector<float> ir;      // [el][az][ear][tap] at fs
    std::vector<float> delay;   // [el][az][ear] at fs

    const float* irAt (int el, int az, int ear) const noexcept
    {
        return ir.data() + ((size_t) ((el * numAz + az) * 2 + ear)) * (size_t) taps;
    }
    float delayAt (int el, int az, int ear) const noexcept
    {
        return delay[(size_t) ((el * numAz + az) * 2 + ear)];
    }

    /** Bilinear interpolation between the four surrounding grid directions.
        azDeg: clockwise from the front (90 = right); elDeg: up is positive.
        irL/irR receive 'taps' samples. Real-time safe. */
    void lookup (float azDeg, float elDeg, float* irL, float* irR, float& delayL, float& delayR) const noexcept;
};

std::shared_ptr<const PreparedHrtf> prepareHrtf (std::shared_ptr<const HrtfSet> set, double fs);

/** Process-wide list of installed sets. The app installs them once at start-up
    (from the "hrtf" folder next to the executable); the DSP picks them up in prepare(). */
class HrtfLibrary
{
public:
    static void install (std::vector<std::shared_ptr<const HrtfSet>> sets);
    static std::vector<std::shared_ptr<const HrtfSet>> sets();
    static int count();
    static int indexOf (const std::string& key);   // -1 if not installed

    /** All sets resampled to fs (cached per rate). Not real-time safe: call from prepare(). */
    static std::vector<std::shared_ptr<const PreparedHrtf>> preparedFor (double fs);

private:
    static std::mutex& lock();
};

//==============================================================================
/** One virtual loudspeaker rendered through a measured HRTF. */
class HrirVoice
{
public:
    static constexpr int kMaxTaps = 512;
    static constexpr int kDelayRing = 512;      // > max onset delay (~2.5 ms at 192 kHz)

    void prepare (int taps, int fadeSamples);
    void reset() noexcept;

    /** True when a new direction can be accepted (the last crossfade has finished). */
    bool ready() const noexcept { return fadeLeft == 0; }
    bool hasFilter() const noexcept { return primed; }
    int numTaps() const noexcept { return taps; }

    /** Loads a new filter pair. 'immediate' skips the crossfade (first use / after reset). */
    void setFilter (const float* irL, const float* irR, float delayL, float delayR, bool immediate) noexcept;

    /** Pointers the caller may fill with a looked-up filter before calling commit(). */
    float* stagingL() noexcept { return staging[0].data(); }
    float* stagingR() noexcept { return staging[1].data(); }
    void commit (float delayL, float delayR, bool immediate) noexcept;

    /** Adds the binaural rendering of 'in' to outL/outR. */
    void process (const float* in, float* outL, float* outR, int n) noexcept;

private:
    int taps = 0, fadeLen = 256, fadeLeft = 0, histPos = 0, inWrite = 0;
    bool primed = false;
    std::array<std::vector<float>, 2> cur, prev, staging;   // impulse responses, reversed for the dot product
    std::array<std::vector<float>, 2> hist;                  // mirrored history of the delayed input
    std::array<float, kDelayRing> inRing {};
    std::array<float, 2> delay {}, delayTarget {};
};

} // namespace cinemax::dsp
