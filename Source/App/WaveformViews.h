#pragma once

#include "Api/HypeEngineApi.h"
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
    ScrollingWaveform (EngineInterface&, int deckIndex);

    /** Called by the UI timer with the latest deck state. */
    void update (const DeckState&, std::shared_ptr<const WaveformData> waveform);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    EngineInterface& engine;
    const int deck;
    DeckState state;
    std::shared_ptr<const WaveformData> waveform;
    double visibleSeconds = 8.0;
    int lastDragX = 0;
};

/** Whole-track overview with progress, cue marker and click/drag-to-seek. */
class OverviewWaveform : public juce::Component, public juce::SettableTooltipClient
{
public:
    OverviewWaveform (EngineInterface&, int deckIndex);

    void update (const DeckState&, std::shared_ptr<const WaveformData> waveform);

    void paint (juce::Graphics&) override;
    void resized() override { cacheValid = false; }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    void rebuildCache();
    void seekTo (int x);

    EngineInterface& engine;
    const int deck;
    DeckState state;
    std::shared_ptr<const WaveformData> waveform;
    juce::Image cache;
    bool cacheValid = false;
};
} // namespace hype::ui
