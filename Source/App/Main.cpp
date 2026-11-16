#include "MainComponent.h"

namespace hype::ui
{
class HypeApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "HYPE DJ PRO"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        const auto args = juce::StringArray::fromTokens (commandLine, true);
        window = std::make_unique<MainWindow>();
        window->main->handleCommandLine (args);
    }

    void shutdown() override
    {
        window.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

private:
    struct MainWindow : public juce::DocumentWindow
    {
        MainWindow() : juce::DocumentWindow ("HYPE DJ PRO", Palette::background, juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            main = new MainComponent();
            setContentOwned (main, true);
            setResizable (true, true);
            setResizeLimits (1280, 800, 4000, 3000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

        MainComponent* main = nullptr;
    };

    HypeLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> window;
};
} // namespace hype::ui

START_JUCE_APPLICATION (hype::ui::HypeApplication)
