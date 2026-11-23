// CINEMAX desktop application entry point.
#include <juce_gui_extra/juce_gui_extra.h>

#include "CinemaxBinaryData.h"
#include "MainWindow.h"
#include "WebBridge.h"
#include "HeadTracker.h"

#include "cinemax/core/Engine.h"
#include "cinemax/vision/VisionEngine.h"
#include "cinemax/win/RoutingState.h"

#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")

namespace cinemax::app
{
//==============================================================================
/** Hidden top-level window that receives WM_QUERYENDSESSION / WM_ENDSESSION so the
    Windows default output is handed back *synchronously* before logoff/shutdown. */
class SessionWatcher
{
public:
    SessionWatcher (std::function<void()> onEnd, std::function<void (const char*)> onDisplay)
        : onSessionEnd (std::move (onEnd)), onDisplayEvent (std::move (onDisplay))
    {
        WNDCLASSW wc {};
        wc.lpfnWndProc = &SessionWatcher::wndProc;
        wc.hInstance = GetModuleHandleW (nullptr);
        wc.lpszClassName = L"CinemaxSessionWatcher";
        RegisterClassW (&wc);

        hwnd = CreateWindowExW (WS_EX_TOOLWINDOW, wc.lpszClassName, L"CINEMAX session watcher", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        if (hwnd != nullptr)
        {
            SetWindowLongPtrW (hwnd, GWLP_USERDATA, (LONG_PTR) this);
            // Display on/off and session unlock: the display driver may have dropped the
            // picture processing, so Vision re-applies it.
            displayNotify = RegisterPowerSettingNotification (hwnd, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
            WTSRegisterSessionNotification (hwnd, NOTIFY_FOR_THIS_SESSION);
        }
    }

    ~SessionWatcher()
    {
        if (displayNotify != nullptr)
            UnregisterPowerSettingNotification (displayNotify);
        if (hwnd != nullptr)
        {
            WTSUnRegisterSessionNotification (hwnd);
            DestroyWindow (hwnd);
        }
    }

private:
    static LRESULT CALLBACK wndProc (HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<SessionWatcher*> (GetWindowLongPtrW (h, GWLP_USERDATA));

        if (msg == WM_QUERYENDSESSION)
            return TRUE;

        if (msg == WM_ENDSESSION && wp != FALSE && self != nullptr && self->onSessionEnd)
        {
            win::log::info ("Session ending - restoring audio routing");
            self->onSessionEnd();
            return 0;
        }
        if (self != nullptr && self->onDisplayEvent)
        {
            if (msg == WM_DISPLAYCHANGE)
                self->onDisplayEvent ("display mode changed");
            else if (msg == WM_POWERBROADCAST && wp == PBT_APMRESUMEAUTOMATIC)
                self->onDisplayEvent ("resumed from sleep");
            else if (msg == WM_POWERBROADCAST && wp == PBT_POWERSETTINGCHANGE && lp != 0)
            {
                auto* ps = reinterpret_cast<const POWERBROADCAST_SETTING*> (lp);
                if (IsEqualGUID (ps->PowerSetting, GUID_CONSOLE_DISPLAY_STATE) && ps->DataLength >= sizeof (DWORD)
                    && *reinterpret_cast<const DWORD*> (ps->Data) == 1)
                    self->onDisplayEvent ("display turned on");
            }
            else if (msg == WM_WTSSESSION_CHANGE && wp == WTS_SESSION_UNLOCK)
                self->onDisplayEvent ("session unlocked");
        }

        return DefWindowProcW (h, msg, wp, lp);
    }

    HWND hwnd = nullptr;
    HPOWERNOTIFY displayNotify = nullptr;
    std::function<void()> onSessionEnd;
    std::function<void (const char*)> onDisplayEvent;
};

//==============================================================================
class CinemaxApplication;

class TrayIcon : public juce::SystemTrayIconComponent
{
public:
    TrayIcon (Engine& e, vision::VisionEngine& v, std::function<void()> open, std::function<void()> quit)
        : engine (e), visionEngine (v), openWindow (std::move (open)), quitApp (std::move (quit))
    {
        juce::MemoryInputStream in (CinemaxBinary::cinemax_icon_png, (size_t) CinemaxBinary::cinemax_icon_pngSize, false);
        auto img = juce::PNGImageFormat().decodeImage (in);
        if (img.isValid())
            setIconImage (img, img);
        refresh();
    }

    void refresh()
    {
        const auto s = engine.snapshot();
        juce::String tip = "CINEMAX - ";
        tip << (s.enabled ? "On" : "Off");
        if (s.enabled)
        {
            for (auto& p : presetCatalog())
                if (p.id == s.params.preset)
                    tip << " | " << p.name;
            if (s.output)
                tip << " | " << toJuce (s.output->info.name);
        }
        setIconTooltip (tip);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isLeftButtonDown() && ! e.mods.isPopupMenu())
        {
            openWindow();
            return;
        }

        juce::Process::makeForegroundProcess();
        const auto s = engine.snapshot();

        juce::PopupMenu presets;
        for (auto& p : presetCatalog())
            presets.addItem (juce::String (p.name), true, s.params.preset == p.id,
                             [this, id = p.id] { engine.applyPreset (id); });

        juce::PopupMenu menu;
        menu.addSectionHeader ("CINEMAX");
        menu.addItem ("Open CINEMAX", [this] { openWindow(); });
        menu.addSeparator();
        menu.addItem ("Audio engine on", true, s.enabled, [this, on = s.enabled] { engine.setEnabled (! on); });
        menu.addItem ("Bypass processing", s.enabled, s.bypass, [this, b = s.bypass] { engine.setBypass (! b); });
        menu.addSeparator();
        const auto mode = s.settings.listeningMode;
        menu.addItem ("Music", s.enabled, mode == ListeningMode::Music, [this] { engine.setListeningMode (ListeningMode::Music); });
        menu.addItem ("Movies", s.enabled, mode == ListeningMode::Movies, [this] { engine.setListeningMode (ListeningMode::Movies); });
        menu.addItem ("Auto (detect music / movies)", s.enabled, mode == ListeningMode::Auto, [this] { engine.setListeningMode (ListeningMode::Auto); });
        menu.addSubMenu ("Preset", presets, s.enabled);
        menu.addSeparator();

        const auto vs = visionEngine.snapshot();
        menu.addItem ("Picture processing on", true, vs.settings.enabled, [this, on = vs.settings.enabled] { visionEngine.setEnabled (! on); });
        if (! vs.displays.empty())
        {
            const auto key = vs.displays.front().info.key;
            juce::PopupMenu pics;
            pics.addItem ("Auto (follow the app in front)", vs.settings.enabled, ! vs.displays.front().profile.manual,
                          [this, key] { visionEngine.setAuto (key); });
            for (auto& p : vision::picturePresetCatalog())
                pics.addItem (juce::String (p.name), vs.settings.enabled, vs.displays.front().activePreset == p.id,
                              [this, key, id = p.id] { visionEngine.selectPreset (key, id); });
            menu.addSubMenu ("Picture", pics, vs.settings.enabled);
        }
        menu.addItem ("Restore original display", [this] { visionEngine.restoreOriginal(); });
        menu.addSeparator();
        menu.addItem ("Restore Windows audio routing", [this] { engine.restoreDefaultRouting(); });
        menu.addItem ("Quit CINEMAX", [this] { quitApp(); });

        menu.showMenuAsync (juce::PopupMenu::Options());
    }

private:
    Engine& engine;
    vision::VisionEngine& visionEngine;
    std::function<void()> openWindow, quitApp;
};

//==============================================================================
class CinemaxApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        win::log::init ("cinemax");
        win::log::info ("CINEMAX %s starting (%s)", JUCE_APPLICATION_VERSION_STRING, commandLine.toRawUTF8());

        engine = std::make_unique<Engine>();
        visionEngine = std::make_unique<vision::VisionEngine>();
        bridge = std::make_unique<WebBridge> (*engine, *visionEngine);

        visionEngine->onStateChanged = [this] { bridge->emitState(); };
        visionEngine->onNotice = [this] (const char* level, const std::string& message) { bridge->emitNotice (level, message); };
        // Picture + sound scenes: a game / video / work app in front also steers the sound.
        visionEngine->onSceneChanged = [this] (vision::Scene scene, const std::string& label)
        {
            if (! visionEngine->snapshot().settings.linkAudio)
            {
                engine->setSceneHint (SceneHint::None, {});
                return;
            }
            const auto hint = scene == vision::Scene::Game  ? SceneHint::Game
                            : scene == vision::Scene::Movie ? SceneHint::Movie
                            : scene == vision::Scene::Work  ? SceneHint::Work
                            : scene == vision::Scene::Music ? SceneHint::Music
                                                            : SceneHint::None;
            engine->setSceneHint (hint, label);
        };

        engine->onStateChanged = [this]
        {
            bridge->emitState();
            if (tray != nullptr)
                tray->refresh();
        };

        engine->onNotice = [this] (const char* level, const std::string& message)
        {
            bridge->emitNotice (level, message);
            // Warnings always; and where the sound now plays (e.g. Bluetooth earbuds connected and
            // Windows switched to them) - otherwise the speakers just go quiet with no explanation.
            const bool outputMoved = message.find ("Now playing through") != std::string::npos;
            const bool important = std::string (level) != "info" || outputMoved;
            if (mainWindow == nullptr && tray != nullptr && important)
                tray->showInfoBubble ("CINEMAX", juce::String (message));
        };

        sessionWatcher = std::make_unique<SessionWatcher> ([this]
        {
            if (visionEngine != nullptr)
                visionEngine->shutdown();
            if (engine != nullptr)
                engine->shutdown();
        },
        [this] (const char* reason)
        {
            // Give the driver a moment to finish its own mode set before re-loading.
            juce::Timer::callAfterDelay (700, [this, why = std::string (reason)]
            {
                if (visionEngine != nullptr)
                    visionEngine->reapplyAll (why.c_str());
            });
        });

        launchGuard();
        engine->initialise();
        visionEngine->initialise();

        // Webcam head tracking runs only while it can be heard (and the user switched it on).
        headTracker = std::make_unique<HeadTracker> (engine->headPoseState());
        bridge->setHeadTracker (headTracker.get());
        trackerTimer.fn = [this]
        {
            if (engine == nullptr || headTracker == nullptr) return;
            const bool want = engine->headTrackingUseful();
            headTracker->setFlip (engine->headTrackingFlip());
            if (want != headTracker->isActive())
                headTracker->setActive (want);
        };
        trackerTimer.startTimer (700);

        tray = std::make_unique<TrayIcon> (*engine, *visionEngine, [this] { showWindow(); }, [this] { quit(); });

        if (! commandLine.contains ("--tray"))
            showWindow();
    }

    void shutdown() override
    {
        trackerTimer.stopTimer();
        if (bridge != nullptr) bridge->setHeadTracker (nullptr);
        headTracker.reset();   // camera off
        mainWindow.reset();
        tray.reset();
        if (visionEngine != nullptr)
            visionEngine->shutdown();   // display back to how it was
        if (engine != nullptr)
            engine->shutdown();
        bridge.reset();
        visionEngine.reset();
        engine.reset();
        sessionWatcher.reset();
        win::log::info ("CINEMAX exited");
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override
    {
        showWindow();
    }

private:
    void showWindow()
    {
        if (mainWindow == nullptr)
            mainWindow = std::make_unique<MainWindow> (*bridge, [this] { closeWindow(); });

        mainWindow->setVisible (true);
        mainWindow->setMinimised (false);
        mainWindow->toFront (true);
    }

    void closeWindow()
    {
        const bool toTray = engine != nullptr && engine->snapshot().settings.minimizeToTray;
        if (toTray)
        {
            mainWindow.reset();   // frees the WebView2 processes; the engine keeps running
            if (tray != nullptr && engine->isEnabled())
                tray->showInfoBubble ("CINEMAX is still running", "Audio processing continues. Right-click the tray icon for options.");
        }
        else
        {
            quit();
        }
    }

    /** Starts CinemaxGuard.exe, which restores the Windows output if this process dies. */
    void launchGuard()
    {
        const std::wstring guard = win::paths::exeDir() + L"\\CinemaxGuard.exe";
        if (GetFileAttributesW (guard.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            win::log::warn ("CinemaxGuard.exe not found next to CINEMAX.exe - crash recovery relies on next launch / logon entry");
            return;
        }

        std::wstring cmd = L"\"" + guard + L"\" --watch " + std::to_wstring (GetCurrentProcessId());
        STARTUPINFOW si { sizeof (si) };
        PROCESS_INFORMATION pi {};
        if (CreateProcessW (nullptr, cmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB,
                            nullptr, nullptr, &si, &pi)
            || CreateProcessW (nullptr, cmd.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
        {
            CloseHandle (pi.hThread);
            CloseHandle (pi.hProcess);
            win::log::info ("Recovery guard started (pid %lu)", pi.dwProcessId);
        }
        else
        {
            win::log::warn ("Could not start recovery guard: %s", win::hresultToString (HRESULT_FROM_WIN32 (GetLastError())).c_str());
        }
    }

    struct FnTimer : juce::Timer
    {
        std::function<void()> fn;
        void timerCallback() override { if (fn) fn(); }
    };

    std::unique_ptr<Engine> engine;
    std::unique_ptr<vision::VisionEngine> visionEngine;
    std::unique_ptr<WebBridge> bridge;
    std::unique_ptr<HeadTracker> headTracker;
    FnTimer trackerTimer;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<TrayIcon> tray;
    std::unique_ptr<SessionWatcher> sessionWatcher;
};

} // namespace cinemax::app

START_JUCE_APPLICATION (cinemax::app::CinemaxApplication)
