// Connects the React UI (WebView2) to the engine. Implements the contract in
// ui/src/bridge/types.ts: native functions, state/notice events and the ~30 Hz
// meter stream. The bridge outlives individual windows (the WebView is destroyed
// when CINEMAX is minimised to the tray).
#pragma once

#include "HeadTracker.h"
#include <juce_gui_extra/juce_gui_extra.h>

#include "cinemax/core/Engine.h"
#include "cinemax/vision/VisionEngine.h"

namespace cinemax::app
{
class WebBridge : private juce::Timer
{
public:
    WebBridge (Engine& engine, vision::VisionEngine& vision);
    ~WebBridge() override;

    /** Options for a new WebBrowserComponent (native functions, resources, WebView2 setup). */
    juce::WebBrowserComponent::Options makeOptions();

    /** Called when a browser is created / destroyed. */
    void attach (juce::WebBrowserComponent* browser);

    void emitState();
    void emitNotice (const char* level, const std::string& message);

    juce::var stateToVar() const;
    juce::var headTrackingToVar() const;

    void setHeadTracker (HeadTracker* t) noexcept { headTracker = t; }

    std::function<void()> onOpenSoundSettings;

private:
    void timerCallback() override;
    void emitMeters();
    static juce::var deviceToVar (const DeviceView& d);

    Engine& engine;
    vision::VisionEngine& visionEngine;
    juce::WebBrowserComponent* browser = nullptr;
    HeadTracker* headTracker = nullptr;
    Analyzer analyzer;
    bool visualizerWanted = false;
};

} // namespace cinemax::app
