#include "JogWheel.h"

namespace hype::ui
{
JogWheel::JogWheel (AudioEngine& e, int d) : engine (e), deck (d)
{
    setTooltip ("Jog wheel: turn while playing to nudge the tempo, while paused to scrub");
}

void JogWheel::update (const Deck::State& s)
{
    state = s;
    repaint();
}

float JogWheel::angleOf (juce::Point<float> p) const
{
    const auto c = getLocalBounds().toFloat().getCentre();
    return std::atan2 (p.y - c.y, p.x - c.x);
}

void JogWheel::paint (juce::Graphics& g)
{
    const auto accent = Palette::forDeck (deck);
    const auto bounds = getLocalBounds().toFloat();
    const float size = juce::jmin (bounds.getWidth(), bounds.getHeight()) - 4.0f;
    const auto r = juce::Rectangle<float> (size, size).withCentre (bounds.getCentre());
    const auto c = r.getCentre();
    const float radius = size * 0.5f;

    // Outer ring with position indicator arc
    g.setColour (Palette::well);
    g.fillEllipse (r);
    g.setColour (touching ? accent : Palette::outline.brighter (0.2f));
    g.drawEllipse (r.reduced (1.0f), touching ? 3.0f : 2.0f);

    if (state.loaded && state.durationSeconds > 0.0)
    {
        juce::Path progress;
        const float pr = radius - 5.0f;
        progress.addCentredArc (c.x, c.y, pr, pr, 0.0f, 0.0f, (float) (state.position * juce::MathConstants<double>::twoPi), true);
        g.setColour (accent.withAlpha (0.8f));
        g.strokePath (progress, juce::PathStrokeType (3.0f));
    }

    // Platter
    const auto platter = r.reduced (size * 0.07f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2b3039), platter.getX(), platter.getY(),
                                             juce::Colour (0xff111318), platter.getRight(), platter.getBottom(), false));
    g.fillEllipse (platter);
    g.setColour (juce::Colours::white.withAlpha (0.03f));
    for (float k = 0.2f; k < 0.95f; k += 0.08f)
        g.drawEllipse (platter.reduced (platter.getWidth() * 0.5f * k), 1.0f);

    // Rotating marker (follows the playhead)
    const double revolutions = state.elapsedSeconds / secondsPerRevolution;
    const float angle = (float) ((revolutions - std::floor (revolutions)) * juce::MathConstants<double>::twoPi)
                        - juce::MathConstants<float>::halfPi;
    const float pr = platter.getWidth() * 0.5f;
    const auto outer = c + juce::Point<float> (std::cos (angle), std::sin (angle)) * (pr - 6.0f);
    const auto inner = c + juce::Point<float> (std::cos (angle), std::sin (angle)) * (pr * 0.45f);
    g.setColour (state.playing ? accent : Palette::text.withAlpha (0.8f));
    g.drawLine ({ inner, outer }, 4.0f);

    // Centre cap
    const auto capR = platter.reduced (platter.getWidth() * 0.34f);
    g.setColour (Palette::panelRaised);
    g.fillEllipse (capR);
    g.setColour (accent.withAlpha (0.7f));
    g.drawEllipse (capR, 1.5f);
    g.setColour (Palette::text);
    g.setFont (HypeLookAndFeel::font (capR.getHeight() * 0.42f, true));
    g.drawText (deck == 0 ? "A" : "B", capR, juce::Justification::centred);
}

void JogWheel::turn (double deltaRadians)
{
    if (! state.loaded || std::abs (deltaRadians) < 1.0e-6)
        return;

    auto& d = engine.getDeck (deck);
    if (state.playing)
    {
        // Clockwise (positive) speeds up. ~0.6 tempo ratio per radian per event, capped.
        d.setPitchBend (juce::jlimit (-0.5, 0.5, deltaRadians * 0.6));
        bending = true;
        startTimer (70);
    }
    else
    {
        d.seekRelativeSeconds (deltaRadians / juce::MathConstants<double>::twoPi * secondsPerRevolution);
    }
}

void JogWheel::mouseDown (const juce::MouseEvent& e)
{
    touching = true;
    lastAngle = angleOf (e.position);
    repaint();
}

void JogWheel::mouseDrag (const juce::MouseEvent& e)
{
    const float a = angleOf (e.position);
    float delta = a - lastAngle;
    if (delta > juce::MathConstants<float>::pi)  delta -= juce::MathConstants<float>::twoPi;
    if (delta < -juce::MathConstants<float>::pi) delta += juce::MathConstants<float>::twoPi;
    lastAngle = a;
    turn (delta);
}

void JogWheel::mouseUp (const juce::MouseEvent&)
{
    touching = false;
    timerCallback();
    repaint();
}

void JogWheel::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    turn ((std::abs (w.deltaY) > 0.0f ? w.deltaY : w.deltaX) * 0.6);
}

void JogWheel::timerCallback()
{
    stopTimer();
    if (bending)
    {
        engine.getDeck (deck).setPitchBend (0.0);
        bending = false;
    }
}
} // namespace hype::ui
