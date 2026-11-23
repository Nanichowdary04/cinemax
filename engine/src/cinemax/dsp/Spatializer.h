#pragma once

#include "ChannelLayout.h"
#include "Filters.h"
#include "Hrtf.h"
#include "Upmixer.h"
#include "../core/Params.h"

#include <vector>

namespace cinemax::dsp
{
//==============================================================================
/** Venue model, from a small theatre to a large concert hall.
    Mono sum (high-passed so bass stays dry and tight) -> early reflections whose
    spacing scales with the venue -> pre-delay -> 4 input diffusers -> 8-line feedback
    delay network (Hadamard mixing, per-line air-absorption damping, two slowly
    modulated lines so sustained notes never ring metallically) -> decorrelated
    stereo tail. All parameter changes glide (no clicks). */
class RoomAmbience
{
public:
    static constexpr int kLines = 8;

    void prepare (double sampleRate);
    void reset() noexcept;
    /** reverbWet / erWet: linear mix amounts (0..~0.4).
        size: 0 = small theatre (RT60 ~0.45 s) .. 1 = large concert hall (~2.1 s). */
    void setParams (float reverbWet, float erWet, float size) noexcept;
    void process (float* left, float* right, int numSamples) noexcept;

    static float rt60ForSize (float size) noexcept { return 0.45f + 1.65f * std::clamp (size, 0.0f, 1.0f); }

private:
    struct Line
    {
        std::vector<float> data;
        int mask = 0, write = 0, length = 1;
        float feedback = 0.0f;
        OnePole damping;
        inline void push (float x) noexcept { data[(size_t) write] = x; write = (write + 1) & mask; }
        inline float read (float delay) const noexcept
        {
            const int di = (int) delay;
            const float frac = delay - (float) di;
            const float a = data[(size_t) ((write - di) & mask)];
            const float b = data[(size_t) ((write - di - 1) & mask)];
            return a + frac * (b - a);
        }
    };

    struct Diffuser
    {
        std::vector<float> data;
        int mask = 0, write = 0, delay = 1;
        float g = 0.6f;
        inline float process (float x) noexcept
        {
            const float delayed = data[(size_t) ((write - delay) & mask)];
            const float v = x - g * delayed;
            data[(size_t) write] = v;
            write = (write + 1) & mask;
            return delayed + g * v;
        }
    };

    void applySize (float size) noexcept;
    inline float readEr (float delaySamples) const noexcept;

    double fs = 48000.0;
    std::vector<float> erBuffer;
    int erMask = 0, erWrite = 0;
    static constexpr int kErTaps = 6;
    std::array<float, kErTaps> erBaseL {}, erBaseR {};   // tap times at size 1 (samples)
    std::array<float, kErTaps> erGainL {}, erGainR {};
    std::array<Line, kLines> lines;
    std::array<Diffuser, 4> diffusers;
    Biquad inHp, inLp;
    Ramp revWet, erWetRamp;
    float revWetTarget = 0.0f, erWetTarget = 0.0f;
    float sizeTarget = 0.25f, appliedSize = -1.0f;
    float preDelay = 0.0f, preDelayTarget = 0.0f;        // samples, glides
    float erScale = 1.0f, erScaleTarget = 1.0f;          // glides
    double lfoPhase1 = 0.0, lfoPhase2 = 0.0, lfoInc1 = 0.0, lfoInc2 = 0.0;
    float lfoDepth = 0.0f;
    float glideCoeff = 0.999f;
};

//==============================================================================
/** Virtual surround / spatial engine. Converts the (possibly multichannel) input to
    stereo while placing every channel at its loudspeaker position:
    - headphones: binaural rendering with a spherical-head HRTF model
      (Woodworth ITD + Brown-Duda head-shadow per ear + rear pinna cue) plus early
      reflections for externalisation
    - speakers: stereo widening with band-limited crosstalk cancellation
    Stereo input is up-mixed (correlation-based centre extraction, decorrelated
    surround ambience) in proportion to the intensity. Intensity 0 / disabled gives
    exactly the original stereo signal (or a standard downmix for multichannel). */
class Spatializer
{
public:
    void prepare (double sampleRate, const ChannelLayout& layout, int maxBlock);
    void reset() noexcept;
    void setParams (const DspParams& p, bool headphonesOutput) noexcept;
    void process (const float* const* in, float* outL, float* outR, int numSamples) noexcept;

    /** Headphone rendering: -1 = spherical head model, >= 0 = measured set (HrtfLibrary index).
        Unknown indexes fall back to the head model. */
    void setHrtf (int setIndex) noexcept;
    int activeHrtf() const noexcept { return validSet (hrtfIndex) ? hrtfIndex : -1; }

    /** Head orientation in degrees (yaw > 0 = turned right, pitch > 0 = looking up). The
        virtual speakers stay where they are in the room, so they move the other way. */
    void setHeadPose (float yawDeg, float pitchDeg) noexcept { poseYaw = yawDeg; posePitch = pitchDeg; }

    /** Speaker virtualiser geometry: half-angle of the speakers seen from the seat (laptop
        default ~10 deg) and how much crosstalk to cancel (0..1). */
    void setSpeakerGeometry (float halfAngleDeg, float crosstalkAmount) noexcept;

    /** Ear-fit test: pink-noise bursts walking round the head (front, right, behind, left,
        above) through one set (-1 = head model). -2 stops. A new id restarts the sequence. */
    void setAudition (int setIndex, unsigned id) noexcept;
    bool auditionActive() const noexcept { return auditionSet != -2 && auditionPos < kAuditionSteps; }
    void renderAudition (float* outL, float* outR, int n) noexcept;

    static constexpr int kAuditionSteps = 5;
    static constexpr float kAuditionAz[kAuditionSteps] { 0.0f, 90.0f, 180.0f, 270.0f, 0.0f };
    static constexpr float kAuditionEl[kAuditionSteps] { 0.0f, 0.0f, 0.0f, 0.0f, 50.0f };

    /** Delay of the output relative to the input (the immersive up-mix frame, else 0). */
    int latencySamples() const noexcept { return immersiveActive ? upmix.latencySamples() : 0; }

    /** Content analyser speech evidence (0..1), for the immersive dialogue enhancer. */
    void setSpeech (float speech01) noexcept { upmix.setSpeech (speech01); }

    /** True while stereo is up-mixed to the 7.1.4 bed (DOLBY-STYLE). */
    bool immersiveRunning() const noexcept { return immersiveActive; }
    const ImmersiveUpmixer& upmixer() const noexcept { return upmix; }

private:
    /** One virtual loudspeaker rendered to both ears with a spherical-head model:
        Woodworth ITD + Brown-Duda head shadow from the true 3-D direction, a rear
        pinna shelf, and for elevated sources the spectral elevation cues
        (Blauert's "above" band around 8 kHz, a dip near 3.6 kHz). */
    struct BinauralSource
    {
        static constexpr int kRing = 1024;
        std::array<float, kRing> ring {};
        int write = 0;
        float delayL = 0.0f, delayR = 0.0f;
        OnePole shadowL, shadowR;
        Biquad rearL, rearR, elevBoostL, elevBoostR, elevCutL, elevCutR;
        bool rear = false, elevated = false;

        void configure (double fs, float azimuthDeg, float elevationDeg) noexcept;
        void reset() noexcept;
        inline void render (float x, float& earL, float& earR) noexcept;
    };

    struct DelayBuffer
    {
        std::vector<float> data;
        int mask = 0, write = 0;
        void allocate (int minSize);
        void clear() noexcept;
        inline float process (float x, int delay) noexcept
        {
            data[(size_t) write] = x;
            const float y = data[(size_t) ((write - delay) & mask)];
            write = (write + 1) & mask;
            return y;
        }
    };

    enum Src
    {
        SrcFL, SrcFR, SrcC, SrcSL, SrcSR, SrcBL, SrcBR, SrcFLC, SrcFRC, SrcBC,
        SrcTFL, SrcTFR, SrcTBL, SrcTBR, SrcTC,        // height layer (real top channels or up-mixed)
        kMaxSources
    };

    static bool isHeightSource (int id) noexcept { return id >= SrcTFL && id <= SrcTC; }
    void synthesiseHeight (const float* sideW, const float* mid, int n) noexcept;

    // --- immersive bed (DOLBY-STYLE) ----------------------------------------------------
    /** One bed speaker fed from a surround/height feed: its own delay (precedence: the
        front stays the anchor) and all-pass chain, so no two bed speakers are coherent. */
    struct Decorrelator
    {
        DelayBuffer line;
        int delay = 0;
        float gain = 1.0f;
        std::array<OnePole, 3> ap;
        inline float process (float x) noexcept
        {
            float y = line.process (x, delay);
            for (auto& a : ap) y = a.process (y);
            return gain * y;
        }
        void reset() noexcept { line.clear(); for (auto& a : ap) a.reset(); }
    };

    /** An early reflection off a wall / the ceiling, carrying the speakers that face it,
        heard from the wall's direction (head model). Externalises the virtual speakers
        without a long reverb tail. */
    struct Reflection
    {
        DelayBuffer line;
        int delay = 0;
        OnePole lp, hp;
        BinauralSource ear;
        std::array<float, kMaxSources> send {};
    };

    void buildImmersive (const float* inL, const float* inR, int n) noexcept;
    void addReflections (float* outL, float* outR, int n) noexcept;
    void resetImmersive() noexcept;

    ImmersiveUpmixer upmix;
    bool immersiveWanted = false, immersiveActive = false;
    bool freshStart = true;   // nothing processed since prepare()/reset(): switch modes at once
    Ramp modeGain;
    std::vector<float> feedData;
    float* feeds[ImmersiveUpmixer::kFeeds] {};
    DelayBuffer alignL, alignR;                  // reference path delayed like the up-mix
    enum { BedSL, BedBL, BedSR, BedBR, BedTFL, BedTBL, BedTFR, BedTBR, kBed };
    std::array<Decorrelator, kBed> bed;
    static constexpr int kReflections = 4;
    std::array<Reflection, kReflections> refl;
    Ramp reflGain;
    float reflGainTarget = 0.0f;

    void renderSources (int numSources, const int* sourceIds, float* outL, float* outR, int n, bool headphones) noexcept;

    double fs = 48000.0;
    ChannelLayout layout;
    int maxBlock = 0;

    // per-block scratch
    std::vector<float> srcData;   // kMaxSources * maxBlock
    float* src[kMaxSources] {};
    std::vector<float> directL, directR, effL, effR, lfeBus, itBuf, wBuf, hBuf, sideBuf, midBuf;

    // parameters
    Ramp intensity, width, height;
    float intensityTarget = 0.0f, widthTarget = 1.0f, heightTarget = 0.0f;
    bool headphones = false;
    bool surroundOn = false;

    // width split (bass stays mono)
    Biquad sideHp;
    // upmix correlation estimators
    double eLR = 0.0, eLL = 0.0, eRR = 0.0, corrCoeff = 0.0;
    // surround decorrelation
    DelayBuffer surroundDelayL, surroundDelayR;
    int surroundDelayLSamples = 0, surroundDelayRSamples = 0;
    std::array<OnePole, 3> apL, apR;
    // speaker-mode crosstalk cancellation
    DelayBuffer xtcDelayL, xtcDelayR;
    int xtcDelaySamples = 0, xtcMaxSamples = 0;
    float xtcAmount = 0.45f;
    Biquad xtcBandL, xtcBandR, xtcLpL, xtcLpR;
    // LFE
    Biquad lfeLp;
    // height up-mix: high-passed, decorrelated feeds for the virtual ceiling speakers
    Biquad heightHpL1, heightHpL2, heightHpR1, heightHpR2;
    DelayBuffer heightDelayL, heightDelayR;
    int heightDelayLSamples = 0, heightDelayRSamples = 0;
    std::array<OnePole, 3> hapL, hapR;
    // speaker-mode height: elevation EQ on the height bus
    Biquad spkElevBoostL, spkElevBoostR, spkElevCutL, spkElevCutR;

    std::array<BinauralSource, kMaxSources> binaural;

    // measured HRTFs ("real ears")
    bool validSet (int i) const noexcept { return i >= 0 && i < (int) hrtfSets.size(); }
    void updateHrirDirections (int numSources, const int* ids) noexcept;
    void relativeDirection (float az, float el, float& relAz, float& relEl) const noexcept;
    std::vector<std::shared_ptr<const PreparedHrtf>> hrtfSets;
    int hrtfIndex = -1;
    float poseYaw = 0.0f, posePitch = 0.0f;
    std::array<HrirVoice, kMaxSources> hrir;
    std::array<int, kMaxSources> voiceSet {};
    std::array<float, kMaxSources> voiceYaw {}, voicePitch {};
    std::array<bool, kMaxSources> voiceUsed {};
    std::array<float, kMaxSources> srcAz {}, srcEl {};

    // ear-fit test
    int auditionSet = -2;
    unsigned auditionId = 0;
    int auditionPos = kAuditionSteps, auditionSample = 0;
    uint32_t noiseState = 0x12345678u;
    float pink[7] {};
    HrirVoice auditionVoice;
    BinauralSource auditionModel;
    std::vector<float> auditionIn;

    RoomAmbience room;
    float roomTarget = 0.0f;
};

} // namespace cinemax::dsp
