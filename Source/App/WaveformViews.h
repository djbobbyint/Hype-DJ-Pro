#pragma once

#include "Engine/AudioEngine.h"
#include "Widgets.h"

namespace hype::ui
{
/** Colour for a waveform column from its band energies (bass red, mids green, highs blue). */
juce::Colour bandColour (uint8_t low, uint8_t mid, uint8_t high, float brightness = 1.0f);

/**
    Zoomed waveform that scrolls past a fixed centre playhead, with beat grid and cue marker.
    Drag while paused to scrub; drag while playing to nudge (pitch bend). Mouse wheel zooms.
*/
class ScrollingWaveform : public juce::Component
{
public:
    ScrollingWaveform (AudioEngine&, int deckIndex);

    /** Called by the UI timer with the latest deck state. */
    void update (const Deck::State&, const LoadedTrack::Ptr& track);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    AudioEngine& engine;
    const int deck;
    Deck::State state;
    LoadedTrack::Ptr track;
    double visibleSeconds = 8.0;
    int lastDragX = 0;
};

/** Whole-track overview with progress, cue marker and click/drag-to-seek. */
class OverviewWaveform : public juce::Component, public juce::SettableTooltipClient
{
public:
    OverviewWaveform (AudioEngine&, int deckIndex);

    void update (const Deck::State&, const LoadedTrack::Ptr& track);

    void paint (juce::Graphics&) override;
    void resized() override { cacheValid = false; }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    void rebuildCache();
    void seekTo (int x);

    AudioEngine& engine;
    const int deck;
    Deck::State state;
    LoadedTrack::Ptr track;
    juce::Image cache;
    bool cacheValid = false;
};
} // namespace hype::ui
