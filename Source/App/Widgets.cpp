#include "Widgets.h"

namespace hype::ui
{
LevelMeter::LevelMeter (int channels) : numChannels (juce::jlimit (1, 2, channels))
{
    setTooltip ("Peak level (dBFS). Click to reset the clip indicator.");
}

float LevelMeter::dbToProportion (float db)
{
    return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
}

void LevelMeter::setLevels (float left, float right, bool clipped)
{
    const float levels[2] { left, right };
    for (int c = 0; c < numChannels; ++c)
    {
        const float db = juce::Decibels::gainToDecibels (levels[c], minDb);
        // Instant attack, ~20 dB/s release at 60 fps.
        displayDb[c] = db > displayDb[c] ? db : juce::jmax (db, displayDb[c] - 0.35f);
        if (db >= holdDb[c]) { holdDb[c] = db; holdFrames[c] = 90; }
        else if (--holdFrames[c] <= 0) holdDb[c] = juce::jmax (minDb, holdDb[c] - 0.5f);
        if (db >= 0.0f) clipped = true;
    }
    if (clipped) clipHold = 120;
    else if (clipHold > 0) --clipHold;
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto clipArea = r.removeFromTop (6.0f);
    r.removeFromTop (2.0f);

    g.setColour (clipHold > 0 ? Palette::danger : Palette::well);
    g.fillRoundedRectangle (clipArea, 1.5f);

    const float gap = 2.0f;
    const float barWidth = (r.getWidth() - gap * (numChannels - 1)) / numChannels;
    const int segments = juce::jmax (8, (int) (r.getHeight() / 4.0f));
    const float segH = r.getHeight() / segments;

    for (int c = 0; c < numChannels; ++c)
    {
        auto bar = juce::Rectangle<float> (r.getX() + c * (barWidth + gap), r.getY(), barWidth, r.getHeight());
        g.setColour (Palette::well);
        g.fillRect (bar);

        const float level = dbToProportion (displayDb[c]);
        for (int s = 0; s < segments; ++s)
        {
            const float segBottom = (float) s / segments;
            if (segBottom >= level)
                break;
            const float segDb = minDb + segBottom * (maxDb - minDb);
            auto colour = segDb > -1.0f ? Palette::meterHigh : segDb > -9.0f ? Palette::meterMid : Palette::meterLow;
            g.setColour (colour);
            g.fillRect (bar.getX(), bar.getBottom() - (s + 1) * segH + 0.5f, bar.getWidth(), segH - 1.0f);
        }

        if (holdDb[c] > minDb + 0.5f)
        {
            const float y = bar.getBottom() - dbToProportion (holdDb[c]) * bar.getHeight();
            g.setColour (Palette::text.withAlpha (0.85f));
            g.fillRect (bar.getX(), y - 1.0f, bar.getWidth(), 2.0f);
        }
    }

    // 0 dB marker
    g.setColour (Palette::textDim.withAlpha (0.6f));
    const float zeroY = r.getBottom() - dbToProportion (0.0f) * r.getHeight();
    g.fillRect (r.getX(), zeroY, r.getWidth(), 1.0f);
}

//==============================================================================
Knob::Knob (const juce::String& captionText, juce::Colour accent, bool bipolar) : caption (captionText)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    slider.getProperties().set (HypeLookAndFeel::bipolarProperty, bipolar);
    slider.setMouseDragSensitivity (180);
    slider.setVelocityBasedMode (false);
    slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    slider.onValueChange = [this] { repaint(); };
    addAndMakeVisible (slider);
}

void Knob::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (14);
    r.removeFromBottom (13);
    slider.setBounds (r);
}

void Knob::paint (juce::Graphics& g)
{
    drawCaption (g, caption, getLocalBounds().removeFromTop (14));
    const auto text = formatValue ? formatValue (slider.getValue()) : juce::String (slider.getValue(), 1);
    g.setColour (Palette::textDim);
    g.setFont (HypeLookAndFeel::font (11.0f));
    g.drawText (text, getLocalBounds().removeFromBottom (13), juce::Justification::centred);
}

juce::String formatTime (double seconds, bool tenths)
{
    seconds = juce::jmax (0.0, seconds);
    const int m = (int) (seconds / 60.0);
    const double s = seconds - m * 60.0;
    if (tenths)
        return juce::String (m).paddedLeft ('0', 2) + ":" + juce::String ((int) s).paddedLeft ('0', 2)
               + "." + juce::String ((int) ((s - (int) s) * 10.0));
    return juce::String (m).paddedLeft ('0', 2) + ":" + juce::String ((int) s).paddedLeft ('0', 2);
}

void drawCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area, juce::Justification j, juce::Colour c)
{
    g.setColour (c);
    g.setFont (HypeLookAndFeel::font (11.0f, true).withExtraKerningFactor (0.08f));
    g.drawText (text.toUpperCase(), area, j);
}
} // namespace hype::ui
