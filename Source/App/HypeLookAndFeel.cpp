#include "HypeLookAndFeel.h"

namespace hype::ui
{
HypeLookAndFeel::HypeLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, Palette::background);
    setColour (juce::DocumentWindow::backgroundColourId, Palette::background);
    setColour (juce::Label::textColourId, Palette::text);
    setColour (juce::TextButton::buttonColourId, Palette::panelRaised);
    setColour (juce::TextButton::buttonOnColourId, Palette::deckA);
    setColour (juce::TextButton::textColourOffId, Palette::text);
    setColour (juce::TextButton::textColourOnId, Palette::background);
    setColour (juce::ComboBox::backgroundColourId, Palette::well);
    setColour (juce::ComboBox::outlineColourId, Palette::outline);
    setColour (juce::ComboBox::textColourId, Palette::text);
    setColour (juce::ComboBox::arrowColourId, Palette::textDim);
    setColour (juce::PopupMenu::backgroundColourId, Palette::panelRaised);
    setColour (juce::PopupMenu::textColourId, Palette::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::deckA.withAlpha (0.25f));
    setColour (juce::PopupMenu::highlightedTextColourId, Palette::text);
    setColour (juce::ToggleButton::textColourId, Palette::text);
    setColour (juce::ToggleButton::tickColourId, Palette::deckA);
    setColour (juce::ToggleButton::tickDisabledColourId, Palette::textFaint);
    setColour (juce::Slider::rotarySliderFillColourId, Palette::deckA);
    setColour (juce::Slider::rotarySliderOutlineColourId, Palette::outline);
    setColour (juce::Slider::thumbColourId, Palette::text);
    setColour (juce::Slider::trackColourId, Palette::deckA);
    setColour (juce::Slider::backgroundColourId, Palette::well);
    setColour (juce::Slider::textBoxTextColourId, Palette::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ScrollBar::thumbColourId, Palette::outline);
    setColour (juce::AlertWindow::backgroundColourId, Palette::panelRaised);
    setColour (juce::AlertWindow::textColourId, Palette::text);
    setColour (juce::AlertWindow::outlineColourId, Palette::outline);
    setColour (juce::TooltipWindow::backgroundColourId, Palette::panelRaised);
    setColour (juce::TooltipWindow::textColourId, Palette::text);
    setColour (juce::TooltipWindow::outlineColourId, Palette::outline);
    setColour (juce::CaretComponent::caretColourId, Palette::deckA);
}

juce::Font HypeLookAndFeel::font (float height, bool bold)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Font HypeLookAndFeel::monoFont (float height, bool bold)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height,
                                          bold ? juce::Font::bold : juce::Font::plain));
}

void HypeLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                        float startAngle, float endAngle, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (3.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float trackWidth = juce::jmax (2.5f, radius * 0.14f);
    const float arcRadius = radius - trackWidth * 0.5f;
    const auto accent = s.findColour (juce::Slider::rotarySliderFillColourId);
    const bool bipolar = (bool) s.getProperties()[bipolarProperty];
    const float angle = startAngle + pos * (endAngle - startAngle);

    // Track
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (Palette::outline);
    g.strokePath (track, juce::PathStrokeType (trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc
    const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
    if (std::abs (angle - from) > 0.01f)
    {
        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
        g.setColour (s.isEnabled() ? accent : Palette::textFaint);
        g.strokePath (arc, juce::PathStrokeType (trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Knob body
    const float bodyRadius = arcRadius - trackWidth * 1.1f;
    g.setGradientFill (juce::ColourGradient (Palette::panelRaised.brighter (0.15f), centre.x, centre.y - bodyRadius,
                                             Palette::well, centre.x, centre.y + bodyRadius, false));
    g.fillEllipse (centre.x - bodyRadius, centre.y - bodyRadius, bodyRadius * 2.0f, bodyRadius * 2.0f);
    g.setColour (Palette::outline.brighter (0.2f));
    g.drawEllipse (centre.x - bodyRadius, centre.y - bodyRadius, bodyRadius * 2.0f, bodyRadius * 2.0f, 1.0f);

    // Pointer
    const auto tip = centre.getPointOnCircumference (bodyRadius * 0.85f, angle);
    const auto base = centre.getPointOnCircumference (bodyRadius * 0.25f, angle);
    g.setColour (Palette::text);
    g.drawLine ({ base, tip }, juce::jmax (2.0f, radius * 0.09f));
}

void HypeLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                        juce::Slider::SliderStyle style, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat();
    const bool vertical = style == juce::Slider::LinearVertical;
    const auto accent = s.findColour (juce::Slider::trackColourId);

    // Groove
    const float grooveThickness = 6.0f;
    juce::Rectangle<float> groove = vertical
        ? juce::Rectangle<float> (bounds.getCentreX() - grooveThickness * 0.5f, bounds.getY(), grooveThickness, bounds.getHeight())
        : juce::Rectangle<float> (bounds.getX(), bounds.getCentreY() - grooveThickness * 0.5f, bounds.getWidth(), grooveThickness);
    g.setColour (Palette::well);
    g.fillRoundedRectangle (groove, 3.0f);
    g.setColour (Palette::outline);
    g.drawRoundedRectangle (groove, 3.0f, 1.0f);

    // Scale ticks
    g.setColour (Palette::textFaint);
    for (int i = 0; i <= 10; ++i)
    {
        const float t = (float) i / 10.0f;
        const float len = (i == 5) ? 9.0f : 5.0f;
        if (vertical)
        {
            const float ty = bounds.getY() + t * bounds.getHeight();
            g.fillRect (groove.getX() - len - 4.0f, ty - 0.5f, len, 1.0f);
            g.fillRect (groove.getRight() + 4.0f, ty - 0.5f, len, 1.0f);
        }
        else
        {
            const float tx = bounds.getX() + t * bounds.getWidth();
            g.fillRect (tx - 0.5f, groove.getY() - len - 4.0f, 1.0f, len);
            g.fillRect (tx - 0.5f, groove.getBottom() + 4.0f, 1.0f, len);
        }
    }

    // Cap
    const float capLong = 34.0f, capShort = 20.0f;
    juce::Rectangle<float> cap = vertical
        ? juce::Rectangle<float> (capLong, capShort).withCentre ({ bounds.getCentreX(), pos })
        : juce::Rectangle<float> (capShort, capLong).withCentre ({ pos, bounds.getCentreY() });
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a404b), cap.getX(), cap.getY(),
                                             juce::Colour (0xff1c2027), cap.getRight(), cap.getBottom(), false));
    g.fillRoundedRectangle (cap, 3.0f);
    g.setColour (Palette::outline.brighter (0.3f));
    g.drawRoundedRectangle (cap, 3.0f, 1.0f);
    g.setColour (s.isEnabled() ? accent : Palette::textFaint);
    if (vertical)
        g.fillRect (cap.getX() + 3.0f, cap.getCentreY() - 1.0f, cap.getWidth() - 6.0f, 2.0f);
    else
        g.fillRect (cap.getCentreX() - 1.0f, cap.getY() + 3.0f, 2.0f, cap.getHeight() - 6.0f);
}

void HypeLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                            bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const float corner = juce::jmin (6.0f, r.getHeight() * 0.3f);
    const bool on = b.getToggleState();
    const auto onColour = b.findColour (juce::TextButton::buttonOnColourId);

    if (on)
    {
        g.setColour (onColour.withAlpha (0.25f));
        g.fillRoundedRectangle (r.expanded (1.0f), corner + 1.0f);   // glow
        g.setColour (down ? onColour.darker (0.2f) : onColour);
        g.fillRoundedRectangle (r, corner);
    }
    else
    {
        auto base = b.findColour (juce::TextButton::buttonColourId);
        if (down)             base = base.darker (0.3f);
        else if (highlighted) base = base.brighter (0.12f);
        g.setGradientFill (juce::ColourGradient (base.brighter (0.06f), r.getX(), r.getY(),
                                                 base.darker (0.15f), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, corner);
        g.setColour (onColour.withAlpha (b.isEnabled() ? 0.55f : 0.2f));
        g.drawRoundedRectangle (r, corner, 1.2f);
    }
}

void HypeLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    const bool on = b.getToggleState();
    g.setFont (getTextButtonFont (b, b.getHeight()));
    auto colour = on ? b.findColour (juce::TextButton::textColourOnId)
                     : b.findColour (juce::TextButton::buttonOnColourId).interpolatedWith (Palette::text, 0.35f);
    g.setColour (b.isEnabled() ? colour : colour.withAlpha (0.4f));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (4, 2), juce::Justification::centred, 2);
}

void HypeLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    auto r = b.getLocalBounds().toFloat();
    auto box = r.removeFromLeft (r.getHeight()).reduced (4.0f);
    g.setColour (Palette::well);
    g.fillRoundedRectangle (box, 3.0f);
    g.setColour (highlighted ? Palette::textDim : Palette::outline);
    g.drawRoundedRectangle (box, 3.0f, 1.0f);
    if (b.getToggleState())
    {
        g.setColour (b.findColour (juce::ToggleButton::tickColourId));
        g.fillRoundedRectangle (box.reduced (3.5f), 2.0f);
    }
    g.setColour (b.findColour (juce::ToggleButton::textColourId).withAlpha (b.isEnabled() ? 1.0f : 0.4f));
    g.setFont (font (14.0f));
    g.drawFittedText (b.getButtonText(), r.toNearestInt().withTrimmedLeft (4), juce::Justification::centredLeft, 1);
}

void HypeLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<int> (w, h).toFloat().reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (box.hasKeyboardFocus (true) ? Palette::deckA : box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, 4.0f, 1.0f);

    juce::Path arrow;
    const float ax = (float) w - 16.0f, ay = (float) h * 0.5f;
    arrow.addTriangle (ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (box.isEnabled() ? 1.0f : 0.3f));
    g.fillPath (arrow);
}
} // namespace hype::ui
