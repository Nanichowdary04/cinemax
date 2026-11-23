// Volume-aware tone ("volume modeler"): keeps the tonal balance as the volume goes down.
#pragma once

#include "Filters.h"

namespace cinemax::dsp
{
/** Our hearing loses bass (and some treble) faster than mid-range as sound gets quieter
    (equal-loudness contours, ISO 226). Mixes are made loud, so at a low listening volume
    they sound thin. This adds back the bass and top that a quieter level takes away, in
    proportion to how far the system volume is turned down:

      attenuation <= 6 dB     : nothing (around the level mixes are made at)
      each dB below that      : +0.32 dB at 110 Hz and below (max +9 dB),
                                +0.09 dB at 9 kHz and above   (max +3 dB)

    The volume comes from the device: on Windows from the endpoint volume CINEMAX
    already mirrors, on the phone from the media volume. Changes glide (~30 ms per
    control block), so turning the volume never clicks. Allocation-free. */
class VolumeModeler
{
public:
    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;
        low.prepare (fs, FilterType::LowShelf, 110.0, 0.6, 0.0);
        high.prepare (fs, FilterType::HighShelf, 9000.0, 0.7, 0.0);
        reset();
    }

    void reset() noexcept
    {
        low.reset();
        high.reset();
    }

    /** attenuationDb: how far the playback volume is below full (>= 0). */
    void setTarget (bool enabled, float attenuationDb) noexcept
    {
        const float a = enabled ? std::max (0.0f, attenuationDb - 6.0f) : 0.0f;
        bass = std::min (9.0f, 0.32f * a);
        treble = std::min (3.0f, 0.09f * a);
        // Quantised so a volume held still never re-designs the filters.
        bass = std::round (bass * 4.0f) / 4.0f;
        treble = std::round (treble * 4.0f) / 4.0f;
        low.setTarget (110.0, 0.6, bass);
        high.setTarget (9000.0, 0.7, treble);
    }

    void process (float* l, float* r, int n) noexcept
    {
        for (int start = 0; start < n; start += 32)
        {
            const int len = std::min (32, n - start);
            low.advance (len);
            high.advance (len);
            for (auto* f : { &low, &high })
            {
                if (f->isTransparent())
                    continue;
                for (int i = start; i < start + len; ++i)
                {
                    l[i] = f->process (0, l[i]);
                    r[i] = f->process (1, r[i]);
                }
            }
        }
    }

    float bassDb() const noexcept { return bass; }
    float trebleDb() const noexcept { return treble; }

private:
    double fs = 48000.0;
    SmoothedFilter low, high;
    float bass = 0.0f, treble = 0.0f;
};

} // namespace cinemax::dsp
