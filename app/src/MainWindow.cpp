#include "MainWindow.h"

#include "CinemaxBinaryData.h"

#include <windows.h>
#include <dwmapi.h>

namespace cinemax::app
{
MainWindow::Content::Content (WebBridge& b) : bridge (b)
{
    auto options = bridge.makeOptions();

    if (juce::WebBrowserComponent::areOptionsSupported (options))
    {
        browser = std::make_unique<juce::WebBrowserComponent> (options);
        addAndMakeVisible (*browser);
        bridge.attach (browser.get());
        browser->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    }
    else
    {
        fallbackMessage = "The Microsoft Edge WebView2 Runtime is required for the CINEMAX interface.\n"
                          "Install it from https://developer.microsoft.com/microsoft-edge/webview2/ and restart CINEMAX.\n"
                          "Audio processing keeps working from the tray icon.";
    }
}

MainWindow::Content::~Content()
{
    bridge.attach (nullptr);
}

void MainWindow::Content::resized()
{
    if (browser != nullptr)
        browser->setBounds (getLocalBounds());
}

void MainWindow::Content::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0a0a0b));
    if (fallbackMessage.isNotEmpty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.setFont (juce::FontOptions (16.0f));
        g.drawFittedText (fallbackMessage, getLocalBounds().reduced (40), juce::Justification::centred, 6);
    }
}

//==============================================================================
MainWindow::MainWindow (WebBridge& b, std::function<void()> onCloseRequested)
    : juce::DocumentWindow ("CINEMAX", juce::Colour (0xff0a0a0b), juce::DocumentWindow::allButtons),
      bridge (b), onClose (std::move (onCloseRequested))
{
    setUsingNativeTitleBar (true);
    setResizable (true, false);
    setResizeLimits (960, 640, 4096, 4096);
    setContentOwned (new Content (bridge), false);
    centreWithSize (1180, 760);

    {
        juce::MemoryInputStream in (CinemaxBinary::cinemax_icon_png, (size_t) CinemaxBinary::cinemax_icon_pngSize, false);
        if (auto img = juce::PNGImageFormat().decodeImage (in); img.isValid())
            setIcon (img);
    }

    setVisible (true);
    applyNativeChrome();
}

MainWindow::~MainWindow()
{
    clearContentComponent();
}

void MainWindow::closeButtonPressed()
{
    if (onClose)
        onClose();
}

void MainWindow::applyNativeChrome()
{
    auto* peer = getPeer();
    if (peer == nullptr)
        return;

    auto hwnd = static_cast<HWND> (peer->getNativeHandle());

    // Dark title bar + caption colour matching the app background (Windows 11).
    const BOOL dark = TRUE;
    DwmSetWindowAttribute (hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof (dark));
    const COLORREF caption = RGB (10, 10, 11);
    DwmSetWindowAttribute (hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof (caption));
    const COLORREF text = RGB (235, 235, 235);
    DwmSetWindowAttribute (hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof (text));
}

} // namespace cinemax::app
