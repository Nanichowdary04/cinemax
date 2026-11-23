#include "cinemax/win/RoutingState.h"

#include <tlhelp32.h>
#include <ctime>
#include <sstream>

namespace cinemax::win
{
namespace
{
    constexpr const wchar_t* runKeyPath       = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr const wchar_t* recoveryValue    = L"CINEMAX Audio Recovery";
    constexpr const wchar_t* launchValue      = L"CINEMAX";

    std::string trim (std::string s)
    {
        while (! s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        return s.substr (i);
    }

    bool setRunValue (const wchar_t* name, const std::wstring* command)
    {
        HKEY key = nullptr;
        if (RegCreateKeyExW (HKEY_CURRENT_USER, runKeyPath, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE,
                             nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;

        LONG r;
        if (command != nullptr)
            r = RegSetValueExW (key, name, 0, REG_SZ, reinterpret_cast<const BYTE*> (command->c_str()),
                                (DWORD) ((command->size() + 1) * sizeof (wchar_t)));
        else
        {
            r = RegDeleteValueW (key, name);
            if (r == ERROR_FILE_NOT_FOUND)
                r = ERROR_SUCCESS;
        }

        RegCloseKey (key);
        return r == ERROR_SUCCESS;
    }
}

//==============================================================================
std::optional<RoutingState> loadRoutingState()
{
    auto data = readFile (paths::routingStateFile());
    if (! data)
        return std::nullopt;

    RoutingState st;
    std::istringstream in (*data);
    std::string line;

    while (std::getline (in, line))
    {
        line = trim (line);
        if (line.empty() || line[0] == '#')
            continue;

        const auto eq = line.find ('=');
        if (eq == std::string::npos)
            continue;

        const auto key = trim (line.substr (0, eq));
        const auto value = trim (line.substr (eq + 1));

        if (key == "engaged")          st.engaged = (value == "1" || value == "true");
        else if (key == "pid")         st.pid = (unsigned long) std::strtoul (value.c_str(), nullptr, 10);
        else if (key == "source")      st.sourceId = fromUtf8 (value);
        else if (key == "restore_to")  st.restoreToId = fromUtf8 (value);
        else if (key == "timestamp")   st.timestamp = std::strtoll (value.c_str(), nullptr, 10);
    }

    return st;
}

bool saveRoutingState (const RoutingState& st)
{
    std::ostringstream out;
    out << "# CINEMAX routing recovery state. Used to restore your Windows audio output\n"
           "# if CINEMAX exits unexpectedly. Safe to delete when CINEMAX is not running.\n";
    out << "engaged=" << (st.engaged ? 1 : 0) << "\n";
    out << "pid=" << st.pid << "\n";
    out << "source=" << toUtf8 (st.sourceId) << "\n";
    out << "restore_to=" << toUtf8 (st.restoreToId) << "\n";
    out << "timestamp=" << (long long) std::time (nullptr) << "\n";
    return writeFileAtomically (paths::routingStateFile(), out.str());
}

bool isProcessAlive (unsigned long pid, const wchar_t* expectedImageName)
{
    if (pid == 0)
        return false;

    HANDLE h = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (h == nullptr)
        return false;

    bool alive = WaitForSingleObject (h, 0) == WAIT_TIMEOUT;

    if (alive && expectedImageName != nullptr)
    {
        wchar_t path[MAX_PATH * 2];
        DWORD size = (DWORD) std::size (path);
        if (QueryFullProcessImageNameW (h, 0, path, &size))
        {
            std::wstring p (path, size);
            const auto slash = p.find_last_of (L"\\/");
            const auto image = slash == std::wstring::npos ? p : p.substr (slash + 1);
            alive = _wcsicmp (image.c_str(), expectedImageName) == 0;
        }
    }

    CloseHandle (h);
    return alive;
}

RestoreResult restoreRouting (const RoutingState& st)
{
    RestoreResult result;

    const auto consoleId = getDefaultRenderId (0);
    const auto mediaId   = getDefaultRenderId (1);

    auto pointsAtVirtual = [&] (const std::wstring& id)
    {
        if (id.empty())
            return false;
        if (! st.sourceId.empty() && _wcsicmp (id.c_str(), st.sourceId.c_str()) == 0)
            return true;

        // Also treat any virtual device as "ours" if no explicit source was recorded.
        if (st.sourceId.empty())
        {
            auto all = enumerateRenderEndpoints (true);
            if (auto e = findEndpoint (all, id))
                return e->isVirtual;
        }
        return false;
    };

    if (! pointsAtVirtual (consoleId) && ! pointsAtVirtual (mediaId))
    {
        result.message = "Windows default output is not the CINEMAX virtual device - nothing to restore.";
        RoutingState cleared = st;
        cleared.engaged = false;
        saveRoutingState (cleared);
        return result;
    }

    const auto all = enumerateRenderEndpoints (true);
    std::vector<std::wstring> prefs;
    if (! st.restoreToId.empty())
        prefs.push_back (st.restoreToId);

    auto target = pickPhysicalOutput (all, prefs);
    if (! target)
    {
        result.ok = false;
        result.message = "No physical playback device is currently available to restore to.";
        return result;
    }

    const HRESULT hr = setDefaultRenderEndpoint (target->id, false);
    if (FAILED (hr))
    {
        result.ok = false;
        result.message = "Could not change the Windows default output: " + hresultToString (hr);
        return result;
    }

    result.changed = true;
    result.deviceName = target->name;
    result.message = "Windows audio output restored to " + toUtf8 (target->name) + ".";

    RoutingState cleared = st;
    cleared.engaged = false;
    saveRoutingState (cleared);
    return result;
}

void setLogonRecoveryEntry (bool enabled)
{
    if (enabled)
    {
        const std::wstring cmd = L"\"" + paths::exeDir() + L"\\CinemaxGuard.exe\" --restore-if-stale";
        setRunValue (recoveryValue, &cmd);
    }
    else
    {
        setRunValue (recoveryValue, nullptr);
    }
}

bool setLaunchOnStartup (bool enabled, const std::wstring& exePath)
{
    if (enabled)
    {
        const std::wstring cmd = L"\"" + exePath + L"\" --tray";
        return setRunValue (launchValue, &cmd);
    }
    return setRunValue (launchValue, nullptr);
}

bool isLaunchOnStartupEnabled()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW (HKEY_CURRENT_USER, runKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;

    const bool exists = RegQueryValueExW (key, launchValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey (key);
    return exists;
}

} // namespace cinemax::win
