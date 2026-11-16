#include "DeckComponent.h"

namespace hype::ui
{
namespace
{
    constexpr double pitchRanges[] { 0.08, 0.16, 0.50, 1.00 };
    constexpr double bendAmount = 0.04;
}

DeckComponent::DeckComponent (AudioEngine& e, int d)
    : engine (e), deck (d), accent (Palette::forDeck (d)), overview (e, d), jog (e, d)
{
    auto& dk = engine.getDeck (deck);

    addAndMakeVisible (overview);
    addAndMakeVisible (jog);

    for (auto* b : { &loadButton, &ejectButton, &keyLockButton, &rangeButton, &syncButton })
    {
        b->setColour (juce::TextButton::buttonOnColourId, accent);
        addAndMakeVisible (*b);
    }

    loadButton.setTooltip ("Load a track (or drag a file onto the deck)");
    loadButton.onClick = [this] { openFileChooser(); };

    ejectButton.setTooltip ("Unload the track");
    ejectButton.onClick = [this]
    {
        if (state.playing) { showMessage ("Pause the deck before ejecting"); return; }
        engine.getDeck (deck).eject();
    };

    keyLockButton.setClickingTogglesState (true);
    keyLockButton.setTooltip ("Key lock: keep the original key when changing tempo");
    keyLockButton.onClick = [this] { engine.getDeck (deck).setKeyLock (keyLockButton.getToggleState()); };

    rangeButton.setTooltip ("Pitch fader range");
    rangeButton.onClick = [this] { cyclePitchRange(); };

    syncButton.setClickingTogglesState (false);
    syncButton.setTooltip ("Sync: match tempo and beat phase to the other deck and keep tempo locked");
    syncButton.onClick = [this] { toggleSync(); };

    // Transport
    playButton.setColour (juce::TextButton::buttonOnColourId, Palette::play);
    playButton.setTooltip ("Play / pause");
    playButton.onClick = [this] { engine.getDeck (deck).togglePlayPause(); };
    addAndMakeVisible (playButton);

    cueButton.setColour (juce::TextButton::buttonOnColourId, Palette::cue);
    cueButton.setTooltip ("CUE: while playing returns to the cue point; when paused sets the cue point, "
                          "or previews from it while held");
    cueButton.onPress   = [this] { engine.getDeck (deck).cueDown(); };
    cueButton.onRelease = [this] { engine.getDeck (deck).cueUp(); };
    addAndMakeVisible (cueButton);

    // Pitch
    pitchFader.setSliderStyle (juce::Slider::LinearVertical);
    pitchFader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    pitchFader.setRange (-1.0, 1.0, 0.0005);
    pitchFader.setDoubleClickReturnValue (true, 0.0);
    pitchFader.setColour (juce::Slider::trackColourId, accent);
    pitchFader.setTooltip ("Pitch / tempo fader (down = faster). Double-click to reset.");
    // CDJ convention: pulling the fader towards you (down) speeds up.
    pitchFader.onValueChange = [this] { engine.getDeck (deck).setPitchFader (-pitchFader.getValue()); };
    addAndMakeVisible (pitchFader);

    for (auto* b : { &bendDown, &bendUp })
    {
        b->setColour (juce::TextButton::buttonOnColourId, accent);
        addAndMakeVisible (*b);
    }
    bendDown.setTooltip ("Pitch bend: slow down while held");
    bendUp.setTooltip ("Pitch bend: speed up while held");
    bendDown.onPress = [this] { engine.getDeck (deck).setPitchBend (-bendAmount); };
    bendUp.onPress   = [this] { engine.getDeck (deck).setPitchBend (bendAmount); };
    bendDown.onRelease = bendUp.onRelease = [this] { engine.getDeck (deck).setPitchBend (0.0); };

    dk.setPitchRange (pitchRanges[0]);

    // Stable IDs (automation, MIDI mapping, UI self-test).
    const juce::String p = deck == 0 ? "A." : "B.";
    loadButton.setComponentID (p + "load");
    ejectButton.setComponentID (p + "eject");
    keyLockButton.setComponentID (p + "keylock");
    rangeButton.setComponentID (p + "range");
    syncButton.setComponentID (p + "sync");
    playButton.setComponentID (p + "play");
    cueButton.setComponentID (p + "cue");
    bendDown.setComponentID (p + "bendDown");
    bendUp.setComponentID (p + "bendUp");
    pitchFader.setComponentID (p + "pitch");
    jog.setComponentID (p + "jog");
    overview.setComponentID (p + "overview");
}

void DeckComponent::cyclePitchRange()
{
    auto& dk = engine.getDeck (deck);
    int idx = 0;
    for (int i = 0; i < 4; ++i)
        if (std::abs (dk.getPitchRange() - pitchRanges[i]) < 1e-6)
            idx = (i + 1) % 4;
    // Keep the current tempo when changing range (like a CDJ: the tempo only re-maps on the next fader move).
    const double tempo = dk.getTempoRatio();
    dk.setPitchRange (pitchRanges[idx]);
    dk.setTempoRatio (tempo);
}

void DeckComponent::toggleSync()
{
    const int other = 1 - deck;
    if (engine.getSyncLockMaster (deck) == other)
    {
        engine.setSyncLock (deck, -1);
        return;
    }
    const auto otherState = engine.getDeck (other).getState();
    if (state.originalBpm <= 0.0 || otherState.originalBpm <= 0.0)
    {
        showMessage ("Sync needs a BPM on both decks");
        return;
    }
    engine.syncDeck (deck, other, true);   // match tempo + snap beat phase
    engine.setSyncLock (deck, other);      // then keep following the other deck's tempo
}

void DeckComponent::openFileChooser()
{
    if (state.playing)
    {
        showMessage ("Deck is playing - pause it before loading a new track");
        return;
    }
    chooser = std::make_unique<juce::FileChooser> (juce::String ("Load track to deck ") + (deck == 0 ? "A" : "B"),
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   engine.getSupportedFormatsWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile())
                                  requestLoad (f);
                          });
}

void DeckComponent::requestLoad (const juce::File& file)
{
    if (state.playing)
    {
        showMessage ("Deck is playing - pause it before loading a new track");
        return;
    }

    loading = true;
    showMessage ("Loading " + file.getFileName() + "...", Palette::textDim);
    juce::Component::SafePointer<DeckComponent> safe (this);
    engine.loadTrackAsync (deck, file, [safe] (int, const AudioFileDecoder::Result& r)
    {
        const bool ok = r.wasOk();
        const auto error = r.error;
        juce::MessageManager::callAsync ([safe, ok, error]
        {
            if (safe == nullptr) return;
            safe->loading = false;
            if (ok) safe->showMessage ({}, Palette::textDim);
            else if (! error.startsWith ("Cancelled")) safe->showMessage ("Could not load: " + error);
        });
    });
}

void DeckComponent::showMessage (const juce::String& text, juce::Colour colour)
{
    message = text;
    messageColour = colour;
    messageUntil = juce::Time::getMillisecondCounter() + (text.isEmpty() ? 0u : 4000u);
    repaint();
}

bool DeckComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && juce::File (files[0]).existsAsFile();
}

void DeckComponent::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    repaint();
    if (! files.isEmpty())
        requestLoad (juce::File (files[0]));
}

void DeckComponent::update (const Deck::State& s, const LoadedTrack::Ptr& track)
{
    state = s;
    overview.update (s, track);
    jog.update (s);

    playButton.setToggleState (s.playing && ! s.cuePreviewing, juce::dontSendNotification);
    playButton.setButtonText (s.playing && ! s.cuePreviewing ? "PAUSE" : "PLAY");
    cueButton.setToggleState (s.cuePreviewing || (! s.playing && s.loaded && std::abs (s.elapsedSeconds - s.cueSeconds) < 0.01),
                              juce::dontSendNotification);
    keyLockButton.setToggleState (s.keyLock, juce::dontSendNotification);
    syncButton.setToggleState (engine.getSyncLockMaster (deck) == 1 - deck, juce::dontSendNotification);

    auto& dk = engine.getDeck (deck);
    rangeButton.setButtonText ("+/-" + juce::String (juce::roundToInt (dk.getPitchRange() * 100.0)) + "%");
    if (! pitchFader.isMouseButtonDown())
        pitchFader.setValue (-dk.getPitchFader(), juce::dontSendNotification);   // follows sync

    for (auto* b : std::initializer_list<juce::Button*> { &playButton, &cueButton, &syncButton, &bendDown, &bendUp, &ejectButton })
        b->setEnabled (s.loaded);

    if (messageUntil != 0 && juce::Time::getMillisecondCounter() > messageUntil && ! loading)
    {
        message.clear();
        messageUntil = 0;
    }
    repaint (headerArea);
    repaint (infoArea);
    repaint (timeArea);
}

void DeckComponent::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (Palette::panel);
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (dragOver ? accent : Palette::outline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, dragOver ? 2.5f : 1.0f);

    // Header: deck letter + title / artist (or message)
    {
        auto h = headerArea;
        auto badge = h.removeFromLeft (34).withSizeKeepingCentre (30, 30);
        g.setColour (accent);
        g.fillRoundedRectangle (badge.toFloat(), 5.0f);
        g.setColour (Palette::background);
        g.setFont (HypeLookAndFeel::font (20.0f, true));
        g.drawText (deck == 0 ? "A" : "B", badge, juce::Justification::centred);
        h.removeFromLeft (10);

        auto titleRow = h.removeFromTop (h.getHeight() / 2 + 3);
        g.setFont (HypeLookAndFeel::font (18.0f, true));
        g.setColour (state.loaded ? Palette::text : Palette::textFaint);
        g.drawText (state.loaded ? (state.title.isNotEmpty() ? state.title : juce::String ("Untitled")) : juce::String ("No track loaded"),
                    titleRow, juce::Justification::bottomLeft, true);

        g.setFont (HypeLookAndFeel::font (13.0f));
        if (message.isNotEmpty())
        {
            g.setColour (messageColour);
            g.drawText (message, h, juce::Justification::centredLeft, true);
        }
        else
        {
            g.setColour (Palette::textDim);
            g.drawText (state.loaded ? (state.artist.isNotEmpty() ? state.artist : juce::String ("Unknown artist")) : juce::String ("Drag a file here or press LOAD"),
                        h, juce::Justification::centredLeft, true);
        }
    }

    // BPM / pitch block
    {
        auto r = infoArea;
        g.setColour (Palette::well);
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        r.reduce (12, 6);

        auto bpmArea = r.removeFromLeft (r.getWidth() * 55 / 100);
        g.setFont (HypeLookAndFeel::monoFont (46.0f));
        g.setColour (state.originalBpm > 0 ? Palette::text : Palette::textFaint);
        g.drawText (state.originalBpm > 0 ? juce::String (state.currentBpm, 2) : juce::String ("--.--"),
                    bpmArea.withTrimmedBottom (16), juce::Justification::centredLeft);
        drawCaption (g, state.originalBpm > 0 ? "BPM  (orig " + juce::String (state.originalBpm, 2) + ")" : juce::String ("BPM"),
                     bpmArea.removeFromBottom (16), juce::Justification::centredLeft);

        const auto pct = state.pitchPercent;
        g.setFont (HypeLookAndFeel::monoFont (22.0f));
        g.setColour (std::abs (pct) < 0.005 ? Palette::textDim : accent);
        g.drawText ((pct >= 0 ? "+" : "") + juce::String (pct, 2) + "%", r.withTrimmedBottom (16),
                    juce::Justification::centredRight);
        drawCaption (g, state.keyLockPathActive ? "TEMPO  *  KEY LOCK" : "TEMPO",
                     r.removeFromBottom (16), juce::Justification::centredRight,
                     state.keyLockPathActive ? accent : Palette::textDim);
    }

    // Times
    {
        auto r = timeArea;
        const int third = r.getWidth() / 3;
        auto drawTime = [&g] (juce::Rectangle<int> area, const juce::String& caption, const juce::String& value,
                              juce::Colour colour, juce::Justification j)
        {
            drawCaption (g, caption, area.removeFromTop (14), j);
            g.setFont (HypeLookAndFeel::monoFont (20.0f));
            g.setColour (colour);
            g.drawText (value, area, j);
        };
        const bool warn = state.loaded && state.remainingSeconds < 30.0 && state.playing;
        drawTime (r.removeFromLeft (third), "Elapsed", state.loaded ? formatTime (state.elapsedSeconds) : "--:--.-",
                  Palette::text, juce::Justification::centredLeft);
        drawTime (r.removeFromLeft (third), "Remaining", state.loaded ? "-" + formatTime (state.remainingSeconds) : "--:--.-",
                  warn ? Palette::danger : Palette::text, juce::Justification::centred);
        drawTime (r, "Duration", state.loaded ? formatTime (state.durationSeconds, false) : "--:--",
                  Palette::textDim, juce::Justification::centredRight);
    }

    // Pitch fader legend
    {
        auto pf = pitchFader.getBounds();
        g.setColour (Palette::textDim);
        g.setFont (HypeLookAndFeel::font (14.0f, true));
        g.drawText ("-", pf.getX(), pf.getY() - 16, pf.getWidth(), 14, juce::Justification::centred);
        g.drawText ("+", pf.getX(), pf.getBottom() + 1, pf.getWidth(), 14, juce::Justification::centred);
    }
}

void DeckComponent::resized()
{
    auto r = getLocalBounds().reduced (12);
    const bool mirrored = deck == 1;   // pitch section on the outer edge

    auto header = r.removeFromTop (44);
    auto headerButtons = mirrored ? header.removeFromLeft (150) : header.removeFromRight (150);
    loadButton.setBounds (headerButtons.removeFromLeft (80).reduced (2, 6));
    ejectButton.setBounds (headerButtons.reduced (2, 6));
    headerArea = header;
    if (mirrored) headerArea.removeFromLeft (8); else headerArea.removeFromRight (8);

    r.removeFromTop (8);
    infoArea = r.removeFromTop (78);
    r.removeFromTop (8);
    timeArea = r.removeFromTop (38);
    r.removeFromTop (8);
    overview.setBounds (r.removeFromTop (40));
    r.removeFromTop (10);

    auto transport = r.removeFromBottom (52);
    r.removeFromBottom (10);

    // Pitch section
    auto pitch = mirrored ? r.removeFromRight (70) : r.removeFromLeft (70);
    rangeButton.setBounds (pitch.removeFromTop (28));
    keyLockButton.setBounds (pitch.removeFromBottom (28));
    pitch.removeFromBottom (6);
    auto bend = pitch.removeFromBottom (30);
    bendDown.setBounds (bend.removeFromLeft (bend.getWidth() / 2).reduced (2, 0));
    bendUp.setBounds (bend.reduced (2, 0));
    pitch.removeFromBottom (20);
    pitch.removeFromTop (20);
    pitchFader.setBounds (pitch);

    // Jog
    if (mirrored) r.removeFromRight (10); else r.removeFromLeft (10);
    jog.setBounds (r);

    // Transport: CUE, PLAY, SYNC
    const int w = transport.getWidth();
    auto cueArea  = transport.removeFromLeft (w * 32 / 100);
    auto syncArea = transport.removeFromRight (w * 22 / 100);
    cueButton.setBounds (cueArea.reduced (3, 0));
    syncButton.setBounds (syncArea.reduced (3, 0));
    playButton.setBounds (transport.reduced (3, 0));
}
} // namespace hype::ui
