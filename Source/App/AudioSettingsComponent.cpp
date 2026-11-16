#include "AudioSettingsComponent.h"

namespace hype::ui
{
namespace
{
    constexpr int rowHeight = 30;
    constexpr int labelWidth = 150;

    juce::Colour stateColour (DeviceState s)
    {
        switch (s)
        {
            case DeviceState::running:    return Palette::play;
            case DeviceState::fallback:   return Palette::cue;
            case DeviceState::recovering: return Palette::cue;
            case DeviceState::noDevice:   return Palette::danger;
        }
        return Palette::danger;
    }

    juce::String stateText (DeviceState s)
    {
        switch (s)
        {
            case DeviceState::running:    return "CONNECTED";
            case DeviceState::fallback:   return "FALLBACK DEVICE";
            case DeviceState::recovering: return "RECONNECTING";
            case DeviceState::noDevice:   return "NO DEVICE";
        }
        return {};
    }
}

AudioSettingsComponent::AudioSettingsComponent (DeviceControl& c) : controller (c)
{
    for (auto* box : { &typeBox, &deviceBox, &rateBox, &bufferBox, &masterBox, &phonesBox })
        addAndMakeVisible (*box);

    typeBox.onChange = [this]
    {
        if (updating) return;
        applyChange ([this] (auto& s) { s.typeName = typeBox.getText(); s.deviceName = {}; s.sampleRate = 0; s.bufferSize = 0; s.outputChannels.clear(); });
    };
    deviceBox.onChange = [this]
    {
        if (updating) return;
        applyChange ([this] (auto& s) { s.deviceName = deviceBox.getText(); s.sampleRate = 0; s.bufferSize = 0; s.outputChannels.clear(); });
    };
    rateBox.onChange = [this]
    {
        if (updating) return;
        applyChange ([this] (auto& s) { s.sampleRate = rateBox.getSelectedId(); });
    };
    bufferBox.onChange = [this]
    {
        if (updating) return;
        applyChange ([this] (auto& s) { s.bufferSize = bufferBox.getSelectedId(); });
    };
    masterBox.onChange = [this]
    {
        if (updating) return;
        controller.setOutputAssignment (masterBox.getSelectedId() - 2, controller.getSettings().cueFirstChannel);
        refresh();
        if (onSettingsChanged) onSettingsChanged();
    };
    phonesBox.onChange = [this]
    {
        if (updating) return;
        controller.setOutputAssignment (controller.getSettings().masterFirstChannel, phonesBox.getSelectedId() - 2);
        refresh();
        if (onSettingsChanged) onSettingsChanged();
    };

    refreshButton.setTooltip ("Rescan for audio devices (e.g. after connecting an interface)");
    refreshButton.onClick = [this] { controller.refreshDevices(); refresh(); };
    addAndMakeVisible (refreshButton);

    fallbackToggle.onClick = [this]
    {
        if (updating) return;
        controller.setFallbackToDefaultDevice (fallbackToggle.getToggleState());   // no device restart
        if (onSettingsChanged) onSettingsChanged();
    };
    addAndMakeVisible (fallbackToggle);

    channelViewport.setViewedComponent (&channelHolder, false);
    channelViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (channelViewport);

    setSize (640, 560);
    refresh();
}

void AudioSettingsComponent::applyChange (const std::function<void (DeviceSettings&)>& change)
{
    auto s = controller.getSettings();
    if (auto st = controller.getStatus(); s.typeName.isEmpty())
        s.typeName = st.typeName;
    change (s);
    controller.apply (s);
    refresh();
    if (onSettingsChanged) onSettingsChanged();
}

void AudioSettingsComponent::fillPairCombo (juce::ComboBox& box, int selectedFirst, const juce::StringArray& names)
{
    box.clear (juce::dontSendNotification);
    box.addItem ("Off", 1);
    for (int ch = 0; ch < names.size(); ch += 2)
    {
        const auto label = ch + 1 < names.size() ? names[ch] + " / " + names[ch + 1] : names[ch] + " (mono)";
        box.addItem (label, ch + 2);   // id = firstChannel + 2
    }
    box.setSelectedId (selectedFirst >= 0 && selectedFirst < names.size() ? selectedFirst + 2 : 1, juce::dontSendNotification);
}

void AudioSettingsComponent::rebuildChannelToggles (const juce::StringArray& names, const juce::BigInteger& active)
{
    channelToggles.clear();
    for (int ch = 0; ch < names.size(); ++ch)
    {
        auto* t = channelToggles.add (new juce::ToggleButton (names[ch]));
        t->setToggleState (active[ch], juce::dontSendNotification);
        t->onClick = [this, ch, t]
        {
            // Start from what the device is running now, then toggle this channel.
            auto channels = controller.getActiveOutputChannels();
            channels.setBit (ch, t->getToggleState());
            applyChange ([channels] (auto& s) { s.outputChannels = channels; });
        };
        channelHolder.addAndMakeVisible (t);
    }
    const int cols = 4;
    const int w = juce::jmax (100, channelViewport.getWidth() - 12);
    const int rows = (names.size() + cols - 1) / cols;
    channelHolder.setSize (w, juce::jmax (1, rows) * 26);
    for (int i = 0; i < channelToggles.size(); ++i)
        channelToggles[i]->setBounds ((i % cols) * (w / cols), (i / cols) * 26, w / cols, 24);
}

void AudioSettingsComponent::refresh()
{
    const juce::ScopedValueSetter<bool> svs (updating, true);
    const auto st = controller.getStatus();
    const auto& s = controller.getSettings();

    typeBox.clear (juce::dontSendNotification);
    const auto types = controller.getDeviceTypes();
    for (int i = 0; i < types.size(); ++i)
        typeBox.addItem (types[i], i + 1);
    typeBox.setText (st.typeName.isNotEmpty() ? st.typeName : s.typeName, juce::dontSendNotification);

    deviceBox.clear (juce::dontSendNotification);
    const auto devices = controller.getDeviceNames (typeBox.getText());
    for (int i = 0; i < devices.size(); ++i)
        deviceBox.addItem (devices[i], i + 1);
    deviceBox.setText (st.deviceName, juce::dontSendNotification);
    deviceBox.setTextWhenNoChoicesAvailable ("(no devices found)");

    rateBox.clear (juce::dontSendNotification);
    for (auto rate : controller.getAvailableSampleRates())
        rateBox.addItem (juce::String (rate / 1000.0, ((int) rate) % 1000 == 0 ? 0 : 1) + " kHz", (int) rate);
    rateBox.setSelectedId ((int) st.sampleRate, juce::dontSendNotification);

    bufferBox.clear (juce::dontSendNotification);
    for (auto size : controller.getAvailableBufferSizes())
        bufferBox.addItem (juce::String (size) + " samples"
                           + (st.sampleRate > 0 ? "  (" + juce::String (1000.0 * size / st.sampleRate, 1) + " ms)" : juce::String()),
                           size);
    bufferBox.setSelectedId (st.bufferSize, juce::dontSendNotification);

    const auto channelNames = controller.getOutputChannelNames();
    fillPairCombo (masterBox, s.masterFirstChannel, channelNames);
    fillPairCombo (phonesBox, s.cueFirstChannel, channelNames);

    rebuildChannelToggles (channelNames, controller.getActiveOutputChannels());

    fallbackToggle.setToggleState (s.fallbackToDefaultDevice, juce::dontSendNotification);

    for (auto* c : { (juce::Component*) &rateBox, (juce::Component*) &bufferBox, (juce::Component*) &masterBox, (juce::Component*) &phonesBox })
        c->setEnabled (st.numOutputChannels > 0);

    repaint();
}

void AudioSettingsComponent::paint (juce::Graphics& g)
{
    g.fillAll (Palette::panel);

    // Status card
    const auto st = controller.getStatus();
    auto card = statusArea;
    g.setColour (Palette::well);
    g.fillRoundedRectangle (card.toFloat(), 6.0f);
    card.reduce (14, 10);

    auto top = card.removeFromTop (22);
    g.setColour (stateColour (st.state));
    g.fillEllipse (top.removeFromLeft (14).withSizeKeepingCentre (10, 10).toFloat());
    top.removeFromLeft (6);
    g.setFont (HypeLookAndFeel::font (12.0f, true));
    g.drawText (stateText (st.state), top, juce::Justification::centredLeft);

    g.setColour (Palette::text);
    g.setFont (HypeLookAndFeel::font (18.0f, true));
    g.drawText (st.deviceName.isNotEmpty() ? st.deviceName : juce::String ("No audio device"), card.removeFromTop (26),
                juce::Justification::centredLeft, true);

    g.setColour (Palette::textDim);
    g.setFont (HypeLookAndFeel::font (14.0f));
    juce::String details;
    if (st.sampleRate > 0)
        details << st.typeName << "   |   " << juce::String (st.sampleRate / 1000.0, 1) << " kHz   |   "
                << st.bufferSize << " samples   |   " << juce::String (st.latencyMs, 1) << " ms output latency   |   "
                << st.numActiveOutputs << " of " << st.numOutputChannels << " outputs active";
    else
        details << (st.typeName.isNotEmpty() ? st.typeName : juce::String ("No driver"));
    g.drawText (details, card.removeFromTop (20), juce::Justification::centredLeft, true);

    if (st.message.isNotEmpty())
    {
        g.setColour (st.state == DeviceState::running ? Palette::textDim : Palette::cue);
        g.drawText (st.message, card.removeFromTop (20), juce::Justification::centredLeft, true);
    }

    // Row labels
    g.setColour (Palette::textDim);
    g.setFont (HypeLookAndFeel::font (14.0f));
    auto label = [&g] (juce::Component& c, const juce::String& text)
    {
        g.drawText (text, c.getX() - labelWidth, c.getY(), labelWidth - 10, c.getHeight(), juce::Justification::centredRight);
    };
    label (typeBox, "Audio driver");
    label (deviceBox, "Output device");
    label (rateBox, "Sample rate");
    label (bufferBox, "Buffer size");
    label (masterBox, "Master output");
    label (phonesBox, "Headphone output");
    g.drawText ("Active outputs", channelViewport.getX() - labelWidth, channelViewport.getY(), labelWidth - 10, 24,
                juce::Justification::centredRight);
}

void AudioSettingsComponent::resized()
{
    auto r = getLocalBounds().reduced (18);
    statusArea = r.removeFromTop (110);
    r.removeFromTop (16);

    auto row = [&r] () { auto x = r.removeFromTop (rowHeight); r.removeFromTop (8); x.removeFromLeft (labelWidth); return x; };

    auto typeRow = row();
    refreshButton.setBounds (typeRow.removeFromRight (100));
    typeRow.removeFromRight (8);
    typeBox.setBounds (typeRow);
    deviceBox.setBounds (row());
    rateBox.setBounds (row());
    bufferBox.setBounds (row());
    masterBox.setBounds (row());
    phonesBox.setBounds (row());

    auto fb = r.removeFromBottom (26);
    fb.removeFromLeft (labelWidth);
    fallbackToggle.setBounds (fb);
    r.removeFromBottom (8);
    r.removeFromLeft (labelWidth);
    channelViewport.setBounds (r);
    rebuildChannelToggles (controller.getOutputChannelNames(), controller.getActiveOutputChannels());
}
} // namespace hype::ui
