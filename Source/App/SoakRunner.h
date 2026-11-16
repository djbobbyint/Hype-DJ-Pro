#pragma once

#include "Api/HypeEngineApi.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <random>

namespace hype::ui
{
/**
    Real-time stability run on the actual audio device (manual hardware test, docs/TESTING.md).

    Started with --soak <minutes>. Both decks play continuously while a seeded "virtual DJ"
    operates every mixer and deck control twice a second, exactly like the automated soak test,
    but in real time through the selected audio interface. Every minute it appends a line to the
    report (device, sample rate, buffer, CPU peak, driver xruns, deck positions, master peak);
    at the end it writes PASS/FAIL and quits with exit code 0/1.

    Fails if: a deck stops advancing, the device drops out, the driver reports xruns, the
    master clips, or the meters report non-finite values.
*/
class SoakRunner : private juce::Timer
{
public:
    SoakRunner (EngineInterface& engine, double minutes, juce::File reportFile);
    void start();

private:
    void timerCallback() override;
    void act();
    void report (const juce::String& line);
    void finish();

    EngineInterface& engine;
    const double durationSeconds;
    const juce::File reportFile;
    std::minstd_rand rng { 7 };
    double startMs = 0, lastMinuteMs = 0;
    double lastPos[numDecks] { -1, -1 };
    int stalls = 0, deviceProblems = 0, clips = 0, nonFinite = 0, startXruns = -1, actions = 0;
    double cpuPeak = 0;
    float masterPeak = 0;
};
} // namespace hype::ui
