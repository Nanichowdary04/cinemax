// CinemaxGuard.exe - tiny watchdog that guarantees Windows is never left without a
// working audio output - or with a changed picture - because of CINEMAX. No JUCE.
//
//   --watch <pid>          wait for CINEMAX to exit; if it died while routing was
//                          engaged, hand the default output back to the real device
//   --restore-if-stale     (logon Run entry) restore if routing is engaged but
//                          CINEMAX is not running
//   --restore [--quiet]    manual / uninstaller recovery
#include "cinemax/win/RoutingState.h"
#include "cinemax/win/VisionState.h"

#include <shellapi.h>

using namespace cinemax::win;

namespace
{
    bool hasArg (int argc, wchar_t** argv, const wchar_t* name)
    {
        for (int i = 1; i < argc; ++i)
            if (_wcsicmp (argv[i], name) == 0)
                return true;
        return false;
    }

    const wchar_t* argValue (int argc, wchar_t** argv, const wchar_t* name)
    {
        for (int i = 1; i + 1 < argc; ++i)
            if (_wcsicmp (argv[i], name) == 0)
                return argv[i + 1];
        return nullptr;
    }

    DWORD findCinemaxProcess()
    {
        if (auto st = loadRoutingState(); st && isProcessAlive (st->pid, L"CINEMAX.exe"))
            return st->pid;
        return 0;
    }

    /** Picture: undo CINEMAX Vision's display changes if the process that made them is gone. */
    void restoreVisionIfOrphaned (DWORD pid)
    {
        auto vs = loadVisionState();
        if (! vs || ! vs->engaged)
            return;
        if (pid != 0 && vs->pid != pid && isProcessAlive (vs->pid, L"CINEMAX.exe"))
            return;   // another, running CINEMAX owns them
        log::warn ("CINEMAX Vision display changes left behind - restoring the original picture");
        log::info ("%s", restoreVision (*vs).c_str());
        clearVisionState();
    }

    int watch (DWORD pid)
    {
        HANDLE h = OpenProcess (SYNCHRONIZE, FALSE, pid);
        if (h == nullptr)
        {
            log::warn ("Cannot watch pid %lu", pid);
            return 1;
        }

        log::info ("Watching CINEMAX pid %lu", pid);
        WaitForSingleObject (h, INFINITE);
        CloseHandle (h);
        Sleep (300);

        restoreVisionIfOrphaned (pid);

        auto st = loadRoutingState();
        if (st && st->engaged && st->pid == pid)
        {
            log::warn ("CINEMAX exited while audio routing was engaged - restoring");
            ComScope com;
            const auto r = restoreRouting (*st);
            log::info ("%s", r.message.c_str());
            setLogonRecoveryEntry (false);
            return r.ok ? 0 : 2;
        }

        log::info ("CINEMAX exited cleanly");
        return 0;
    }

    int restoreIfStale (DWORD delayMs)
    {
        // Give an auto-started CINEMAX time to come up and take over.
        Sleep (delayMs);
        restoreVisionIfOrphaned (0);

        auto st = loadRoutingState();
        if (! st || ! st->engaged)
        {
            setLogonRecoveryEntry (false);
            return 0;
        }

        if (isProcessAlive (st->pid, L"CINEMAX.exe"))
            return 0;   // CINEMAX is running and owns the routing

        log::warn ("Stale routing at logon - restoring");
        ComScope com;
        const auto r = restoreRouting (*st);
        log::info ("%s", r.message.c_str());
        setLogonRecoveryEntry (false);
        return r.ok ? 0 : 2;
    }

    int manualRestore (bool quiet)
    {
        if (const DWORD pid = findCinemaxProcess())
        {
            if (! quiet)
            {
                const int answer = MessageBoxW (nullptr,
                    L"CINEMAX is currently running.\n\nClose CINEMAX and restore your normal Windows audio output?",
                    L"CINEMAX audio recovery", MB_YESNO | MB_ICONQUESTION);
                if (answer != IDYES)
                    return 3;
            }

            if (HANDLE h = OpenProcess (PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid))
            {
                TerminateProcess (h, 0);
                WaitForSingleObject (h, 3000);
                CloseHandle (h);
            }
        }

        restoreVisionIfOrphaned (0);

        ComScope com;
        RoutingState st;
        if (auto loaded = loadRoutingState())
            st = *loaded;
        st.engaged = true;

        const auto r = restoreRouting (st);
        setLogonRecoveryEntry (false);
        log::info ("Manual restore: %s", r.message.c_str());

        if (! quiet)
        {
            std::wstring text = fromUtf8 (r.message);
            if (! r.ok)
                text += L"\n\nOpen Settings > System > Sound and choose your speakers or headphones.";
            MessageBoxW (nullptr, text.c_str(), L"CINEMAX audio recovery", MB_OK | (r.ok ? MB_ICONINFORMATION : MB_ICONWARNING));
        }
        return r.ok ? 0 : 2;
    }
}

int WINAPI wWinMain (HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW (GetCommandLineW(), &argc);
    log::init ("guard");

    int result = 0;

    if (const wchar_t* pid = argValue (argc, argv, L"--watch"))
        result = watch ((DWORD) wcstoul (pid, nullptr, 10));
    else if (hasArg (argc, argv, L"--restore-if-stale"))
    {
        const wchar_t* d = argValue (argc, argv, L"--delay");
        result = restoreIfStale (d != nullptr ? (DWORD) wcstoul (d, nullptr, 10) : 20000);
    }
    else if (hasArg (argc, argv, L"--restore"))
        result = manualRestore (hasArg (argc, argv, L"--quiet"));
    else
        result = manualRestore (false);

    LocalFree (argv);
    return result;
}
