#pragma once

#include "AudioSettingsComponent.h"
#include "DeckComponent.h"
#include "MixerComponent.h"
#include "UiSelfTest.h"

namespace hype::ui
{
/**
    Top-level HYPE DJ PRO interface: status bar, two scrolling waveforms, Deck A | Mixer | Deck B.
    Owns the audio engine and device controller. A 60 Hz timer pulls state from the engine
    (lock-free reads) and refreshes the display; controls write straight to the engine.
*/
class MainComponent : public juce::Component, private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

    void openAudioSettings();
    void saveSettings();

    /** Developer options: --load-a <file> --load-b <file> --play --screenshot <png> [--settings]. */
    void handleCommandLine (const juce::StringArray& args);

private:
    void timerCallback() override;
    void onCommandLineLoadsFinished();
    void restoreSettings();
    std::unique_ptr<juce::XmlElement> createMixerXml() const;
    void restoreMixerXml (const juce::XmlElement*);

    // The UI owns the engine only through the public API.
    std::unique_ptr<EngineInterface> enginePtr { createEngine() };
    EngineInterface& engine { *enginePtr };
    DeviceControl& deviceController { engine.devices() };

    ScrollingWaveform waveA { engine, 0 }, waveB { engine, 1 };
    DeckComponent deckA { engine, 0 }, deckB { engine, 1 };
    std::unique_ptr<MixerComponent> mixer;
    juce::TextButton settingsButton { "AUDIO SETTINGS" };
    juce::TooltipWindow tooltips { this, 700 };

    std::unique_ptr<juce::PropertiesFile> properties;
    juce::Component::SafePointer<juce::DialogWindow> settingsWindow;
    juce::Component::SafePointer<AudioSettingsComponent> settingsComponent;

    DeviceStatus deviceStatus;
    int pendingLoads = 0;
    bool startSelfTestAfterLoads = false, playAfterLoads = false;
    juce::Rectangle<int> topBar, statusPill;
    int frame = 0;

    std::unique_ptr<UiSelfTest> selfTest;
    juce::File screenshotFile;
    int screenshotAtFrame = -1;
};
} // namespace hype::ui
