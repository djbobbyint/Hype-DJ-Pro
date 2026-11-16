#pragma once

#include "Engine/AudioEngine.h"
#include "Widgets.h"

namespace hype::ui
{
/** One mixer channel: GAIN, HI/MID/LOW with kills, headphone CUE, level meter, channel fader. */
class ChannelStrip : public juce::Component
{
public:
    ChannelStrip (Mixer&, Deck&, int deckIndex);

    void updateMeter (float peak);
    void syncFromEngine();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    Mixer& mixer;
    Deck& deck;
    const int index;
    const juce::Colour accent;

    Knob gain, high, mid, low;
    juce::TextButton killHigh { "K" }, killMid { "K" }, killLow { "K" };
    juce::TextButton cueButton;
    juce::Slider fader;
    LevelMeter meter { 1 };
};

/** Centre section: two channel strips, master + headphones, crossfader with curve. */
class MixerComponent : public juce::Component
{
public:
    explicit MixerComponent (AudioEngine&);

    void update (const Mixer::Meters&);
    /** Re-reads every control from the engine (changes made by MIDI, presets, other code...). */
    void syncFromEngine();
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    AudioEngine& engine;
    Mixer& mixer;

    ChannelStrip stripA, stripB;

    Knob master, cueMix, cueLevel, curve;
    LevelMeter masterMeter { 2 }, cueMeter { 2 };
    juce::TextButton splitButton { "SPLIT" }, reverseButton { "REV" };
    juce::Slider crossfader;

    bool limiting = false;
    int updates = 0;
    juce::Rectangle<int> masterArea, phonesArea, xfArea, limitLed;
};
} // namespace hype::ui
