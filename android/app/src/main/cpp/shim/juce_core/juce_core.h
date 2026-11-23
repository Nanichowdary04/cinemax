// Minimal stand-ins for the few JUCE pieces the shared CINEMAX DSP headers mention.
// The Android build compiles the real engine/src/cinemax/dsp sources unchanged; it
// only needs denormal protection and the type names used by Metering.h.
#pragma once

#include <cstdint>

namespace juce
{
/** Flush denormals to zero for the lifetime of the object (decaying filter tails). */
class ScopedNoDenormals
{
public:
    ScopedNoDenormals() noexcept
    {
#if defined(__aarch64__)
        asm volatile ("mrs %0, fpcr" : "=r"(saved));
        asm volatile ("msr fpcr, %0" ::"r"(saved | (1ull << 24)));   // FZ
#elif defined(__x86_64__) || defined(__i386__)
        asm volatile ("stmxcsr %0" : "=m"(csr));
        unsigned int v = csr | 0x8040u;                               // FTZ | DAZ
        asm volatile ("ldmxcsr %0" ::"m"(v));
#endif
    }
    ~ScopedNoDenormals() noexcept
    {
#if defined(__aarch64__)
        asm volatile ("msr fpcr, %0" ::"r"(saved));
#elif defined(__x86_64__) || defined(__i386__)
        asm volatile ("ldmxcsr %0" ::"m"(csr));
#endif
    }

private:
#if defined(__aarch64__)
    uint64_t saved = 0;
#elif defined(__x86_64__) || defined(__i386__)
    unsigned int csr = 0;
#endif
};

/** Type placeholder: the Android MeterBus (meterbus_android.cpp) does not use a FIFO. */
class AbstractFifo
{
public:
    explicit AbstractFifo (int) noexcept {}
};
} // namespace juce
