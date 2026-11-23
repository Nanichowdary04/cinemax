// Which apps are playing audio into an endpoint (Windows audio sessions).
// Used by Auto mode: a film player or streaming site is a far more reliable hint
// than sound analysis alone. Nothing here is stored or sent anywhere.
#pragma once

#include "Win32Common.h"

namespace cinemax::win
{
struct AudioSessionInfo
{
    unsigned long pid = 0;
    std::wstring exeName;                 // "vlc.exe"
    std::vector<std::wstring> windowTitles; // visible top-level windows of the app
    float peak = 0.0f;                    // current output peak of this session (0..1)
    bool active = false;                  // AudioSessionStateActive
};

/** Sessions currently rendering to the given endpoint. COM must be initialised. */
std::vector<AudioSessionInfo> listRenderSessions (const std::wstring& endpointId);

enum class ContentHint { None, Music, Movie };

struct AppHint
{
    ContentHint hint = ContentHint::None;
    std::string source;   // short label of the matched app / site, e.g. "VLC", "Netflix"
};

/** Classifies one session from its app and window titles (keyword rules). */
AppHint classifySession (const AudioSessionInfo& s);

/** Hint from the loudest audible session that matches a rule (or None). */
AppHint detectAppHint (const std::vector<AudioSessionInfo>& sessions);

} // namespace cinemax::win
