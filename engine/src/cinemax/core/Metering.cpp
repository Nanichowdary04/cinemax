#include "Metering.h"

#include <cmath>
#include <cstring>

namespace cinemax
{
MeterBus::MeterBus()
{
    fifoData.assign ((size_t) kFifoSize, 0.0f);
}

void MeterBus::pushAnalysis (const float* left, const float* right, int n) noexcept
{
    if (! analysisEnabled.load (std::memory_order_relaxed))
        return;

    int start1, size1, start2, size2;
    fifo.prepareToWrite (n, start1, size1, start2, size2);   // writes only what fits; never blocks

    for (int i = 0; i < size1; ++i)
        fifoData[(size_t) (start1 + i)] = 0.5f * (left[i] + right[i]);
    for (int i = 0; i < size2; ++i)
        fifoData[(size_t) (start2 + i)] = 0.5f * (left[size1 + i] + right[size1 + i]);

    fifo.finishedWrite (size1 + size2);
}

int MeterBus::readAnalysis (float* dest, int maxSamples) noexcept
{
    int start1, size1, start2, size2;
    fifo.prepareToRead (maxSamples, start1, size1, start2, size2);

    if (size1 > 0) std::memcpy (dest, fifoData.data() + start1, sizeof (float) * (size_t) size1);
    if (size2 > 0) std::memcpy (dest + size1, fifoData.data() + start2, sizeof (float) * (size_t) size2);

    fifo.finishedRead (size1 + size2);
    return size1 + size2;
}

void MeterBus::resetForNewStream (double rate) noexcept
{
    sampleRate.store (rate);
    peakL = 0; peakR = 0; inputPeak = 0;
    rmsDbL = -100; rmsDbR = -100;
    bassRms = 0;
    limiterGrDb = 0; compressorGrDb = 0;
    loudnessLufs = -70;
    clip = false; limiting = false; active = false;
    bufferFillMs = 0; driftPpm = 0;
}

//==============================================================================
Analyzer::Analyzer()
{
    history.assign ((size_t) kHistory, 0.0f);
    scratch.assign ((size_t) 1 << 15, 0.0f);
    fftData.assign ((size_t) kFftSize * 2, 0.0f);
    smoothed.fill (-100.0f);
}

MeterFrame Analyzer::compute (MeterBus& bus)
{
    MeterFrame f;

    // Drain everything the audio thread produced since the last frame into the ring.
    const int got = bus.readAnalysis (scratch.data(), (int) scratch.size());
    for (int i = 0; i < got; ++i)
    {
        history[(size_t) historyPos] = scratch[(size_t) i];
        historyPos = (historyPos + 1) % kHistory;
    }

    const double rate = bus.sampleRate.load();
    if (rate != lastRate)
    {
        lastRate = rate;
        const double binHz = rate / kFftSize;
        for (int b = 0; b <= MeterFrame::kBands; ++b)
        {
            const double hz = 20.0 * std::pow (1000.0, (double) b / MeterFrame::kBands);   // 20 Hz .. 20 kHz
            bandEdges[(size_t) b] = std::clamp ((int) std::lround (hz / binHz), 1, kFftSize / 2 - 1);
        }
        smoothed.fill (-100.0f);
    }

    f.active = bus.active.load();

    // --- spectrum -------------------------------------------------------------
    for (int i = 0; i < kFftSize; ++i)
        fftData[(size_t) i] = history[(size_t) ((historyPos - kFftSize + i + kHistory) % kHistory)];
    std::fill (fftData.begin() + kFftSize, fftData.end(), 0.0f);

    window.multiplyWithWindowingTable (fftData.data(), (size_t) kFftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    // The window is normalised (coherent gain 1), so a full-scale sine peaks at N/2.
    const float norm = 2.0f / (float) kFftSize;

    for (int b = 0; b < MeterFrame::kBands; ++b)
    {
        const int lo = bandEdges[(size_t) b];
        const int hi = std::max (lo, bandEdges[(size_t) b + 1] - 1);
        float mag = 0.0f;
        for (int k = lo; k <= hi; ++k)
            mag = std::max (mag, fftData[(size_t) k]);

        const float db = juce::jlimit (-100.0f, 0.0f, juce::Decibels::gainToDecibels (mag * norm, -100.0f));
        // Fast attack, smooth release (display ballistics, data stays real).
        smoothed[(size_t) b] = db > smoothed[(size_t) b] ? db : smoothed[(size_t) b] * 0.85f + db * 0.15f;
        f.spectrumDb[(size_t) b] = smoothed[(size_t) b];
    }

    // --- waveform: last ~1024 samples, peak-preserving decimation to 256 points ---
    {
        constexpr int span = 1024;
        constexpr int per = span / MeterFrame::kWavePoints;
        for (int p = 0; p < MeterFrame::kWavePoints; ++p)
        {
            float best = 0.0f;
            for (int k = 0; k < per; ++k)
            {
                const float v = history[(size_t) ((historyPos - span + p * per + k + kHistory) % kHistory)];
                if (std::abs (v) > std::abs (best))
                    best = v;
            }
            f.waveform[(size_t) p] = juce::jlimit (-1.0f, 1.0f, best);
        }
    }

    // --- scalar meters (read-and-reset the "since last read" values) -------------
    f.peakDbL = juce::Decibels::gainToDecibels (bus.peakL.exchange (0.0f), -100.0f);
    f.peakDbR = juce::Decibels::gainToDecibels (bus.peakR.exchange (0.0f), -100.0f);
    f.rmsDbL = bus.rmsDbL.load();
    f.rmsDbR = bus.rmsDbR.load();
    f.inputPeakDb = juce::Decibels::gainToDecibels (bus.inputPeak.exchange (0.0f), -100.0f);

    const float bassDb = juce::Decibels::gainToDecibels (bus.bassRms.load(), -100.0f);
    f.bass = juce::jlimit (0.0f, 1.0f, (bassDb + 48.0f) / 42.0f);

    f.limiterGrDb = bus.limiterGrDb.exchange (0.0f);
    f.compressorGrDb = bus.compressorGrDb.load();
    f.loudnessLufs = bus.loudnessLufs.load();
    f.clip = bus.clip.exchange (false);
    f.limiting = bus.limiting.exchange (false);
    f.cpuPercent = bus.cpuPercent.load();
    f.bufferFillMs = bus.bufferFillMs.load();
    f.driftPpm = bus.driftPpm.load();
    f.xruns = bus.xruns.load();
    f.movieProbability = bus.movieProbability.load();
    f.contentConfidence = bus.contentConfidence.load();
    f.smartBassDb = bus.smartBassDb.load();
    f.smartTrebleDb = bus.smartTrebleDb.load();

    return f;
}

} // namespace cinemax
