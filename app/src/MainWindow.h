#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "WebBridge.h"

namespace cinemax::app
{
/** Hosts the WebView2 UI. Destroyed (with its WebView) when CINEMAX goes to the tray,
    so the hidden app costs no browser processes. */
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (WebBridge& bridge, std::function<void()> onCloseRequested);
    ~MainWindow() override;

    void closeButtonPressed() override;

private:
    void applyNativeChrome();

    class Content : public juce::Component
    {
    public:
        explicit Content (WebBridge& bridge);
        ~Content() override;
        void resized() override;
        void paint (juce::Graphics& g) override;

        WebBridge& bridge;
        std::unique_ptr<juce::WebBrowserComponent> browser;
        juce::String fallbackMessage;
    };

    WebBridge& bridge;
    std::function<void()> onClose;
};

} // namespace cinemax::app
