// What the user is looking at: the foreground app (process name + window title)
// and whether a Direct3D game owns the screen. Used by Auto picture mode. Only
// process/window information is read - no screen capture of any kind.
#pragma once

#include "Win32Common.h"

namespace cinemax::win
{
struct ForegroundInfo
{
    DWORD pid = 0;
    std::wstring exe;          // "Code.exe" (UWP apps resolved behind ApplicationFrameHost)
    std::wstring exePath;
    std::wstring title;
    std::wstring monitorGdi;   // \\.\DISPLAY1 - display the window is on
    bool d3dFullscreen = false;     // Windows: a Direct3D app is running full screen
    bool fullscreenWindow = false;  // the window covers its whole monitor
};

std::optional<ForegroundInfo> foregroundApp();

enum class AppCategory { None, Video, Game, Work, Music, Browser };
const char* appCategoryKey (AppCategory c);

struct AppClass
{
    AppCategory category = AppCategory::None;
    std::string label;          // "VLC", "VS Code", "YouTube: movie", "VALORANT"
    bool browserVideo = false;  // a browser whose title shows a video site / film
};

/** Classifies the foreground app (games, video players, browsers + video sites,
    work apps, music apps). Title rules are shared with audio Auto. */
AppClass classifyApp (const ForegroundInfo& f);

} // namespace cinemax::win
