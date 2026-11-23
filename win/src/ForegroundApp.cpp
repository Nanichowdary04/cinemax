#include "cinemax/win/ForegroundApp.h"

#include "cinemax/win/AudioSessions.h"

#include <shellapi.h>

#include <cwctype>

namespace cinemax::win
{
namespace
{
    std::wstring lower (std::wstring s)
    {
        for (auto& c : s) c = (wchar_t) std::towlower (c);
        return s;
    }

    std::wstring processPath (DWORD pid)
    {
        HANDLE h = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h == nullptr) return {};
        wchar_t buf[MAX_PATH * 2] {};
        DWORD n = (DWORD) std::size (buf);
        std::wstring path;
        if (QueryFullProcessImageNameW (h, 0, buf, &n)) path.assign (buf, n);
        CloseHandle (h);
        return path;
    }

    std::wstring fileName (const std::wstring& path)
    {
        const auto slash = path.find_last_of (L"\\/");
        return slash == std::wstring::npos ? path : path.substr (slash + 1);
    }

    struct ChildSearch { DWORD hostPid = 0; DWORD found = 0; };

    BOOL CALLBACK findChildProcess (HWND child, LPARAM lp)
    {
        auto* s = reinterpret_cast<ChildSearch*> (lp);
        DWORD pid = 0;
        GetWindowThreadProcessId (child, &pid);
        if (pid != 0 && pid != s->hostPid) { s->found = pid; return FALSE; }
        return TRUE;
    }

    struct Rule { const wchar_t* needle; const char* label; AppCategory cat; };

    // Matched against the lower-case exe name (substring).
    const Rule exeRules[] = {
        // Video players / streaming apps
        { L"vlc.exe", "VLC", AppCategory::Video },
        { L"mpc-hc", "MPC-HC", AppCategory::Video },
        { L"mpc-be", "MPC-BE", AppCategory::Video },
        { L"potplayer", "PotPlayer", AppCategory::Video },
        { L"mpv.exe", "mpv", AppCategory::Video },
        { L"wmplayer.exe", "Windows Media Player", AppCategory::Video },
        { L"microsoft.media.player", "Media Player", AppCategory::Video },
        { L"video.ui.exe", "Films & TV", AppCategory::Video },
        { L"kodi.exe", "Kodi", AppCategory::Video },
        { L"plex", "Plex", AppCategory::Video },
        { L"stremio", "Stremio", AppCategory::Video },
        { L"smplayer", "SMPlayer", AppCategory::Video },
        { L"netflix", "Netflix", AppCategory::Video },
        { L"primevideo", "Prime Video", AppCategory::Video },
        { L"disney", "Disney+", AppCategory::Video },
        { L"hotstar", "Hotstar", AppCategory::Video },
        // Games (well-known titles; plus launcher folders and D3D full screen below)
        { L"valorant", "VALORANT", AppCategory::Game },
        { L"league of legends", "League of Legends", AppCategory::Game },
        { L"cs2.exe", "Counter-Strike 2", AppCategory::Game },
        { L"fortniteclient", "Fortnite", AppCategory::Game },
        { L"gta5", "GTA V", AppCategory::Game },
        { L"r5apex", "Apex Legends", AppCategory::Game },
        { L"dota2", "Dota 2", AppCategory::Game },
        { L"robloxplayer", "Roblox", AppCategory::Game },
        { L"minecraft", "Minecraft", AppCategory::Game },
        { L"eldenring", "Elden Ring", AppCategory::Game },
        { L"cyberpunk2077", "Cyberpunk 2077", AppCategory::Game },
        // Work: editors, IDEs, office, documents, terminals
        { L"code.exe", "VS Code", AppCategory::Work },
        { L"antigravity", "Antigravity", AppCategory::Work },
        { L"cursor.exe", "Cursor", AppCategory::Work },
        { L"devenv.exe", "Visual Studio", AppCategory::Work },
        { L"idea64", "IntelliJ IDEA", AppCategory::Work },
        { L"pycharm", "PyCharm", AppCategory::Work },
        { L"webstorm", "WebStorm", AppCategory::Work },
        { L"clion", "CLion", AppCategory::Work },
        { L"rider", "Rider", AppCategory::Work },
        { L"studio64", "Android Studio", AppCategory::Work },
        { L"sublime_text", "Sublime Text", AppCategory::Work },
        { L"notepad++", "Notepad++", AppCategory::Work },
        { L"notepad.exe", "Notepad", AppCategory::Work },
        { L"winword", "Word", AppCategory::Work },
        { L"excel.exe", "Excel", AppCategory::Work },
        { L"powerpnt", "PowerPoint", AppCategory::Work },
        { L"onenote", "OneNote", AppCategory::Work },
        { L"outlook", "Outlook", AppCategory::Work },
        { L"olk.exe", "Outlook", AppCategory::Work },
        { L"ms-teams", "Teams", AppCategory::Work },
        { L"windowsterminal", "Terminal", AppCategory::Work },
        { L"powershell", "PowerShell", AppCategory::Work },
        { L"pwsh.exe", "PowerShell", AppCategory::Work },
        { L"cmd.exe", "Command Prompt", AppCategory::Work },
        { L"acrobat", "Acrobat", AppCategory::Work },
        { L"acrord32", "Acrobat Reader", AppCategory::Work },
        { L"sumatrapdf", "SumatraPDF", AppCategory::Work },
        { L"obsidian", "Obsidian", AppCategory::Work },
        { L"notion", "Notion", AppCategory::Work },
        { L"claude.exe", "Claude", AppCategory::Work },
        // Music apps
        { L"spotify", "Spotify", AppCategory::Music },
        { L"youtube music", "YouTube Music", AppCategory::Music },
        { L"applemusic", "Apple Music", AppCategory::Music },
        { L"itunes", "iTunes", AppCategory::Music },
        { L"foobar2000", "foobar2000", AppCategory::Music },
        { L"musicbee", "MusicBee", AppCategory::Music },
        { L"aimp", "AIMP", AppCategory::Music },
        { L"winamp", "Winamp", AppCategory::Music },
    };

    const wchar_t* browsers[] = { L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe", L"opera.exe",
                                  L"vivaldi.exe", L"arc.exe", L"zen.exe", L"thorium.exe" };

    // Install folders of game launchers (path substring, lower case).
    const wchar_t* gameFolders[] = { L"\\steamapps\\common\\", L"\\epic games\\", L"\\riot games\\", L"\\xboxgames\\",
                                     L"\\gog galaxy\\games\\", L"\\ea games\\", L"\\ubisoft game launcher\\games\\",
                                     L"\\battle.net\\", L"\\rockstar games\\" };
}

const char* appCategoryKey (AppCategory c)
{
    switch (c)
    {
        case AppCategory::Video:   return "video";
        case AppCategory::Game:    return "game";
        case AppCategory::Work:    return "work";
        case AppCategory::Music:   return "music";
        case AppCategory::Browser: return "browser";
        case AppCategory::None:    break;
    }
    return "none";
}

std::optional<ForegroundInfo> foregroundApp()
{
    HWND w = GetForegroundWindow();
    if (w == nullptr) return std::nullopt;

    ForegroundInfo f;
    GetWindowThreadProcessId (w, &f.pid);
    if (f.pid == 0) return std::nullopt;

    wchar_t title[512] {};
    GetWindowTextW (w, title, (int) std::size (title));
    f.title = title;
    f.exePath = processPath (f.pid);
    f.exe = fileName (f.exePath);

    // UWP apps live inside ApplicationFrameHost: the real app owns a child window.
    if (_wcsicmp (f.exe.c_str(), L"ApplicationFrameHost.exe") == 0)
    {
        ChildSearch s { f.pid, 0 };
        EnumChildWindows (w, findChildProcess, (LPARAM) &s);
        if (s.found != 0)
        {
            f.pid = s.found;
            f.exePath = processPath (s.found);
            f.exe = fileName (f.exePath);
        }
    }

    QUERY_USER_NOTIFICATION_STATE qs {};
    if (SUCCEEDED (SHQueryUserNotificationState (&qs)))
        f.d3dFullscreen = qs == QUNS_RUNNING_D3D_FULL_SCREEN;

    HMONITOR hm = MonitorFromWindow (w, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi {};
    mi.cbSize = sizeof (mi);
    RECT wr {};
    if (hm != nullptr && GetMonitorInfoW (hm, &mi))
    {
        f.monitorGdi = mi.szDevice;
        if (GetWindowRect (w, &wr))
            f.fullscreenWindow = wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top
                              && wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
    }
    return f;
}

AppClass classifyApp (const ForegroundInfo& f)
{
    const auto exe = lower (f.exe);
    const auto path = lower (f.exePath);

    for (auto* b : browsers)
        if (exe == b)
        {
            AppClass c { AppCategory::Browser, toUtf8 (f.exe) };
            // Same title rules as audio Auto: YouTube film, Netflix, Prime Video ...
            AudioSessionInfo s;
            s.exeName = f.exe;
            s.windowTitles = { f.title };
            const auto hint = classifySession (s);
            if (hint.hint == ContentHint::Movie) { c.browserVideo = true; c.label = hint.source; }
            else if (hint.hint == ContentHint::Music) { c.category = AppCategory::Music; c.label = hint.source; }
            return c;
        }

    for (auto& r : exeRules)
        if (exe.find (r.needle) != std::wstring::npos)
            return { r.cat, r.label, false };

    for (auto* g : gameFolders)
        if (path.find (g) != std::wstring::npos)
            return { AppCategory::Game, toUtf8 (f.exe), false };

    if (f.d3dFullscreen)
        return { AppCategory::Game, toUtf8 (f.exe), false };

    return { AppCategory::None, toUtf8 (f.exe), false };
}

} // namespace cinemax::win
