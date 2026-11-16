#pragma once

#include "Engine/AudioEngine.h"
#include "Widgets.h"

namespace hype::ui
{
/**
    Jog wheel. The platter turns with the real playhead (33 1/3 RPM: one revolution = 1.8 s).
      - Playing: turning it nudges tempo (momentary pitch bend proportional to speed of turn).
      - Paused:  turning it scrubs the playhead (one revolution = 1.8 s of audio).
    Mouse wheel does the same in small steps.
*/
class JogWheel : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    JogWheel (AudioEngine&, int deckIndex);

    void update (const Deck::State&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr double secondsPerRevolution = 1.8;

private:
    void timerCallback() override;   // releases the bend shortly after the wheel stops
    void turn (double deltaRadians);
    float angleOf (juce::Point<float>) const;

    AudioEngine& engine;
    const int deck;
    Deck::State state;
    float lastAngle = 0.0f;
    bool touching = false;
    bool bending = false;
};
} // namespace hype::ui
