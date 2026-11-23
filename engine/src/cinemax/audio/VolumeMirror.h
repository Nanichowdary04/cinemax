// Keeps Windows volume control natural while CINEMAX is routed:
//  - The Windows volume flyout / keys now control the virtual device. Its level is
//    mirrored onto the real output device (hardware / Bluetooth absolute volume),
//    and the real device's own changes (e.g. earbud buttons) are mirrored back.
//  - WASAPI loopback of a software-volume endpoint is post-volume, so the captured
//    signal is attenuated by the virtual device's volume. That attenuation is undone
//    before DSP (input gain) so the real device applies the volume exactly once and
//    dynamics/loudness processing never fights the user's volume setting.
//  - Because the real device always carries the current volume, turning CINEMAX off
//    never causes a sudden jump in loudness.
//  - A device switch never makes it louder: the new device gets the level the user was
//    listening at (or its own, if lower), and for a few seconds after a switch the device
//    can't raise its own volume (Bluetooth earbuds push theirs right after connecting).
#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace cinemax::audio
{
class VolumeMirror
{
public:
    /** deviceVolumeDb (optional) receives the real output's volume in dB (<= 0, -96 when
        muted, 0 when unknown): how far the listening level is below full, for the
        volume-aware tone. */
    explicit VolumeMirror (std::atomic<float>& inputGain, std::atomic<float>* deviceVolumeDb = nullptr);
    ~VolumeMirror();

    /** Message thread. Copies the output's current volume onto the source, then mirrors. */
    bool start (const std::wstring& sourceId, const std::wstring& outputId, bool compensate);
    void stop();
    void setCompensate (bool shouldCompensate);

    bool sourceUsesSoftwareVolume() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cinemax::audio
