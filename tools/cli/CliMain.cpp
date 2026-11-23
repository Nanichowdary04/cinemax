// cinemax-cli: diagnostics and independent testing of each stage.
//
//   cinemax-cli devices
//       List playback endpoints with CINEMAX's classification (read-only).
//
//   cinemax-cli process <in.wav> <out.wav> [--preset NAME] [--headphones]
//       Offline: run a WAV file through the DSP chain (tests processing alone).
//
//   cinemax-cli run [--source ID|default] [--output ID|default] [--seconds N]
//                   [--preset NAME] [--buffer MS] [--mute-output]
//       Live: loopback-capture the source, process, render to the output. Does NOT
//       change Windows routing. --mute-output renders silence (tests capture + DSP +
//       render timing with no audible output); required when source == output.
#include "cinemax/head/FaceAnalyser.h"
#include "HeadTracker.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "cinemax/audio/WasapiPipeline.h"
#include "cinemax/core/Metering.h"
#include "cinemax/core/Params.h"
#include "cinemax/dsp/DspChain.h"
#include "cinemax/win/AudioSessions.h"
#include "cinemax/win/Endpoints.h"

#include <cstdio>
#include <iostream>

using namespace cinemax;

namespace
{
    std::string arg (const juce::StringArray& a, const char* name, const char* fallback = "")
    {
        const int i = a.indexOf (name);
        return (i >= 0 && i + 1 < a.size()) ? a[i + 1].toStdString() : std::string (fallback);
    }

    int listDevices()
    {
        win::ComScope com;
        const auto all = win::enumerateRenderEndpoints (true);
        const auto def = win::getDefaultRenderId (1);

        std::printf ("%-3s %-44s %-34s %-11s %s\n", "", "NAME", "KIND", "FORMAT", "ID");
        for (auto& e : all)
        {
            char fmt[32];
            std::snprintf (fmt, sizeof (fmt), "%d Hz/%dch", e.sampleRate, e.channels);
            std::printf ("%-3s %-44s %-34s %-11s %s\n",
                         _wcsicmp (e.id.c_str(), def.c_str()) == 0 ? "*" : "",
                         win::toUtf8 (e.name).substr (0, 44).c_str(),
                         (std::string (win::kindLabel (e.kind)) + (e.isBuiltIn ? " (built-in)" : "")).c_str(),
                         fmt, win::toUtf8 (e.id).c_str());
        }

        const auto virt = win::virtualCandidates (all);
        std::printf ("\n* = Windows default output.  Virtual source for CINEMAX: %s\n",
                     virt.empty() ? "NONE (install VB-CABLE)" : win::toUtf8 (virt.front().name).c_str());
        return 0;
    }

    int listSessions()
    {
        win::ComScope com;
        const auto all = win::enumerateRenderEndpoints (true);
        std::vector<win::EndpointInfo> targets = win::virtualCandidates (all);
        if (auto d = win::findEndpoint (all, win::getDefaultRenderId (1)); d && ! d->isVirtual)
            targets.push_back (*d);

        for (auto& ep : targets)
        {
            std::printf ("%s\n", win::toUtf8 (ep.name).c_str());
            for (auto& s : win::listRenderSessions (ep.id))
            {
                const auto h = win::classifySession (s);
                std::printf ("    %-28s pid %-6lu %-8s peak %.3f  windows %zu  hint: %s%s\n",
                             win::toUtf8 (s.exeName).c_str(), s.pid, s.active ? "active" : "idle", s.peak,
                             s.windowTitles.size(),
                             h.hint == win::ContentHint::Movie ? "MOVIE" : h.hint == win::ContentHint::Music ? "MUSIC" : "none",
                             h.source.empty() ? "" : (" (" + h.source + ")").c_str());
            }
            const auto best = win::detectAppHint (win::listRenderSessions (ep.id));
            std::printf ("    => app hint: %s %s\n\n",
                         best.hint == win::ContentHint::Movie ? "MOVIE" : best.hint == win::ContentHint::Music ? "MUSIC" : "none (Auto uses sound analysis)",
                         best.source.c_str());
        }
        return 0;
    }

    PresetId presetArg (const juce::StringArray& a)
    {
        const auto name = arg (a, "--preset", "cinema");
        if (auto p = presetFromKey (name))
            return *p;
        std::fprintf (stderr, "Unknown preset '%s', using cinema\n", name.c_str());
        return PresetId::Cinema;
    }

    int processFile (const juce::StringArray& a)
    {
        if (a.size() < 3)
        {
            std::fprintf (stderr, "usage: cinemax-cli process <in.wav> <out.wav> [--preset NAME] [--headphones]\n");
            return 2;
        }

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();

        const juce::File inFile (juce::File::getCurrentWorkingDirectory().getChildFile (a[1]));
        const juce::File outFile (juce::File::getCurrentWorkingDirectory().getChildFile (a[2]));

        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (inFile));
        if (reader == nullptr)
        {
            std::fprintf (stderr, "Cannot read %s\n", inFile.getFullPathName().toRawUTF8());
            return 1;
        }

        const int channels = std::min ((int) reader->numChannels, dsp::kMaxInputChannels);
        const double rate = reader->sampleRate;
        const auto layout = dsp::ChannelLayout::fromMask (0, channels);

        ProcessingParams pp;
        pp.user = makePreset (presetArg (a));
        pp.headphones = a.contains ("--headphones");

        dsp::DspChain chain;
        chain.prepare (rate, layout);
        chain.setParams (pp);
        MeterBus meters;

        outFile.deleteFile();
        std::unique_ptr<juce::OutputStream> os (outFile.createOutputStream().release());
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (os.get(), rate, 2, 24, {}, 0));
        if (writer == nullptr)
        {
            std::fprintf (stderr, "Cannot write %s\n", outFile.getFullPathName().toRawUTF8());
            return 1;
        }
        os.release();   // owned by the writer now

        constexpr int block = 512;
        juce::AudioBuffer<float> in (channels, block), out (2, block);
        const juce::int64 total = reader->lengthInSamples;
        const int latency = chain.latencySamples();
        float maxOut = 0.0f;
        juce::int64 written = 0;
        dsp::LoudnessMeter inLoud, outLoud;
        inLoud.prepare (rate);
        outLoud.prepare (rate);

        for (juce::int64 pos = 0; pos < total + latency; pos += block)
        {
            const int n = (int) std::min<juce::int64> (block, total + latency - pos);
            in.clear();
            if (pos < total)
                reader->read (&in, 0, (int) std::min<juce::int64> (n, total - pos), pos, true, true);

            inLoud.process (in.getReadPointer (0), in.getReadPointer (std::min (1, channels - 1)), n);
            chain.process (in.getArrayOfWritePointers(), n, 1.0f, out.getWritePointer (0), out.getWritePointer (1), &meters);

            // Compensate the limiter look-ahead so output aligns with input.
            const int skip = (int) std::clamp<juce::int64> (latency - pos, 0, n);
            if (n - skip > 0)
            {
                juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, skip, n - skip);
                writer->writeFromAudioSampleBuffer (view, 0, n - skip);
                maxOut = std::max (maxOut, view.getMagnitude (0, n - skip));
                outLoud.process (view.getReadPointer (0), view.getReadPointer (1), n - skip);
                written += n - skip;
            }
        }

        std::printf ("Processed %lld frames at %.0f Hz, %d ch -> stereo. Loudness in %.1f LUFS -> out %.1f LUFS. "
                     "Output peak %.2f dBFS (ceiling %.1f dB).\n",
                     (long long) written, rate, channels, inLoud.integratedLufs(), outLoud.integratedLufs(),
                     juce::Decibels::gainToDecibels (maxOut), pp.user.limiterCeilingDb);
        return 0;
    }

    std::wstring resolveDevice (const std::string& spec)
    {
        if (spec.empty() || spec == "default")
            return win::getDefaultRenderId (1);
        return win::fromUtf8 (spec);
    }

    int runLive (const juce::StringArray& a)
    {
        win::ComScope com;

        audio::PipelineConfig cfg;
        cfg.sourceId = resolveDevice (arg (a, "--source", "default"));
        cfg.outputId = resolveDevice (arg (a, "--output", "default"));
        cfg.bufferMs = std::atoi (arg (a, "--buffer", "20").c_str());
        cfg.muteOutput = a.contains ("--mute-output");
        cfg.highQualityResampling = a.contains ("--hq");

        if (_wcsicmp (cfg.sourceId.c_str(), cfg.outputId.c_str()) == 0 && ! cfg.muteOutput)
        {
            std::fprintf (stderr, "Source and output are the same device: that would feed back. Use --mute-output for a silent test.\n");
            return 2;
        }

        TripleBuffer<ProcessingParams> params;
        MeterBus meters;
        std::atomic<float> gain { 1.0f };
        audio::WasapiPipeline pipeline (params, meters, gain);

        std::atomic<bool> failed { false };
        pipeline.onStreamError = [&failed] (long hr, std::string msg)
        {
            std::fprintf (stderr, "Stream error 0x%08lX: %s\n", (unsigned long) hr, msg.c_str());
            failed = true;
        };

        ProcessingParams pp;
        pp.user = makePreset (presetArg (a));
        params.write (pp);

        const auto r = pipeline.start (cfg, pp);
        if (! r.ok)
        {
            std::fprintf (stderr, "Start failed: %s\n", r.message.c_str());
            return 1;
        }

        const auto f = pipeline.format();
        std::printf ("Running: in %.0f Hz %dch (mask 0x%X) -> out %.0f Hz %dch, buffer %.1f ms, est. latency %.1f ms%s\n",
                     f.inputRate, f.inputChannels, f.inputMask, f.outputRate, f.outputChannels, f.bufferMs, f.latencyMs,
                     cfg.muteOutput ? "  [output muted]" : "");

        meters.analysisEnabled = true;
        Analyzer analyzer;
        const int seconds = std::max (1, std::atoi (arg (a, "--seconds", "10").c_str()));

        for (int s = 0; s < seconds && ! failed; ++s)
        {
            juce::Thread::sleep (1000);
            const auto m = analyzer.compute (meters);
            float specMax = -100.0f;
            for (float v : m.spectrumDb) specMax = std::max (specMax, v);
            std::printf ("[%2ds] active=%d in=%6.1f dB out L/R=%6.1f/%6.1f dB lufs=%6.1f lim=%4.1f dB cpu=%5.2f%% fifo=%5.1f ms drift=%+7.1f ppm xruns=%u spec.max=%6.1f\n",
                         s + 1, m.active ? 1 : 0, m.inputPeakDb, m.peakDbL, m.peakDbR, m.loudnessLufs, m.limiterGrDb,
                         m.cpuPercent, m.bufferFillMs, m.driftPpm, (unsigned) m.xruns, specMax);
            std::fflush (stdout);
        }

        pipeline.stop();
        std::printf ("Stopped. %s\n", failed ? "Stream failed (see above)." : "OK");
        return failed ? 1 : 0;
    }
}

//==============================================================================
// Head tracking checks:
//   headtrack image <file.bgra> <width> <height> [--dir D]   face analysis on a raw top-down BGRA image
//   headtrack camera [seconds] [--dir D]                     live camera: status, fps, angles
//   headtrack yuv <file.bgra> <width> <height> [--dir D]     the phone path: the image as a rotated
//                                                            YUV 4:2:0 camera frame, every rotation
// --dir: folder holding onnxruntime.dll and the models folder (default: next to this exe)
static int headTrack (const juce::StringArray& a)
{
    std::wstring dir = win::paths::exeDir();
    const int di = a.indexOf ("--dir");
    if (di >= 0 && di + 1 < a.size()) dir = a[di + 1].toWideCharPointer();

    if (a[1] == "image" && a.size() >= 5)
    {
        juce::MemoryBlock mb;
        if (! juce::File (a[2]).loadFileAsData (mb)) { std::fprintf (stderr, "cannot read %s\n", a[2].toRawUTF8()); return 1; }
        const int w = a[3].getIntValue(), h = a[4].getIntValue();
        if ((int) mb.getSize() != w * h * 4) { std::fprintf (stderr, "size mismatch\n"); return 1; }
        const auto r = HeadTracker::analyseImage (dir, (const unsigned char*) mb.getData(), w, h, 20);
        if (! r.error.empty()) { std::printf ("error: %s\n", r.error.c_str()); return 1; }
        std::printf ("face %s  score %.2f  yaw %+.1f deg  pitchRaw %.2f  %.2f ms/frame\n",
                     r.found ? "found" : "NOT found", r.score, r.rawYaw, r.rawPitch, r.msPerFrame);
        return r.found ? 0 : 3;
    }
    if (a[1] == "yuv" && a.size() >= 5)
    {
        juce::MemoryBlock mb;
        if (! juce::File (a[2]).loadFileAsData (mb)) { std::fprintf (stderr, "cannot read %s\n", a[2].toRawUTF8()); return 1; }
        const int w = a[3].getIntValue(), h = a[4].getIntValue();
        if ((int) mb.getSize() != w * h * 4) { std::fprintf (stderr, "size mismatch\n"); return 1; }
        const auto* bgra = (const uint8_t*) mb.getData();
        head::FaceAnalyser fa;
        std::string err;
        if (! HeadTracker::loadFaceAnalyser (fa, dir, err)) { std::printf ("error: %s\n", err.c_str()); return 1; }
        int failures = 0;
        for (int rot : { 0, 90, 180, 270 })
        {
            // A phone sensor delivers the picture turned: build the frame that must be turned
            // clockwise by 'rot' to stand up, as YUV 4:2:0 with interleaved V/U (NV21).
            const bool swap = rot == 90 || rot == 270;
            const int sw = swap ? h : w, sh = swap ? w : h;
            std::vector<uint8_t> Y ((size_t) sw * sh), VU ((size_t) sw * (sh / 2));
            auto upright = [&] (int sx, int sy, int& ux, int& uy)
            {
                switch (rot)
                {
                    case 90:  ux = sh - 1 - sy; uy = sx;          break;
                    case 180: ux = sw - 1 - sx; uy = sh - 1 - sy; break;
                    case 270: ux = sy;          uy = sw - 1 - sx; break;
                    default:  ux = sx;          uy = sy;          break;
                }
            };
            for (int sy = 0; sy < sh; ++sy)
                for (int sx = 0; sx < sw; ++sx)
                {
                    int ux, uy;
                    upright (sx, sy, ux, uy);
                    const uint8_t* p = bgra + ((size_t) uy * w + ux) * 4;
                    const float R = p[2], G = p[1], B = p[0];
                    Y[(size_t) sy * sw + sx] = (uint8_t) std::clamp (0.299f * R + 0.587f * G + 0.114f * B + 0.5f, 0.0f, 255.0f);
                    if ((sx & 1) == 0 && (sy & 1) == 0)
                    {
                        const size_t c = (size_t) (sy / 2) * sw + (size_t) sx;
                        VU[c] = (uint8_t) std::clamp (0.5f * R - 0.418688f * G - 0.081312f * B + 128.5f, 0.0f, 255.0f);
                        VU[c + 1] = (uint8_t) std::clamp (-0.168736f * R - 0.331264f * G + 0.5f * B + 128.5f, 0.0f, 255.0f);
                    }
                }
            auto run = [&] (int asRot, float& yawOut, float& scoreOut)
            {
                fa.resetCalibration();
                bool found = false;
                for (int i = 0; i < 10; ++i)
                    found = fa.analyseYuv (Y.data(), sw, VU.data() + 1, VU.data(), sw, 2, sw, sh, asRot, 1.0f / 30.0f);
                yawOut = fa.rawYaw;
                scoreOut = fa.score;
                return found;
            };
            // which way up? probe all four readings, the best score wins (what the phone does)
            float probe[4] {};
            int best = 0;
            for (int k = 0; k < 4; ++k)
            {
                probe[k] = fa.probeYuv (Y.data(), sw, VU.data() + 1, VU.data(), sw, 2, sw, sh, k * 90);
                if (probe[k] > probe[best]) best = k;
            }
            float yaw = 0, score = 0;
            const bool found = run (best * 90, yaw, score);
            std::printf ("sensor turned %3d: probe 0:%.2f 90:%.2f 180:%.2f 270:%.2f -> picks %3d %s | face %s score %.2f yaw %+.1f deg\n",
                         rot, probe[0], probe[1], probe[2], probe[3], best * 90, best * 90 == rot ? "ok" : "WRONG",
                         found ? "found" : "NOT found", score, yaw);
            if (! found || best * 90 != rot) ++failures;
        }
        return failures == 0 ? 0 : 3;
    }
    if (a[1] == "camera")
    {
        const int secs = a.size() > 2 && a[2].containsOnly ("0123456789") ? a[2].getIntValue() : 5;
        HeadPose pose;
        HeadTracker t (pose);
        t.setActive (true);
        for (int i = 0; i < secs * 2; ++i)
        {
            Sleep (500);
            const auto st = t.status();
            std::printf ("%4.1fs  %-12s fps %4.1f  yaw %+5.1f  pitch %+5.1f  %s\n", (i + 1) * 0.5, st.status.c_str(), st.fps, st.yaw, st.pitch, st.message.c_str());
        }
        t.setActive (false);
        return 0;
    }
    std::fprintf (stderr, "usage: headtrack image <file.bgra> <w> <h> | headtrack camera [seconds]\n");
    return 2;
}

int main (int argc, char* argv[])
{
    juce::StringArray a;
    for (int i = 1; i < argc; ++i)
        a.add (juce::String::fromUTF8 (argv[i]));

    if (a.isEmpty() || a[0] == "help" || a[0] == "--help")
    {
        std::printf ("cinemax-cli devices | process <in.wav> <out.wav> [--preset P] [--headphones] |\n"
                     "            run [--source ID] [--output ID] [--seconds N] [--preset P] [--buffer MS] [--mute-output] [--hq]\n"
                     "presets: neutral cinema imax theatre deep-bass gaming music dialogue night\n");
        return 0;
    }

    if (a[0] == "devices") return listDevices();
    if (a[0] == "sessions") return listSessions();
    if (a[0] == "process") return processFile (a);
    if (a[0] == "run")     return runLive (a);
    if (a[0] == "headtrack") return headTrack (a);

    std::fprintf (stderr, "Unknown command '%s'\n", a[0].toRawUTF8());
    return 2;
}
