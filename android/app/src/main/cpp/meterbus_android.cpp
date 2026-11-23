// Android MeterBus: the audio thread's meter values are read through JNI (stats);
// the desktop spectrum/waveform hand-off is not needed on the phone.
#include "cinemax/core/Metering.h"

namespace cinemax
{
MeterBus::MeterBus() = default;
void MeterBus::pushAnalysis (const float*, const float*, int) noexcept {}
int MeterBus::readAnalysis (float*, int) noexcept { return 0; }
void MeterBus::resetForNewStream (double rate) noexcept { sampleRate.store (rate, std::memory_order_relaxed); }
} // namespace cinemax
