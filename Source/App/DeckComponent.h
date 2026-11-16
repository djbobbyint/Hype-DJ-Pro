#pragma once

#include "JogWheel.h"
#include "WaveformViews.h"

namespace hype::ui
{
/**
    One deck: track info, large BPM, times, overview waveform, jog wheel, pitch fader with range
    and bend, key lock, CUE / PLAY / SYNC. Every control talks to the engine directly through the
    thread-safe Deck / AudioEngine API; the display is refreshed from DeckState by update().
*/
class DeckComponent : public juce::Component,
                      public juce::FileDragAndDropTarget
{
public:
    DeckComponent (EngineInterface&, int deckIndex);

    void update (const DeckState&, std::shared_ptr<const WaveformData> waveform);

    /** Shows a transient message in the deck header (errors, hints). */
    void showMessage (const juce::String& text, juce::Colour colour = Palette::danger);

    /** Loads a file (rejects it while the deck is playing, to protect the live mix). */
    void requestLoad (const juce::File&);

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override              { dragOver = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void openFileChooser();
    void toggleSync();
    void cyclePitchRange();
    juce::Rectangle<int> infoArea, timeArea, headerArea;

    EngineInterface& engine;
    const int deck;
    const juce::Colour accent;
    DeckState state;

    OverviewWaveform overview;
    JogWheel jog;

    juce::TextButton loadButton { "LOAD" }, ejectButton { "EJECT" }, keyLockButton { "KEY LOCK" },
                     rangeButton { "+/-8%" }, playButton { "PLAY" }, syncButton { "SYNC" };
    HoldButton cueButton { "CUE" }, bendDown { "-" }, bendUp { "+" };
    juce::Slider pitchFader;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::String message;
    juce::Colour messageColour;
    juce::uint32 messageUntil = 0;
    bool dragOver = false;
    bool loading = false;
};
} // namespace hype::ui
