#pragma once

#include "BassEnhancer.h"
#include "ChannelLayout.h"
#include "ContentAnalyzer.h"
#include "DialogueEnhancer.h"
#include "Dynamics.h"
#include "Equalizer.h"
#include "IntelligentEq.h"
#include "TvTuning.h"
#include "Spatializer.h"
#include "VolumeModeler.h"
#include "../core/Metering.h"
#include "../core/Params.h"

namespace cinemax::dsp
{
/** The complete CINEMAX processing graph:

      input (N ch) -> volume compensation -> dialogue -> spatial (N -> 2)
      -> EQ -> bass -> loudness normalisation -> compressor
      -> bypass crossfade (dry = plain downmix) -> look-ahead limiter -> meters

    prepare() allocates; process() is real-time safe (no allocation, no locks,
    no system calls). Blocks of any size are accepted (split internally). */
class DspChain
{
public:
    static constexpr int kMaxBlock = 1024;

    void prepare (double sampleRate, const ChannelLayout& layout);
    void reset() noexcept;

    /** Audio thread: apply new parameters (cheap, may be called every block). */
    void setParams (const ProcessingParams& p) noexcept;

    /** Audio thread: the real output device's volume in dB (<= 0; 0 = full / unknown),
        when it is applied after CINEMAX. Drives the volume-aware tone. */
    void setDeviceVolumeDb (float db) noexcept { deviceVolumeDb = db; }

    /** Head tracking source (may be null). The pose is read once per block. */
    void setHeadPose (const HeadPose* pose) noexcept { headPose = pose; }

    /** in: layout.numChannels planar buffers (modified in place).
        inputGain: linear gain applied before processing (Windows volume compensation). */
    void process (float* const* in, int numSamples, float inputGain,
                  float* outL, float* outR, MeterBus* meters) noexcept;

    int latencySamples() const noexcept { return limiter.latencySamples() + lipSync.delaySamples() + spatial.latencySamples(); }
    double sampleRate() const noexcept { return fs; }
    const ChannelLayout& layout() const noexcept { return inLayout; }
    const ContentAnalyzer& contentAnalyzer() const noexcept { return analyzer; }
    const Spatializer& spatializer() const noexcept { return spatial; }
    const VolumeModeler& volumeTone() const noexcept { return volumeModeler; }

private:
    void processBlock (float* const* in, int n, float* outL, float* outR, MeterBus* meters) noexcept;

    double fs = 48000.0;
    ChannelLayout inLayout;
    ProcessingParams params;
    const HeadPose* headPose = nullptr;
    float deviceVolumeDb = 0.0f;

    ContentAnalyzer analyzer;
    SmoothedFilter smartLow, smartHigh;   // smart tone shelves (2 channels)
    DialogueEnhancer dialogue;
    Spatializer spatial;
    Equalizer eq;
    IntelligentEq ieq;
    VolumeModeler volumeModeler;
    TvSpeakerTuning tvTuning;
    LipSyncDelay lipSync;
    BassEnhancer bass;
    LoudnessNormalizer loudness;
    Compressor compressor;
    Limiter limiter;
    LoudnessMeter outputMeter;

    Ramp inputGainRamp, wetMix;
    std::vector<float> dryL, dryR, wetL, wetR;
    // The dry (bypass) path waits for the spatial stage's latency (immersive up-mix), so
    // "hold to compare" switches between time-aligned signals.
    std::vector<float> dryRingL, dryRingR;
    int dryRingMask = 0, dryRingWrite = 0;
    Biquad bassMeterLp;
    double bassEnergy = 0.0, rmsL = 0.0, rmsR = 0.0;
    double meterCoeff = 0.0;
};

} // namespace cinemax::dsp
