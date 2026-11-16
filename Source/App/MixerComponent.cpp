#include "MixerComponent.h"
#include "Engine/ThreeBandEq.h"

namespace hype::ui
{
namespace
{
    juce::String dbText (double v)
    {
        if (v <= ThreeBandEq::minGainDb + 0.01) return "KILL";
        return (v > 0.05 ? "+" : "") + juce::String (v, 1) + " dB";
    }

    juce::String gainToDbText (double g)
    {
        if (g <= 0.0001) return "-inf";
        const double d = juce::Decibels::gainToDecibels (g);
        return (d > 0.05 ? "+" : "") + juce::String (d, 1) + " dB";
    }
}

//==============================================================================
ChannelStrip::ChannelStrip (Mixer& m, Deck& d, int i)
    : mixer (m), deck (d), index (i), accent (Palette::forDeck (i)),
      gain ("Gain", accent, true), high ("Hi", accent, true), mid ("Mid", accent, true), low ("Low", accent, true)
{
    // Channel gain (trim): -24..+12 dB with 0 dB at the centre of travel.
    gain.slider.setRange (config::minTrimDb, config::maxTrimDb, 0.1);
    gain.slider.setSkewFactorFromMidPoint (0.0);
    gain.slider.setDoubleClickReturnValue (true, 0.0);
    gain.formatValue = [] (double v) { return (v > 0.05 ? "+" : "") + juce::String (v, 1) + " dB"; };
    gain.slider.onValueChange = [this] { deck.setTrimDb ((float) gain.slider.getValue()); gain.repaint(); };
    gain.slider.setTooltip ("Channel gain (trim)");
    addAndMakeVisible (gain);

    Knob* eqKnobs[3] { &low, &mid, &high };
    juce::TextButton* kills[3] { &killLow, &killMid, &killHigh };
    for (int band = 0; band < 3; ++band)
    {
        auto* k = eqKnobs[band];
        k->slider.setRange (ThreeBandEq::minGainDb, ThreeBandEq::maxGainDb, 0.1);
        k->slider.setSkewFactorFromMidPoint (0.0);
        k->slider.setDoubleClickReturnValue (true, 0.0);
        k->formatValue = dbText;
        k->slider.onValueChange = [this, k, band] { mixer.setEqGainDb (index, band, (float) k->slider.getValue()); k->repaint(); };
        k->slider.setTooltip ("EQ: turn fully left to kill the band. Double-click to reset.");
        addAndMakeVisible (*k);

        auto* kb = kills[band];
        kb->setClickingTogglesState (true);
        kb->setColour (juce::TextButton::buttonOnColourId, Palette::danger);
        kb->setTooltip ("Kill this band");
        kb->onClick = [this, kb, band] { mixer.setEqKill (index, band, kb->getToggleState()); };
        addAndMakeVisible (*kb);
    }

    cueButton.setButtonText ("CUE");
    cueButton.setClickingTogglesState (true);
    cueButton.setColour (juce::TextButton::buttonOnColourId, Palette::headphones);
    cueButton.setTooltip ("Headphone cue: pre-listen this deck in the headphones (does not affect the master)");
    cueButton.onClick = [this] { mixer.setCueEnabled (index, cueButton.getToggleState()); };
    addAndMakeVisible (cueButton);

    fader.setSliderStyle (juce::Slider::LinearVertical);
    fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    fader.setRange (0.0, 1.0, 0.001);
    fader.setColour (juce::Slider::trackColourId, accent);
    fader.setTooltip ("Channel volume");
    fader.onValueChange = [this] { mixer.setChannelVolume (index, (float) fader.getValue()); };
    addAndMakeVisible (fader);
    addAndMakeVisible (meter);

    const juce::String p = index == 0 ? "A." : "B.";
    gain.slider.setComponentID (p + "gain");
    high.slider.setComponentID (p + "eq.high");
    mid.slider.setComponentID (p + "eq.mid");
    low.slider.setComponentID (p + "eq.low");
    killHigh.setComponentID (p + "kill.high");
    killMid.setComponentID (p + "kill.mid");
    killLow.setComponentID (p + "kill.low");
    cueButton.setComponentID (p + "phones");
    fader.setComponentID (p + "volume");

    syncFromEngine();
}

void ChannelStrip::syncFromEngine()
{
    auto setIfIdle = [] (juce::Slider& sl, double v)
    {
        if (! sl.isMouseButtonDown() && std::abs (sl.getValue() - v) > 1.0e-6)
            sl.setValue (v, juce::dontSendNotification);
    };
    setIfIdle (fader, mixer.getChannelVolume (index));
    setIfIdle (gain.slider, deck.getTrimDb());
    cueButton.setToggleState (mixer.isCueEnabled (index), juce::dontSendNotification);
    setIfIdle (high.slider, mixer.getEqGainDb (index, ThreeBandEq::high));
    setIfIdle (mid.slider, mixer.getEqGainDb (index, ThreeBandEq::mid));
    setIfIdle (low.slider, mixer.getEqGainDb (index, ThreeBandEq::low));
    killHigh.setToggleState (mixer.isEqKilled (index, ThreeBandEq::high), juce::dontSendNotification);
    killMid.setToggleState (mixer.isEqKilled (index, ThreeBandEq::mid), juce::dontSendNotification);
    killLow.setToggleState (mixer.isEqKilled (index, ThreeBandEq::low), juce::dontSendNotification);
}

void ChannelStrip::updateMeter (float peak)
{
    meter.setLevels (peak);
}

void ChannelStrip::paint (juce::Graphics& g)
{
    drawCaption (g, index == 0 ? "CH A" : "CH B", getLocalBounds().removeFromTop (16), juce::Justification::centred, accent);
}

void ChannelStrip::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (18);

    auto knobRow = [&r] (Knob& k, juce::TextButton* kill)
    {
        auto row = r.removeFromTop (74);
        if (kill != nullptr)
            kill->setBounds (row.removeFromRight (24).withSizeKeepingCentre (22, 20).translated (0, 4));
        else
            row.removeFromRight (24);
        k.setBounds (row);
    };
    knobRow (gain, nullptr);
    r.removeFromTop (4);
    knobRow (high, &killHigh);
    knobRow (mid, &killMid);
    knobRow (low, &killLow);
    r.removeFromTop (8);
    cueButton.setBounds (r.removeFromTop (30).reduced (6, 0));
    r.removeFromTop (10);

    auto meterArea = r.removeFromRight (14);
    meter.setBounds (meterArea.withTrimmedTop (4));
    fader.setBounds (r.reduced (0, 6));
}

//==============================================================================
MixerComponent::MixerComponent (AudioEngine& e)
    : engine (e), mixer (e.getMixer()),
      stripA (e.getMixer(), e.getDeck (0), 0), stripB (e.getMixer(), e.getDeck (1), 1),
      master ("Volume", Palette::text, false), cueMix ("Cue / Mst", Palette::headphones, false),
      cueLevel ("Phones", Palette::headphones, false), curve ("Curve", Palette::text, false)
{
    addAndMakeVisible (stripA);
    addAndMakeVisible (stripB);

    master.slider.setRange (0.0, 2.0, 0.001);
    master.slider.setValue (mixer.getMasterGain(), juce::dontSendNotification);
    master.slider.setDoubleClickReturnValue (true, 1.0);
    master.formatValue = gainToDbText;
    master.slider.onValueChange = [this] { mixer.setMasterGain ((float) master.slider.getValue()); master.repaint(); };
    master.slider.setTooltip ("Master volume");

    cueMix.slider.setRange (0.0, 1.0, 0.001);
    cueMix.slider.setValue (mixer.getCueMix(), juce::dontSendNotification);
    cueMix.slider.setDoubleClickReturnValue (true, 0.0);
    cueMix.formatValue = [] (double v) { return v < 0.02 ? juce::String ("CUE") : v > 0.98 ? juce::String ("MASTER")
                                                                                            : juce::String (juce::roundToInt (v * 100)) + "% mst"; };
    cueMix.slider.onValueChange = [this] { mixer.setCueMix ((float) cueMix.slider.getValue()); cueMix.repaint(); };
    cueMix.slider.setTooltip ("Headphone mix: cued channels <-> master");

    cueLevel.slider.setRange (0.0, 2.0, 0.001);
    cueLevel.slider.setValue (mixer.getCueGain(), juce::dontSendNotification);
    cueLevel.slider.setDoubleClickReturnValue (true, 1.0);
    cueLevel.formatValue = gainToDbText;
    cueLevel.slider.onValueChange = [this] { mixer.setCueGain ((float) cueLevel.slider.getValue()); cueLevel.repaint(); };
    cueLevel.slider.setTooltip ("Headphone volume");

    curve.slider.setRange (0.0, 1.0, 0.001);
    curve.slider.setValue (mixer.getCrossfaderCurveShape(), juce::dontSendNotification);
    curve.formatValue = [] (double v) { return v < 0.08 ? juce::String ("Smooth") : v > 0.85 ? juce::String ("Cut")
                                                                                             : juce::String ("Sharp ") + juce::String (juce::roundToInt (v * 100)); };
    curve.slider.onValueChange = [this] { mixer.setCrossfaderCurveShape ((float) curve.slider.getValue()); curve.repaint(); };
    curve.slider.setTooltip ("Crossfader curve: smooth blend (left) to sharp scratch cut (right)");

    for (auto* k : { &master, &cueMix, &cueLevel, &curve })
        addAndMakeVisible (*k);

    splitButton.setClickingTogglesState (true);
    splitButton.setColour (juce::TextButton::buttonOnColourId, Palette::headphones);
    splitButton.setTooltip ("Split cue: cue in the left ear, master in the right ear");
    splitButton.onClick = [this] { mixer.setSplitCue (splitButton.getToggleState()); };
    addAndMakeVisible (splitButton);

    reverseButton.setClickingTogglesState (true);
    reverseButton.setColour (juce::TextButton::buttonOnColourId, Palette::text);
    reverseButton.setTooltip ("Reverse the crossfader (hamster)");
    reverseButton.onClick = [this] { mixer.setCrossfaderReverse (reverseButton.getToggleState()); };
    addAndMakeVisible (reverseButton);

    crossfader.setSliderStyle (juce::Slider::LinearHorizontal);
    crossfader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    crossfader.setRange (0.0, 1.0, 0.001);
    crossfader.setValue (mixer.getCrossfader(), juce::dontSendNotification);
    crossfader.setDoubleClickReturnValue (true, 0.5);
    crossfader.setColour (juce::Slider::trackColourId, Palette::text);
    crossfader.setTooltip ("Crossfader (double-click to centre)");
    crossfader.onValueChange = [this] { mixer.setCrossfader ((float) crossfader.getValue()); };
    addAndMakeVisible (crossfader);

    master.slider.setComponentID ("master.volume");
    cueMix.slider.setComponentID ("phones.mix");
    cueLevel.slider.setComponentID ("phones.level");
    splitButton.setComponentID ("phones.split");
    crossfader.setComponentID ("xfader");
    curve.slider.setComponentID ("xfader.curve");
    reverseButton.setComponentID ("xfader.reverse");

    masterMeter.setTooltip ("Master output level (dBFS)");
    cueMeter.setTooltip ("Headphone output level (dBFS)");
    addAndMakeVisible (masterMeter);
    addAndMakeVisible (cueMeter);
}

void MixerComponent::syncFromEngine()
{
    stripA.syncFromEngine();
    stripB.syncFromEngine();
    auto setIfIdle = [] (juce::Slider& sl, double v)
    {
        if (! sl.isMouseButtonDown() && std::abs (sl.getValue() - v) > 1.0e-6)
            sl.setValue (v, juce::dontSendNotification);
    };
    setIfIdle (crossfader, mixer.getCrossfader());
    setIfIdle (curve.slider, mixer.getCrossfaderCurveShape());
    setIfIdle (master.slider, mixer.getMasterGain());
    setIfIdle (cueMix.slider, mixer.getCueMix());
    setIfIdle (cueLevel.slider, mixer.getCueGain());
    splitButton.setToggleState (mixer.isSplitCue(), juce::dontSendNotification);
    reverseButton.setToggleState (mixer.isCrossfaderReversed(), juce::dontSendNotification);
}

void MixerComponent::update (const Mixer::Meters& m)
{
    if (++updates % 6 == 0)   // ~10x per second
        syncFromEngine();

    stripA.updateMeter (m.channelPeak[0]);
    stripB.updateMeter (m.channelPeak[1]);
    masterMeter.setLevels (m.masterPeakL, m.masterPeakR, m.masterClipped);
    cueMeter.setLevels (m.cuePeakL, m.cuePeakR);
    const bool nowLimiting = m.limiterReductionDb > 0.5f;
    if (nowLimiting != limiting)
    {
        limiting = nowLimiting;
        repaint (limitLed);
    }
}

void MixerComponent::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (Palette::panel);
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (Palette::outline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);

    g.setColour (Palette::well.withAlpha (0.6f));
    g.fillRoundedRectangle (masterArea.toFloat(), 6.0f);
    g.fillRoundedRectangle (phonesArea.toFloat(), 6.0f);
    g.fillRoundedRectangle (xfArea.toFloat(), 6.0f);

    drawCaption (g, "Master", masterArea.withHeight (18));
    drawCaption (g, "Headphones", phonesArea.withHeight (18), juce::Justification::centred, Palette::headphones);

    g.setColour (limiting ? Palette::danger : Palette::well);
    g.fillRoundedRectangle (limitLed.toFloat(), 3.0f);
    g.setColour (limiting ? Palette::text : Palette::textFaint);
    g.setFont (HypeLookAndFeel::font (10.0f, true));
    g.drawText ("LIMIT", limitLed, juce::Justification::centred);

    // Crossfader A / B labels
    auto xf = crossfader.getBounds();
    g.setFont (HypeLookAndFeel::font (16.0f, true));
    g.setColour (Palette::deckA);
    g.drawText ("A", xf.getX() - 24, xf.getY(), 20, xf.getHeight(), juce::Justification::centred);
    g.setColour (Palette::deckB);
    g.drawText ("B", xf.getRight() + 4, xf.getY(), 20, xf.getHeight(), juce::Justification::centred);
}

void MixerComponent::resized()
{
    auto r = getLocalBounds().reduced (10);

    xfArea = r.removeFromBottom (86);
    r.removeFromBottom (8);
    {
        auto x = xfArea.reduced (8, 6);
        auto side = x.removeFromRight (120);
        curve.setBounds (side.removeFromLeft (70));
        reverseButton.setBounds (side.withSizeKeepingCentre (46, 26));
        x.removeFromRight (8);
        crossfader.setBounds (x.reduced (26, 14));
    }

    const int stripW = juce::jmax (100, r.getWidth() * 30 / 100);
    stripA.setBounds (r.removeFromLeft (stripW));
    stripB.setBounds (r.removeFromRight (stripW));
    r.reduce (8, 0);

    auto centre = r;
    masterArea = centre.removeFromTop (centre.getHeight() * 62 / 100);
    centre.removeFromTop (8);
    phonesArea = centre;

    {
        auto m = masterArea.reduced (6);
        m.removeFromTop (18);
        master.setBounds (m.removeFromTop (78));
        limitLed = m.removeFromBottom (18).withSizeKeepingCentre (52, 16);
        m.removeFromBottom (4);
        masterMeter.setBounds (m.withSizeKeepingCentre (juce::jmin (34, m.getWidth()), m.getHeight()));
    }
    {
        auto p = phonesArea.reduced (6);
        p.removeFromTop (18);
        auto meterArea = p.removeFromRight (16);
        cueMeter.setBounds (meterArea.reduced (0, 4));
        splitButton.setBounds (p.removeFromBottom (26).reduced (4, 0));
        p.removeFromBottom (4);
        const int h = p.getHeight() / 2;
        cueMix.setBounds (p.removeFromTop (h));
        cueLevel.setBounds (p);
    }
}
} // namespace hype::ui
