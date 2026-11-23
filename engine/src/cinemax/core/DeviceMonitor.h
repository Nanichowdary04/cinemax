#pragma once

#include <functional>
#include <memory>
#include <string>

namespace cinemax
{
/** Watches Windows audio endpoint changes (IMMNotificationClient). Callbacks are
    forwarded to the JUCE message thread; the COM callback thread is never blocked. */
class DeviceMonitor
{
public:
    enum class Kind { Added, Removed, StateChanged, DefaultChanged, FormatChanged };

    struct Event
    {
        Kind kind;
        std::wstring deviceId;
        unsigned long state = 0;   // for StateChanged
        int role = -1;             // for DefaultChanged (ERole)
    };

    DeviceMonitor();
    ~DeviceMonitor();

    /** Message thread. 'handler' is invoked on the message thread. */
    bool start (std::function<void (const Event&)> handler);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace cinemax
