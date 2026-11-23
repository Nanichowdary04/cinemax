// Type placeholders for the desktop UI analyser declared in Metering.h (not built on Android).
#pragma once

#include <cstddef>

namespace juce::dsp
{
class FFT
{
public:
    explicit FFT (int) noexcept {}
};

template <typename T>
class WindowingFunction
{
public:
    enum WindowingMethod { hann };
    WindowingFunction (size_t, WindowingMethod, bool = true) noexcept {}
};
} // namespace juce::dsp
