#include "MainComponent.h"

namespace hype::ui
{
namespace
{
    juce::Colour statusColour (DeviceState s)
    {
        switch (s)
        {
            case DeviceState::running:    return Palette::play;
            case DeviceState::fallback:
            case DeviceState::recovering: return Palette::cue;
            case DeviceState::noDevice:   return Palette::danger;
        }
        return Palette::danger;
    }
}

MainComponent::MainComponent()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "HYPE DJ PRO";
   #if JUCE_LINUX || JUCE_BSD
    options.folderName = ".config/HYPE DJ PRO";
   #else
    options.folderName = "HYPE DJ PRO";
   #endif
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    properties = std::make_unique<juce::PropertiesFile> (options);

    // Restore mixer state before the mixer UI reads it.
    if (auto xml = properties->getXmlValue ("mixer"))
        restoreMixerXml (xml.get());

    mixer = std::make_unique<MixerComponent> (engine);

    addAndMakeVisible (waveA);
    addAndMakeVisible (waveB);
    addAndMakeVisible (deckA);
    addAndMakeVisible (deckB);
    addAndMakeVisible (*mixer);

    settingsButton.setColour (juce::TextButton::buttonOnColourId, Palette::text);
    settingsButton.setTooltip ("Audio device, sample rate, buffer size and output routing");
    settingsButton.onClick = [this] { openAudioSettings(); };
    addAndMakeVisible (settingsButton);

    deviceController.setStatusListener ([this]
    {
        deviceStatus = deviceController.getStatus();
        if (settingsComponent != nullptr)
            settingsComponent->refresh();
        repaint (topBar);
    });

    engine.setRunWithoutOutput (true);   // keep the decks running even without an output device
    restoreSettings();
    deviceStatus = deviceController.getStatus();

    setSize (1600, 940);
    startTimerHz (60);
}

MainComponent::~MainComponent()
{
    stopTimer();
    saveSettings();
    selfTest.reset();
    soak.reset();
    deviceController.setStatusListener (nullptr);
    if (settingsWindow != nullptr)
        delete settingsWindow.getComponent();
}

//==============================================================================
void MainComponent::restoreSettings()
{
    auto audioXml = properties->getXmlValue ("audio");
    deviceController.restoreFromXml (audioXml.get());   // null -> default driver + device
}

void MainComponent::saveSettings()
{
    if (selfTest != nullptr || soak != nullptr || screenshotAtFrame >= 0 || screenshotFile != juce::File())
        return;   // automated runs never overwrite the user's settings

    if (auto xml = deviceController.createStateXml())
        properties->setValue ("audio", xml.get());
    if (auto xml = createMixerXml())
        properties->setValue ("mixer", xml.get());
    properties->saveIfNeeded();
}

std::unique_ptr<juce::XmlElement> MainComponent::createMixerXml() const
{
    const auto& m = engine.mixer();
    auto xml = std::make_unique<juce::XmlElement> ("HYPE_MIXER");
    xml->setAttribute ("crossfader", m.getCrossfader());
    xml->setAttribute ("curve", m.getCrossfaderCurveShape());
    xml->setAttribute ("reverse", m.isCrossfaderReversed());
    xml->setAttribute ("master", m.getMasterVolume());
    xml->setAttribute ("cueMix", engine.cue().getCueMix());
    xml->setAttribute ("cueGain", engine.cue().getCueLevel());
    return xml;
}

void MainComponent::restoreMixerXml (const juce::XmlElement* xml)
{
    if (xml == nullptr || ! xml->hasTagName ("HYPE_MIXER"))
        return;
    auto& m = engine.mixer();
    m.setCrossfader ((float) xml->getDoubleAttribute ("crossfader", 0.5));
    m.setCrossfaderCurveShape ((float) xml->getDoubleAttribute ("curve", 0.0));
    m.setCrossfaderReverse (xml->getBoolAttribute ("reverse", false));
    m.setMasterVolume ((float) xml->getDoubleAttribute ("master", 1.0));
    engine.cue().setCueMix ((float) xml->getDoubleAttribute ("cueMix", 0.0));
    engine.cue().setCueLevel ((float) xml->getDoubleAttribute ("cueGain", 1.0));
}

//==============================================================================
void MainComponent::openAudioSettings()
{
    if (settingsWindow != nullptr)
    {
        settingsWindow->toFront (true);
        return;
    }

    auto* content = new AudioSettingsComponent (deviceController);
    content->onSettingsChanged = [this] { saveSettings(); };
    settingsComponent = content;

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "HYPE DJ PRO - Audio Settings";
    o.dialogBackgroundColour = Palette::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = this;
    settingsWindow = o.launchAsync();
}

void MainComponent::timerCallback()
{
    ++frame;

    for (int d = 0; d < 2; ++d)
    {
        auto& dk = engine.deck (d);
        const auto state = dk.getState();
        const auto waveform = dk.getWaveform();
        (d == 0 ? waveA : waveB).update (state, waveform);
        (d == 0 ? deckA : deckB).update (state, waveform);
    }
    mixer->update (engine.mixer().getAndResetMeters(), engine.cue().getAndResetMeters());

    if (frame % 15 == 0)   // device status / CPU 4x per second
    {
        deviceStatus = deviceController.getStatus();
        repaint (topBar);
    }

    if (screenshotAtFrame >= 0 && frame >= screenshotAtFrame)
    {
        screenshotAtFrame = -1;
        auto* target = settingsComponent != nullptr ? (juce::Component*) settingsComponent.getComponent() : (juce::Component*) this;
        auto image = target->createComponentSnapshot (target->getLocalBounds(), true, 2.0f);
        screenshotFile.deleteFile();
        juce::FileOutputStream os (screenshotFile);
        juce::PNGImageFormat().writeImageToStream (image, os);
        os.flush();
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
}

void MainComponent::handleCommandLine (const juce::StringArray& args)
{
    auto value = [&args] (const juce::String& key) { const int i = args.indexOf (key); return i >= 0 ? args[i + 1].unquoted() : juce::String(); };
    const auto cwd = juce::File::getCurrentWorkingDirectory();

    playAfterLoads = args.contains ("--play");
    soakMinutes = value ("--soak").getDoubleValue();
    startSelfTestAfterLoads = args.contains ("--selftest");

    for (int d = 0; d < 2; ++d)
    {
        const auto f = value (d == 0 ? "--load-a" : "--load-b");
        if (f.isEmpty())
            continue;
        ++pendingLoads;
        juce::Component::SafePointer<MainComponent> safe (this);
        engine.loadTrack (d, cwd.getChildFile (f), [safe] (int, const LoadResult&)
        {
            juce::MessageManager::callAsync ([safe]
            {
                if (safe != nullptr && --safe->pendingLoads == 0)
                    juce::Timer::callAfterDelay (200, [safe] { if (safe != nullptr) safe->onCommandLineLoadsFinished(); });
            });
        });
    }
    if (pendingLoads == 0)
        juce::Timer::callAfterDelay (200, [this] { onCommandLineLoadsFinished(); });

    if (args.contains ("--settings"))
        juce::Timer::callAfterDelay (200, [this] { openAudioSettings(); });

    if (auto shot = value ("--screenshot"); shot.isNotEmpty())
    {
        screenshotFile = cwd.getChildFile (shot);
        screenshotAtFrame = 180;   // ~3 s
    }
}

void MainComponent::onCommandLineLoadsFinished()
{
    if (playAfterLoads)
    {
        engine.deck (0).seekToSeconds (20.0);
        engine.deck (0).play();
        engine.deck (1).seekToSeconds (5.0);
        engine.deck (1).setKeyLock (true);
        engine.cue().setCueEnabled (1, true);
        engine.mixer().setCrossfader (0.35f);
        engine.syncDeck (1, 0, true);
        engine.setSyncLock (1, 0);
        engine.deck (1).play();
    }

    if (soakMinutes > 0.0)
    {
        const auto reportFile = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                    .getChildFile ("HYPE DJ PRO soak " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M") + ".txt");
        soak = std::make_unique<SoakRunner> (engine, soakMinutes, reportFile);
        soak->start();
    }

    if (startSelfTestAfterLoads)
    {
        selfTest = std::make_unique<UiSelfTest> (*this, engine);
        selfTest->start();
    }
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Palette::background);

    // Top bar
    auto bar = topBar;
    g.setColour (Palette::panel);
    g.fillRect (bar);
    g.setColour (Palette::outline);
    g.fillRect (bar.removeFromBottom (1));

    auto logo = topBar.reduced (16, 0).removeFromLeft (220);
    g.setFont (HypeLookAndFeel::font (22.0f, true).withExtraKerningFactor (0.06f));
    g.setColour (Palette::text);
    g.drawText ("HYPE", logo, juce::Justification::centredLeft);
    g.setColour (Palette::deckA);
    g.drawText ("DJ PRO", logo.withTrimmedLeft (70), juce::Justification::centredLeft);

    // Device status pill
    const auto& st = deviceStatus;
    g.setColour (Palette::well);
    g.fillRoundedRectangle (statusPill.toFloat(), statusPill.getHeight() * 0.5f);
    auto p = statusPill.reduced (12, 0);
    g.setColour (statusColour (st.state));
    g.fillEllipse (p.removeFromLeft (10).withSizeKeepingCentre (10, 10).toFloat());
    p.removeFromLeft (10);

    juce::String text;
    if (st.state == DeviceState::running || st.state == DeviceState::fallback)
        text << st.deviceName << "   " << juce::String (st.sampleRate / 1000.0, 1) << " kHz   "
             << st.bufferSize << " smp   " << juce::String (st.latencyMs, 1) << " ms   CPU "
             << juce::roundToInt (st.cpuLoad * 100.0) << "%"
             << (st.state == DeviceState::fallback ? "   (fallback)" : "");
    else
        text << (st.message.isNotEmpty() ? st.message : juce::String ("No audio device - open Audio Settings"))
             << (st.engineRunningWithoutOutput ? "   (engine running without output)" : "");

    g.setColour (Palette::text);
    g.setFont (HypeLookAndFeel::font (14.0f));
    g.drawText (text, p, juce::Justification::centredLeft, true);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    topBar = r.removeFromTop (48);
    {
        auto t = topBar.reduced (12, 8);
        settingsButton.setBounds (t.removeFromRight (150));
        t.removeFromRight (12);
        statusPill = t.removeFromRight (juce::jmin (620, t.getWidth() - 240));
    }

    r.reduce (10, 8);
    waveA.setBounds (r.removeFromTop (96));
    r.removeFromTop (4);
    waveB.setBounds (r.removeFromTop (96));
    r.removeFromTop (10);

    const int mixerWidth = juce::jlimit (400, 520, r.getWidth() * 30 / 100);
    const int deckWidth = (r.getWidth() - mixerWidth - 20) / 2;
    deckA.setBounds (r.removeFromLeft (deckWidth));
    deckB.setBounds (r.removeFromRight (deckWidth));
    r.reduce (10, 0);
    mixer->setBounds (r);
}

void MainComponent::mouseUp (const juce::MouseEvent& e)
{
    if (statusPill.contains (e.getPosition()))
        openAudioSettings();
}
} // namespace hype::ui
