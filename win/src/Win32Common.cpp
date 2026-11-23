#include "cinemax/win/Win32Common.h"

#include <shlobj.h>
#include <knownfolders.h>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace cinemax::win
{
//==============================================================================
std::string toUtf8 (const std::wstring& w)
{
    if (w.empty())
        return {};

    const int len = WideCharToMultiByte (CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string out ((size_t) len, '\0');
    WideCharToMultiByte (CP_UTF8, 0, w.data(), (int) w.size(), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring fromUtf8 (const std::string& s)
{
    if (s.empty())
        return {};

    const int len = MultiByteToWideChar (CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring out ((size_t) len, L'\0');
    MultiByteToWideChar (CP_UTF8, 0, s.data(), (int) s.size(), out.data(), len);
    return out;
}

std::string hresultToString (HRESULT hr)
{
    char buffer[64];
    std::snprintf (buffer, sizeof (buffer), "0x%08lX", (unsigned long) hr);
    std::string text (buffer);

    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                    nullptr, (DWORD) hr, 0, (LPWSTR) &msg, 0, nullptr);
    if (n > 0 && msg != nullptr)
    {
        std::wstring m (msg, n);
        while (! m.empty() && (m.back() == L'\n' || m.back() == L'\r' || m.back() == L' ' || m.back() == L'.'))
            m.pop_back();
        text += " (" + toUtf8 (m) + ")";
    }

    if (msg != nullptr)
        LocalFree (msg);

    return text;
}

//==============================================================================
ComScope::ComScope (DWORD model) noexcept
{
    const HRESULT hr = CoInitializeEx (nullptr, model);
    // RPC_E_CHANGED_MODE: COM already initialised with a different model - still usable.
    succeeded = SUCCEEDED (hr) || hr == RPC_E_CHANGED_MODE;
    needsUninit = SUCCEEDED (hr);
}

ComScope::~ComScope()
{
    if (needsUninit)
        CoUninitialize();
}

//==============================================================================
namespace paths
{
    static std::wstring ensureDir (const std::wstring& dir)
    {
        SHCreateDirectoryExW (nullptr, dir.c_str(), nullptr);
        return dir;
    }

    std::wstring dataDir()
    {
        PWSTR base = nullptr;
        std::wstring result;

        if (SUCCEEDED (SHGetKnownFolderPath (FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &base)) && base != nullptr)
            result = std::wstring (base) + L"\\CINEMAX";
        else
            result = L".\\CINEMAX-data";

        if (base != nullptr)
            CoTaskMemFree (base);

        return ensureDir (result);
    }

    std::wstring logsDir()          { return ensureDir (dataDir() + L"\\logs"); }
    std::wstring profilesDir()      { return ensureDir (dataDir() + L"\\profiles"); }
    std::wstring routingStateFile() { return dataDir() + L"\\routing-state.ini"; }
    std::wstring settingsFile()     { return dataDir() + L"\\settings.json"; }

    std::wstring exePath()
    {
        std::wstring buffer (MAX_PATH, L'\0');

        for (;;)
        {
            const DWORD n = GetModuleFileNameW (nullptr, buffer.data(), (DWORD) buffer.size());
            if (n == 0)
                return {};
            if (n < buffer.size())
            {
                buffer.resize (n);
                return buffer;
            }
            buffer.resize (buffer.size() * 2);
        }
    }

    std::wstring exeDir()
    {
        auto p = exePath();
        const auto slash = p.find_last_of (L"\\/");
        return slash == std::wstring::npos ? std::wstring (L".") : p.substr (0, slash);
    }
}

//==============================================================================
namespace log
{
    namespace
    {
        std::mutex logMutex;
        FILE* logFile = nullptr;
        std::wstring logPath;

        void write (const char* level, const char* fmt, va_list args)
        {
            char message[2048];
            std::vsnprintf (message, sizeof (message), fmt, args);

            SYSTEMTIME st;
            GetLocalTime (&st);

            char line[2300];
            std::snprintf (line, sizeof (line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] (tid %lu) %s\n",
                           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                           level, (unsigned long) GetCurrentThreadId(), message);

            OutputDebugStringA (line);

            std::lock_guard<std::mutex> lock (logMutex);
            if (logFile != nullptr)
            {
                std::fputs (line, logFile);
                std::fflush (logFile);
            }
        }
    }

    void init (const char* component)
    {
        std::lock_guard<std::mutex> lock (logMutex);

        if (logFile != nullptr)
            return;

        logPath = paths::logsDir() + L"\\" + fromUtf8 (component) + L".log";

        // Rotate if larger than 1 MB.
        WIN32_FILE_ATTRIBUTE_DATA attr {};
        if (GetFileAttributesExW (logPath.c_str(), GetFileExInfoStandard, &attr)
             && attr.nFileSizeLow > 1024 * 1024)
        {
            const auto old = logPath + L".old";
            DeleteFileW (old.c_str());
            MoveFileW (logPath.c_str(), old.c_str());
        }

        logFile = _wfsopen (logPath.c_str(), L"a", _SH_DENYNO);
    }

    void info  (const char* fmt, ...) { va_list a; va_start (a, fmt); write ("INFO ", fmt, a); va_end (a); }
    void warn  (const char* fmt, ...) { va_list a; va_start (a, fmt); write ("WARN ", fmt, a); va_end (a); }
    void error (const char* fmt, ...) { va_list a; va_start (a, fmt); write ("ERROR", fmt, a); va_end (a); }

    std::wstring currentLogFile()
    {
        std::lock_guard<std::mutex> lock (logMutex);
        return logPath;
    }
}

//==============================================================================
bool writeFileAtomically (const std::wstring& path, const std::string& contents)
{
    const std::wstring temp = path + L".tmp";

    HANDLE h = CreateFileW (temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    const BOOL ok = WriteFile (h, contents.data(), (DWORD) contents.size(), &written, nullptr);
    FlushFileBuffers (h);
    CloseHandle (h);

    if (! ok || written != contents.size())
    {
        DeleteFileW (temp.c_str());
        return false;
    }

    return MoveFileExW (temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::optional<std::string> readFile (const std::wstring& path)
{
    HANDLE h = CreateFileW (path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::nullopt;

    LARGE_INTEGER size {};
    if (! GetFileSizeEx (h, &size) || size.QuadPart > 16 * 1024 * 1024)
    {
        CloseHandle (h);
        return std::nullopt;
    }

    std::string data ((size_t) size.QuadPart, '\0');
    DWORD read = 0;
    const BOOL ok = data.empty() ? TRUE : ReadFile (h, data.data(), (DWORD) data.size(), &read, nullptr);
    CloseHandle (h);

    if (! ok)
        return std::nullopt;

    data.resize (read);
    return data;
}

std::string stableHash (const std::wstring& text)
{
    unsigned long long h = 1469598103934665603ull;
    for (wchar_t c : text)
    {
        h ^= (unsigned long long) (unsigned short) towlower (c);
        h *= 1099511628211ull;
    }

    char buffer[24];
    std::snprintf (buffer, sizeof (buffer), "%016llx", h);
    return buffer;
}

} // namespace cinemax::win
