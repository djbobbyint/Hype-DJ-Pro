#pragma once

#include "Engine/AudioDeviceController.h"
#include "Widgets.h"

namespace hype::ui
{
/**
    Audio settings panel: driver type, device, sample rate, buffer size, active output channels,
    master and headphone output assignment, device refresh and fallback behaviour. Shows the
    current device, active sample rate, latency and connection state. Changes apply immediately.
*/
class AudioSettingsComponent : public juce::Component
{
public:
    explicit AudioSettingsComponent (AudioDeviceController&);

    /** Re-reads everything from the controller (call when the device state changes). */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void()> onSettingsChanged;   // e.g. persist

private:
    void applyChange (const std::function<void (AudioDeviceController::Settings&)>& change);
    void fillPairCombo (juce::ComboBox&, int selectedFirstChannel, const juce::StringArray& channelNames);
    void rebuildChannelToggles (const juce::StringArray& names, const juce::BigInteger& active);

    AudioDeviceController& controller;
    bool updating = false;

    juce::ComboBox typeBox, deviceBox, rateBox, bufferBox, masterBox, phonesBox;
    juce::TextButton refreshButton { "REFRESH" };
    juce::ToggleButton fallbackToggle { "If the device is lost, continue on the system default device" };
    juce::OwnedArray<juce::ToggleButton> channelToggles;
    juce::Component channelHolder;
    juce::Viewport channelViewport;

    juce::Rectangle<int> statusArea;
};
} // namespace hype::ui
