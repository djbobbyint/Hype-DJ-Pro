#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace hype::ui
{
namespace Palette
{
    const juce::Colour background   { 0xff0a0c0f };
    const juce::Colour panel        { 0xff13161b };
    const juce::Colour panelRaised  { 0xff1a1e25 };
    const juce::Colour well         { 0xff0e1014 };
    const juce::Colour outline      { 0xff272c35 };
    const juce::Colour text         { 0xffe8eaed };
    const juce::Colour textDim      { 0xff8a919c };
    const juce::Colour textFaint    { 0xff545b66 };
    const juce::Colour deckA        { 0xff22c4ef };
    const juce::Colour deckB        { 0xffff8a1f };
    const juce::Colour play         { 0xff2fd36f };
    const juce::Colour cue          { 0xfff4a62a };
    const juce::Colour headphones   { 0xffffc93c };
    const juce::Colour danger       { 0xffef4848 };
    const juce::Colour meterLow     { 0xff2fd36f };
    const juce::Colour meterMid     { 0xffe8c22c };
    const juce::Colour meterHigh    { 0xffef4848 };

    inline juce::Colour forDeck (int deck) { return deck == 0 ? deckA : deckB; }
}

/** Dark, high-contrast theme for club lighting. */
class HypeLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HypeLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override { return 14; }

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& background,
                               bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;

    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override  { return font (14.0f); }
    juce::Font getTextButtonFont (juce::TextButton&, int h) override { return font (juce::jmin (15.0f, h * 0.45f), true); }
    juce::Font getLabelFont (juce::Label& l) override      { return l.getFont(); }

    static juce::Font font (float height, bool bold = false);
    static juce::Font monoFont (float height, bool bold = true);

    /** Slider property: set to true for knobs whose arc starts at the centre (EQ, gain, pitch). */
    static constexpr const char* bipolarProperty = "bipolar";
};
} // namespace hype::ui
