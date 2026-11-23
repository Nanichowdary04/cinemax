// Real-time safe metering hand-off from the audio thread to the UI, and the
// UI-side analyser that turns it into spectrum / waveform frames.
// Every number shown in the UI comes from here - nothing is synthesised.
#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <vector>

namespace cinemax
{
/** Shared between the audio thread (writer) and the message thread (reader). */
class MeterBus
{
public:
    MeterBus();

    //==============================================================================
    // Audio thread
    void pushAnalysis (const float* left, const float* right, int n) noexcept;
    static void storeMax (std::atomic<float>& a, float v) noexcept
    {
        float prev = a.load (std::memory_order_relaxed);
        while (v > prev && ! a.compare_exchange_weak (prev, v, std::memory_order_relaxed)) {}
    }

    std::atomic<float> peakL { 0 }, peakR { 0 };       // linear, max since last read
    std::atomic<float> rmsDbL { -100 }, rmsDbR { -100 };
    std::atomic<float> inputPeak { 0 };                 // linear, max since last read
    std::atomic<float> bassRms { 0 };                   // linear
    std::atomic<float> limiterGrDb { 0 };               // max since last read
    std::atomic<float> compressorGrDb { 0 };
    std::atomic<float> loudnessLufs { -70 };
    std::atomic<float> cpuPercent { 0 };
    std::atomic<float> bufferFillMs { 0 };
    std::atomic<float> driftPpm { 0 };
    std::atomic<bool>  clip { false }, limiting { false }, active { false };
    std::atomic<uint32_t> xruns { 0 };
    std::atomic<float> movieProbability { 0.5f };     // content analyser (input)
    std::atomic<float> contentConfidence { 0.0f };
    std::atomic<float> smartBassDb { 0.0f }, smartTrebleDb { 0.0f };   // gains actually applied
    std::atomic<float> analysisSpeech { 0.0f }, analysisBeat { 0.0f };  // diagnostics for Auto
    std::atomic<double> sampleRate { 48000.0 };

    /** UI sets this; when false the audio thread skips pushing analysis samples. */
    std::atomic<bool> analysisEnabled { false };

    //==============================================================================
    // Message thread
    int readAnalysis (float* dest, int maxSamples) noexcept;
    void resetForNewStream (double rate) noexcept;

private:
    static constexpr int kFifoSize = 1 << 15;
    juce::AbstractFifo fifo { kFifoSize };
    std::vector<float> fifoData;
};

//==============================================================================
struct MeterFrame
{
    static constexpr int kBands = 64;
    static constexpr int kWavePoints = 256;

    bool active = false;
    std::array<float, kBands> spectrumDb {};
    std::array<float, kWavePoints> waveform {};
    float peakDbL = -100, peakDbR = -100, rmsDbL = -100, rmsDbR = -100;
    float inputPeakDb = -100;
    float bass = 0;
    float limiterGrDb = 0, compressorGrDb = 0;
    float loudnessLufs = -70;
    bool clip = false, limiting = false;
    float cpuPercent = 0, bufferFillMs = 0, driftPpm = 0;
    uint32_t xruns = 0;
    float movieProbability = 0.5f, contentConfidence = 0.0f, smartBassDb = 0.0f, smartTrebleDb = 0.0f;
};

/** Message-thread analyser: FFT spectrum (64 log bands 20 Hz - 20 kHz) + waveform. */
class Analyzer
{
public:
    Analyzer();
    MeterFrame compute (MeterBus& bus);

private:
    static constexpr int kFftOrder = 11;
    static constexpr int kFftSize = 1 << kFftOrder;
    static constexpr int kHistory = 4096;

    juce::dsp::FFT fft { kFftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) kFftSize, juce::dsp::WindowingFunction<float>::hann, true };
    std::vector<float> history;   // ring of the most recent output samples (mono)
    int historyPos = 0;
    std::vector<float> scratch, fftData;
    std::array<float, MeterFrame::kBands> smoothed {};
    double lastRate = 0.0;
    std::array<int, MeterFrame::kBands + 1> bandEdges {};
};

} // namespace cinemax
