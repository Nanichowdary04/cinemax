// Shared Win32 helpers used by the engine, the app and the recovery guard.
// This library deliberately has NO dependency on JUCE so the tiny guard
// process can restore audio routing even if the main app is broken.
#pragma once

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>
#include <optional>
#include <string>
#include <vector>

namespace cinemax::win
{
//==============================================================================
std::string  toUtf8 (const std::wstring& w);
std::wstring fromUtf8 (const std::string& s);
std::string  hresultToString (HRESULT hr);

/** RAII COM initialisation for the calling thread. */
class ComScope
{
public:
    explicit ComScope (DWORD model = COINIT_MULTITHREADED) noexcept;
    ~ComScope();
    bool ok() const noexcept { return succeeded; }

    ComScope (const ComScope&) = delete;
    ComScope& operator= (const ComScope&) = delete;

private:
    bool succeeded = false, needsUninit = false;
};

//==============================================================================
namespace paths
{
    std::wstring dataDir();          // %LOCALAPPDATA%\CINEMAX (created on demand)
    std::wstring logsDir();          // ...\logs
    std::wstring profilesDir();      // ...\profiles
    std::wstring routingStateFile(); // ...\routing-state.ini
    std::wstring settingsFile();     // ...\settings.json
    std::wstring exePath();          // full path of the running executable
    std::wstring exeDir();           // folder of the running executable
}

//==============================================================================
namespace log
{
    /** Opens %LOCALAPPDATA%\CINEMAX\logs\<component>.log (rotated at ~1 MB). */
    void init (const char* component);
    void info  (const char* fmt, ...);
    void warn  (const char* fmt, ...);
    void error (const char* fmt, ...);
    std::wstring currentLogFile();
}

//==============================================================================
/** Writes a file atomically (temp file + MoveFileEx replace). */
bool writeFileAtomically (const std::wstring& path, const std::string& utf8Contents);
std::optional<std::string> readFile (const std::wstring& path);

/** Simple FNV-1a 64-bit hash rendered as hex - used for profile file names. */
std::string stableHash (const std::wstring& text);

} // namespace cinemax::win
