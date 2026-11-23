#include "cinemax/win/AudioSessions.h"

#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace cinemax::win
{
namespace
{
    std::wstring lower (std::wstring s)
    {
        std::transform (s.begin(), s.end(), s.begin(), [] (wchar_t c) { return (wchar_t) std::towlower (c); });
        return s;
    }

    std::wstring imageName (DWORD pid)
    {
        std::wstring result;
        if (HANDLE h = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
        {
            wchar_t path[MAX_PATH * 2];
            DWORD size = (DWORD) std::size (path);
            if (QueryFullProcessImageNameW (h, 0, path, &size))
            {
                std::wstring p (path, size);
                const auto slash = p.find_last_of (L"\\/");
                result = slash == std::wstring::npos ? p : p.substr (slash + 1);
            }
            CloseHandle (h);
        }
        return result;
    }

    struct WindowSearch
    {
        std::vector<DWORD> pids;
        std::vector<std::wstring>* titles;
    };

    BOOL CALLBACK collectWindow (HWND hwnd, LPARAM lp)
    {
        auto* ws = reinterpret_cast<WindowSearch*> (lp);
        if (! IsWindowVisible (hwnd) || GetWindow (hwnd, GW_OWNER) != nullptr)
            return TRUE;

        DWORD pid = 0;
        GetWindowThreadProcessId (hwnd, &pid);
        if (std::find (ws->pids.begin(), ws->pids.end(), pid) == ws->pids.end())
            return TRUE;

        wchar_t title[512];
        const int n = GetWindowTextW (hwnd, title, (int) std::size (title));
        if (n > 0 && ws->titles->size() < 16)
            ws->titles->emplace_back (title, (size_t) n);
        return TRUE;
    }

    /** Browsers play audio from a helper process without windows; look at every
        process with the same image name (all chrome.exe / msedge.exe ...). */
    std::vector<DWORD> relatedPids (DWORD pid, const std::wstring& exe)
    {
        std::vector<DWORD> pids { pid };
        if (exe.empty())
            return pids;

        HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return pids;

        PROCESSENTRY32W pe { sizeof (pe) };
        for (BOOL ok = Process32FirstW (snap, &pe); ok; ok = Process32NextW (snap, &pe))
            if (pe.th32ProcessID != pid && _wcsicmp (pe.szExeFile, exe.c_str()) == 0)
                pids.push_back (pe.th32ProcessID);

        CloseHandle (snap);
        return pids;
    }

    struct Rule { const wchar_t* needle; const char* label; ContentHint hint; };

    // Dedicated apps (matched against the executable name). YouTube / YouTube Music
    // apps are NOT here: they play both songs and films, so their title decides.
    const Rule musicApps[] = {
        { L"spotify",       "Spotify",        ContentHint::Music },
        { L"applemusic",    "Apple Music",    ContentHint::Music },
        { L"itunes",        "iTunes",         ContentHint::Music },
        { L"amazon music",  "Amazon Music",   ContentHint::Music },
        { L"tidal",         "TIDAL",          ContentHint::Music },
        { L"deezer",        "Deezer",         ContentHint::Music },
        { L"musicbee",      "MusicBee",       ContentHint::Music },
        { L"foobar2000",    "foobar2000",     ContentHint::Music },
        { L"aimp",          "AIMP",           ContentHint::Music },
        { L"winamp",        "Winamp",         ContentHint::Music },
    };

    // Video players: a film unless the window shows an audio file.
    const Rule playerApps[] = {
        { L"vlc",           "VLC",            ContentHint::Movie },
        { L"mpc-hc",        "MPC-HC",         ContentHint::Movie },
        { L"mpc-be",        "MPC-BE",         ContentHint::Movie },
        { L"potplayer",     "PotPlayer",      ContentHint::Movie },
        { L"mpv",           "mpv",            ContentHint::Movie },
        { L"kodi",          "Kodi",           ContentHint::Movie },
        { L"plex",          "Plex",           ContentHint::Movie },
        { L"stremio",       "Stremio",        ContentHint::Movie },
        { L"video.ui",      "Movies & TV",    ContentHint::Movie },
        { L"primevideo",    "Prime Video",    ContentHint::Movie },
        { L"netflix",       "Netflix",        ContentHint::Movie },
        { L"disney",        "Disney+",        ContentHint::Movie },
    };

    const wchar_t* audioFileExts[] = { L".mp3", L".flac", L".m4a", L".wav", L".aac", L".ogg", L".opus", L".wma", L".alac" };

    // Window titles (browser tabs, YouTube, YouTube Music app ...). ORDER MATTERS:
    // specific phrases first, general words last; the first match wins.
    const Rule titleRules[] = {
        // --- specific film phrases ----------------------------------------------------------
        { L"full movie",    "movie",          ContentHint::Movie },
        { L"movie scene",   "movie",          ContentHint::Movie },
        { L"dubbed",        "movie",          ContentHint::Movie },
        { L"web series",    "web series",     ContentHint::Movie },
        { L"full film",     "film",           ContentHint::Movie },
        { L"short film",    "film",           ContentHint::Movie },
        { L"hdrip",         "movie",          ContentHint::Movie },
        { L"webrip",        "movie",          ContentHint::Movie },
        { L"web-dl",        "movie",          ContentHint::Movie },
        { L"bluray",        "movie",          ContentHint::Movie },
        { L"blu-ray",       "movie",          ContentHint::Movie },
        { L"brrip",         "movie",          ContentHint::Movie },
        { L"dvdrip",        "movie",          ContentHint::Movie },
        { L"hdtv",          "movie",          ContentHint::Movie },
        // --- specific music phrases ---------------------------------------------------------
        { L"video song",    "song",           ContentHint::Music },
        { L"full song",     "song",           ContentHint::Music },
        { L"audio song",    "song",           ContentHint::Music },
        { L"lyrical",       "song",           ContentHint::Music },
        { L"lyric video",   "song",           ContentHint::Music },
        { L"lyrics",        "song",           ContentHint::Music },
        { L"jukebox",       "music",          ContentHint::Music },
        { L"official audio","music",          ContentHint::Music },
        { L"music video",   "music",          ContentHint::Music },
        { L"remix",         "music",          ContentHint::Music },
        { L"unplugged",     "music",          ContentHint::Music },
        { L"mashup",        "music",          ContentHint::Music },
        { L"lofi",          "music",          ContentHint::Music },
        { L"lo-fi",         "music",          ContentHint::Music },
        // --- streaming services -----------------------------------------------------------------
        { L"netflix",       "Netflix",        ContentHint::Movie },
        { L"prime video",   "Prime Video",    ContentHint::Movie },
        { L"disney+",       "Disney+",        ContentHint::Movie },
        { L"hotstar",       "Hotstar",        ContentHint::Movie },
        { L"jiocinema",     "JioCinema",      ContentHint::Movie },
        { L"hulu",          "Hulu",           ContentHint::Movie },
        { L"hbo",           "HBO",            ContentHint::Movie },
        { L"apple tv",      "Apple TV",       ContentHint::Movie },
        { L"crunchyroll",   "Crunchyroll",    ContentHint::Movie },
        { L"zee5",          "ZEE5",           ContentHint::Movie },
        { L"sonyliv",       "SonyLIV",        ContentHint::Movie },
        { L"mx player",     "MX Player",      ContentHint::Movie },
        { L"mubi",          "MUBI",           ContentHint::Movie },
        { L"spotify",       "Spotify",        ContentHint::Music },
        { L"soundcloud",    "SoundCloud",     ContentHint::Music },
        { L"apple music",   "Apple Music",    ContentHint::Music },
        { L"amazon music",  "Amazon Music",   ContentHint::Music },
        { L"jiosaavn",      "JioSaavn",       ContentHint::Music },
        { L"gaana",         "Gaana",          ContentHint::Music },
        { L"wynk",          "Wynk",           ContentHint::Music },
        { L"deezer",        "Deezer",         ContentHint::Music },
        // --- general words ------------------------------------------------------------------
        { L"trailer",       "trailer",        ContentHint::Movie },
        { L"teaser",        "trailer",        ContentHint::Movie },
        { L"episode",       "episode",        ContentHint::Movie },
        { L"season",        "series",         ContentHint::Movie },
        { L"scene",         "movie",          ContentHint::Movie },
        { L"movie",         "movie",          ContentHint::Movie },
        { L"film",          "film",           ContentHint::Movie },
        { L"2160p",         "movie",          ContentHint::Movie },
        { L"1080p",         "movie",          ContentHint::Movie },
        { L"720p",          "movie",          ContentHint::Movie },
        { L"480p",          "movie",          ContentHint::Movie },
        { L"song",          "song",           ContentHint::Music },
        { L"album",         "music",          ContentHint::Music },
        { L"playlist",      "music",          ContentHint::Music },
        { L"ft.",           "song",           ContentHint::Music },
        { L"feat.",         "song",           ContentHint::Music },
        { L"music",         "music",          ContentHint::Music },   // also "| YouTube Music" titles
    };

    const char* siteLabel (const std::wstring& titleLower)
    {
        if (titleLower.find (L"youtube music") != std::wstring::npos) return "YouTube Music";
        if (titleLower.find (L"youtube") != std::wstring::npos)       return "YouTube";
        return nullptr;
    }
}

std::vector<AudioSessionInfo> listRenderSessions (const std::wstring& endpointId)
{
    std::vector<AudioSessionInfo> result;

    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioSessionManager2> mgr;
    ComPtr<IAudioSessionEnumerator> sessions;

    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&en)))
        || FAILED (en->GetDevice (endpointId.c_str(), &dev))
        || FAILED (dev->Activate (__uuidof (IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**> (mgr.GetAddressOf())))
        || FAILED (mgr->GetSessionEnumerator (&sessions)))
        return result;

    int count = 0;
    sessions->GetCount (&count);

    for (int i = 0; i < count; ++i)
    {
        ComPtr<IAudioSessionControl> ctl;
        ComPtr<IAudioSessionControl2> ctl2;
        if (FAILED (sessions->GetSession (i, &ctl)) || FAILED (ctl.As (&ctl2)))
            continue;
        if (ctl2->IsSystemSoundsSession() == S_OK)
            continue;

        AudioSessionInfo info;
        DWORD pid = 0;
        ctl2->GetProcessId (&pid);
        info.pid = pid;

        AudioSessionState state = AudioSessionStateInactive;
        ctl2->GetState (&state);
        info.active = state == AudioSessionStateActive;

        ComPtr<IAudioMeterInformation> meter;
        if (SUCCEEDED (ctl.As (&meter)))
            meter->GetPeakValue (&info.peak);

        if (pid == 0 || pid == GetCurrentProcessId())
            continue;

        info.exeName = imageName (pid);

        WindowSearch ws { relatedPids (pid, info.exeName), &info.windowTitles };
        EnumWindows (collectWindow, reinterpret_cast<LPARAM> (&ws));

        result.push_back (std::move (info));
    }

    return result;
}

AppHint classifySession (const AudioSessionInfo& s)
{
    const auto exe = lower (s.exeName);

    // 1. Dedicated music apps.
    for (auto& r : musicApps)
        if (exe.find (r.needle) != std::wstring::npos)
            return { r.hint, r.label };

    // 2. Video players: film, unless they are playing an audio file.
    for (auto& r : playerApps)
        if (exe.find (r.needle) != std::wstring::npos)
        {
            for (auto& title : s.windowTitles)
            {
                const auto t = lower (title);
                for (auto* ext : audioFileExts)
                    if (t.find (ext) != std::wstring::npos)
                        return { ContentHint::Music, r.label };
            }
            return { r.hint, r.label };
        }

    // 3. Everything else (browsers, YouTube / YouTube Music apps ...): what the title says.
    //    Rules are checked in priority order across all of the app's window titles.
    std::vector<std::wstring> titles;
    titles.reserve (s.windowTitles.size());
    for (auto& title : s.windowTitles)
        titles.push_back (lower (title));

    for (auto& r : titleRules)
        for (auto& t : titles)
            if (t.find (r.needle) != std::wstring::npos)
            {
                std::string label = r.label;
                if (const char* site = siteLabel (t))
                    label = std::string (site) + ": " + label;   // e.g. "YouTube: movie"
                return { r.hint, label };
            }

    return {};
}

AppHint detectAppHint (const std::vector<AudioSessionInfo>& sessions)
{
    AppHint best;
    float bestPeak = 0.0f;
    for (auto& s : sessions)
    {
        if (! s.active || s.peak < 0.001f)   // not audibly playing right now
            continue;
        const auto h = classifySession (s);
        if (h.hint != ContentHint::None && s.peak > bestPeak)
        {
            best = h;
            bestPeak = s.peak;
        }
    }
    return best;
}

} // namespace cinemax::win
