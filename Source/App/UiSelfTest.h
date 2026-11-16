#pragma once

#include "Engine/AudioEngine.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace hype::ui
{
/**
    Drives the real UI widgets of a running HYPE DJ PRO window (by component ID, with
    synthesised mouse events / slider moves) and verifies that each one changed the audio
    engine. Started with --selftest; prints PASS/FAIL per check and quits with an exit code.
*/
class UiSelfTest : private juce::Timer
{
public:
    UiSelfTest (juce::Component& root, AudioEngine& engine);
    void start();

private:
    struct Step
    {
        juce::String name;
        std::function<void()> action;
        std::function<bool (juce::String&)> check;   // returns pass/fail, may fill a detail string
    };

    void timerCallback() override;
    void build();

    juce::Component* find (const juce::String& id) const;
    void click (const juce::String& id);
    void press (const juce::String& id, juce::Point<float> at = { -1, -1 });
    void drag (const juce::String& id, juce::Point<float> to);
    void release (const juce::String& id, juce::Point<float> at = { -1, -1 });
    void setSlider (const juce::String& id, double value);
    juce::MouseEvent makeEvent (juce::Component&, juce::Point<float>, bool down) const;

    juce::Component& root;
    AudioEngine& engine;
    std::vector<Step> steps;
    size_t current = 0;
    bool waitingForCheck = false;
    int passed = 0, failed = 0;
    juce::Point<float> dragStart;
    double jogStart = 0.0;
    bool reverseBefore = false;
};
} // namespace hype::ui
