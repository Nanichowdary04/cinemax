// Crash-safe record of what CINEMAX changed in the Windows audio routing, so the
// guard process (or the next app launch, or the next logon) can put it back.
#pragma once

#include "Endpoints.h"

namespace cinemax::win
{
struct RoutingState
{
    bool engaged = false;           // Windows default output currently points at the virtual source
    unsigned long pid = 0;          // process that engaged the routing
    std::wstring sourceId;          // virtual device CINEMAX made the default
    std::wstring restoreToId;       // real device to hand the default back to
    long long timestamp = 0;        // unix seconds of last update
};

std::optional<RoutingState> loadRoutingState();
bool saveRoutingState (const RoutingState& state);

/** True if a process with this id exists and its image name matches (case-insensitive). */
bool isProcessAlive (unsigned long pid, const wchar_t* expectedImageName);

struct RestoreResult
{
    bool changed = false;           // the default device was changed back
    bool ok = true;                 // no error happened
    std::wstring deviceName;
    std::string message;
};

/** Hands the Windows default output back to a real device - but only if the
    default (console or multimedia) currently points at the virtual source, so
    a device the user picked in the meantime is never overridden.
    Clears the engaged flag on success. COM must be initialised. */
RestoreResult restoreRouting (const RoutingState& state);

/** HKCU Run entry that runs "CinemaxGuard.exe --restore-if-stale" at logon while
    routing is engaged (covers power loss / BSOD while CINEMAX was running). */
void setLogonRecoveryEntry (bool enabled);

/** HKCU Run entry for "launch on startup". */
bool setLaunchOnStartup (bool enabled, const std::wstring& exePath);
bool isLaunchOnStartupEnabled();

} // namespace cinemax::win
