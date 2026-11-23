// Intel display-pipe control through the Intel Graphics Control Library
// (ControlLib.dll, installed with Intel graphics drivers). Loads the 3D LUT that
// carries all CINEMAX picture processing, and drives the hardware sharpness filter.
// Applied by the display engine after Windows composes the screen: zero GPU/CPU cost
// per frame, no overlay, works for every app. Loaded at run time; absent = unavailable.
#pragma once

#include "Win32Common.h"

#include <memory>

namespace cinemax::win
{
struct IntelOutputCaps
{
    UINT32 targetId = 0;          // == DISPLAYCONFIG target id
    bool has3DLut = false;
    UINT32 lutBlockId = 0;
    int lutSize = 0;              // samples per channel (17 on Iris Xe)
    bool hasSharpness = false;
    UINT32 sharpFilter = 0;
    float sharpMin = 0, sharpMax = 0;
};

/** HDMI signalling to a TV (info-frames), when the driver and the TV support it. */
struct IntelTvSettings
{
    bool canRange = false, canContentType = false, canLowLatency = false;
    int range = 0;          // 0 default (by video format), 1 limited (16-235), 2 full (0-255)
    int contentType = 0;    // 0 default, 1 disabled, 2 desktop, 3 media, 4 gaming
    int lowLatency = 0;     // 0 default, 1 disabled, 2 enabled (ALLM: TV switches to game mode)
};

struct IntelSharpness
{
    bool enable = false;
    UINT32 filter = 0;
    float intensity = 0;
};

class IntelPipe
{
public:
    IntelPipe();
    ~IntelPipe();

    /** Loads ControlLib.dll and initialises IGCL (once). */
    bool available();

    /** Capabilities of the active output driving this Windows target. */
    std::optional<IntelOutputCaps> caps (UINT32 targetId);

    /** rgb: n*n*n triplets (R outermost) in [0,1]. */
    bool set3DLut (UINT32 targetId, const std::vector<float>& rgb, int n);
    bool restore3DLut (UINT32 targetId);

    std::optional<IntelSharpness> getSharpness (UINT32 targetId);
    bool setSharpness (UINT32 targetId, const IntelSharpness& s);

    /** Level 0 = off, 1..5 mapped onto the filter's hardware range. */
    bool setSharpnessLevel (UINT32 targetId, int level);

    std::optional<IntelTvSettings> getTvSettings (UINT32 targetId);
    /** Values as in IntelTvSettings; -1 leaves a setting alone. Only controllable ones are sent. */
    bool setTvSettings (UINT32 targetId, int range, int contentType, int lowLatency);

    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cinemax::win
