#pragma once

#include <array>
#include <cstdint>

namespace cinemax::dsp
{
/** Up to 7.1.4 (12 channels) from the virtual device. */
constexpr int kMaxInputChannels = 12;

enum class Speaker : uint8_t
{
    FL, FR, FC, LFE, BL, BR, FLC, FRC, BC, SL, SR,
    TC, TFL, TFC, TFR, TBL, TBC, TBR,   // top (height) speakers
    Other
};

/** Input channel layout derived from WAVEFORMATEXTENSIBLE::dwChannelMask. */
struct ChannelLayout
{
    int numChannels = 2;
    std::array<Speaker, kMaxInputChannels> speakers { Speaker::FL, Speaker::FR };

    static ChannelLayout fromMask (uint32_t mask, int channels) noexcept
    {
        ChannelLayout l;
        l.numChannels = channels < 1 ? 1 : (channels > kMaxInputChannels ? kMaxInputChannels : channels);

        if (mask == 0)
        {
            // Defaults when the format carries no mask.
            switch (l.numChannels)
            {
                case 1:  mask = 0x4; break;        // mono -> centre
                case 2:  mask = 0x3; break;
                case 4:  mask = 0x33; break;       // quad
                case 6:  mask = 0x3F; break;       // 5.1 (back)
                case 8:  mask = 0x63F; break;      // 7.1
                case 10: mask = 0x2D03F; break;    // 5.1.4
                case 12: mask = 0x2D63F; break;    // 7.1.4
                default: mask = 0x3; break;
            }
        }

        static constexpr struct { uint32_t bit; Speaker s; } map[] = {
            { 0x1, Speaker::FL }, { 0x2, Speaker::FR }, { 0x4, Speaker::FC }, { 0x8, Speaker::LFE },
            { 0x10, Speaker::BL }, { 0x20, Speaker::BR }, { 0x40, Speaker::FLC }, { 0x80, Speaker::FRC },
            { 0x100, Speaker::BC }, { 0x200, Speaker::SL }, { 0x400, Speaker::SR },
            { 0x800, Speaker::TC }, { 0x1000, Speaker::TFL }, { 0x2000, Speaker::TFC }, { 0x4000, Speaker::TFR },
            { 0x8000, Speaker::TBL }, { 0x10000, Speaker::TBC }, { 0x20000, Speaker::TBR }
        };

        int ch = 0;
        for (auto& m : map)
            if ((mask & m.bit) != 0 && ch < l.numChannels)
                l.speakers[(size_t) ch++] = m.s;

        while (ch < l.numChannels)
            l.speakers[(size_t) ch++] = Speaker::Other;

        // Mono stream: treat as centre.
        if (l.numChannels == 1)
            l.speakers[0] = Speaker::FC;

        return l;
    }

    bool isStereo() const noexcept
    {
        return numChannels == 2 && speakers[0] == Speaker::FL && speakers[1] == Speaker::FR;
    }

    int indexOf (Speaker s) const noexcept
    {
        for (int i = 0; i < numChannels; ++i)
            if (speakers[(size_t) i] == s)
                return i;
        return -1;
    }

    static bool isTop (Speaker s) noexcept
    {
        return s == Speaker::TC || s == Speaker::TFL || s == Speaker::TFC || s == Speaker::TFR
            || s == Speaker::TBL || s == Speaker::TBC || s == Speaker::TBR;
    }

    bool hasTopChannels() const noexcept
    {
        for (int i = 0; i < numChannels; ++i)
            if (isTop (speakers[(size_t) i]))
                return true;
        return false;
    }
};

} // namespace cinemax::dsp
