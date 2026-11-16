#pragma once

#include "HypeLookAndFeel.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>

namespace hype::ui
{
/**
    Segmented vertical level meter (1 or 2 channels) with peak-hold and clip indicator.
    Feed it linear peak levels via setLevels() at the UI refresh rate; ballistics are applied here.
*/
class LevelMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit LevelMeter (int numChannels = 2);

    void setLevels (float left, float right = 0.0f, bool clipped = false);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { clipHold = 0; repaint(); }

    static constexpr float minDb = -48.0f, maxDb = 3.0f;

private:
    static float dbToProportion (float db);

    const int numChannels;
    float displayDb[2] { minDb, minDb };
    float holdDb[2] { minDb, minDb };
    int holdFrames[2] {};
    int clipHold = 0;
};

/** TextButton that reports press and release separately (CUE, pitch bend). */
class HoldButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    std::function<void()> onPress, onRelease;

    void mouseDown (const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseDown (e);
        if (onPress) onPress();
    }
    void mouseUp (const juce::MouseEvent& e) override
    {
        juce::TextButton::mouseUp (e);
        if (onRelease) onRelease();
    }
};

/** Rotary knob with a caption above and a value read-out below. Double-click resets. */
class Knob : public juce::Component
{
public:
    Knob (const juce::String& caption, juce::Colour accent, bool bipolar);

    juce::Slider slider;
    void resized() override;
    void paint (juce::Graphics&) override;

    std::function<juce::String (double)> formatValue;

private:
    juce::String caption;
};

/** Formats seconds as mm:ss.t */
juce::String formatTime (double seconds, bool tenths = true);

/** Small uppercase section caption. */
void drawCaption (juce::Graphics&, const juce::String&, juce::Rectangle<int>, juce::Justification = juce::Justification::centred,
                  juce::Colour = Palette::textDim);
} // namespace hype::ui
