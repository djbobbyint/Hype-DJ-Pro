#include "SoakRunner.h"

#include <iostream>

namespace hype::ui
{
SoakRunner::SoakRunner (EngineInterface& e, double minutes, juce::File file)
    : engine (e), durationSeconds (minutes * 60.0), reportFile (std::move (file)) {}

void SoakRunner::start()
{
    startMs = lastMinuteMs = juce::Time::getMillisecondCounterHiRes();
    const auto st = engine.devices().getStatus();
    startXruns = st.xruns;
    reportFile.replaceWithText ({});
    report ("HYPE DJ PRO real-time soak - " + juce::Time::getCurrentTime().toString (true, true) + " - "
            + juce::String (durationSeconds / 60.0, 1) + " minutes");
    report ("Device: " + st.typeName + " / " + st.deviceName + ", " + juce::String (st.sampleRate, 0) + " Hz, "
            + juce::String (st.bufferSize) + " samples, " + juce::String (st.latencyMs, 1) + " ms");
    report ("Deck A: " + engine.deck (0).getState().title + "   Deck B: " + engine.deck (1).getState().title);

    for (int d = 0; d < numDecks; ++d)
        engine.deck (d).play();
    startTimer (500);
}

void SoakRunner::report (const juce::String& line)
{
    std::cout << line << std::endl;
    reportFile.appendText (line + "\n");
}

void SoakRunner::act()
{
    auto rnd = [this] (double lo, double hi) { return std::uniform_real_distribution<double> (lo, hi) (rng); };
    const int d = (int) rnd (0, 2);
    auto& deck = engine.deck (d);
    auto& mixer = engine.mixer();

    switch ((int) rnd (0, 10))
    {
        case 0: mixer.setCrossfader ((float) rnd (0, 1)); break;
        case 1: mixer.setEqGainDb (d, (EqBand) (int) rnd (0, 3), (float) rnd (-12, 3)); break;
        case 2: mixer.setEqKill (d, (EqBand) (int) rnd (0, 3), rnd (0, 1) < 0.3); break;
        case 3: mixer.setChannelVolume (d, (float) rnd (0.6, 1)); break;
        case 4: deck.setPitchFader (rnd (-0.5, 0.5)); break;
        case 5: deck.setKeyLock (rnd (0, 1) < 0.5); break;
        case 6: deck.seekToNormalised (rnd (0.05, 0.8)); break;
        case 7: engine.cue().setCueEnabled (d, rnd (0, 1) < 0.5); break;
        case 8: deck.setPitchBend (rnd (0, 1) < 0.5 ? 0.0 : rnd (-0.03, 0.03)); break;
        case 9: engine.setSyncLock (d, rnd (0, 1) < 0.5 ? 1 - d : -1); break;
    }
    ++actions;
}

void SoakRunner::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    act();

    // Keep both decks playing (loop back near the end of a track).
    for (int d = 0; d < numDecks; ++d)
    {
        auto& deck = engine.deck (d);
        const auto s = deck.getState();
        if (s.remainingSeconds < 8.0)
            deck.seekToSeconds (2.0);
        if (s.loaded && ! s.playing)
            deck.play();
        if (s.playing && s.effectiveSpeed > 0.5 && std::abs (s.elapsedSeconds - lastPos[d]) < 1.0e-6)
            ++stalls;
        lastPos[d] = s.elapsedSeconds;
    }

    const auto st = engine.devices().getStatus();
    if (st.state != DeviceState::running)
        ++deviceProblems;
    cpuPeak = juce::jmax (cpuPeak, st.cpuLoad);

    // Note: the UI also reads (and resets) the meters; this samples whatever is left at 2 Hz.
    const auto m = engine.mixer().getAndResetMeters();
    if (! std::isfinite (m.masterPeakL) || ! std::isfinite (m.masterPeakR)) ++nonFinite;
    if (m.masterPeakL > 1.0f || m.masterPeakR > 1.0f) ++clips;
    masterPeak = juce::jmax (masterPeak, m.masterPeakL, m.masterPeakR);

    if (now - lastMinuteMs >= 60000.0)
    {
        lastMinuteMs = now;
        const int xruns = (st.xruns >= 0 && startXruns >= 0) ? st.xruns - startXruns : -1;
        report ("  " + juce::String ((now - startMs) / 60000.0, 0).paddedLeft (' ', 4) + " min | "
                + (st.state == DeviceState::running ? "device OK" : "DEVICE " + st.message) + " | CPU peak "
                + juce::String (cpuPeak * 100.0, 1) + "% | xruns " + (xruns >= 0 ? juce::String (xruns) : juce::String ("n/a"))
                + " | A " + juce::String (engine.deck (0).getState().elapsedSeconds, 1) + " s, B "
                + juce::String (engine.deck (1).getState().elapsedSeconds, 1) + " s | master peak "
                + juce::String (juce::Decibels::gainToDecibels (masterPeak), 1) + " dBFS | stalls " + juce::String (stalls)
                + " | actions " + juce::String (actions));
        masterPeak = 0;
    }

    if (now - startMs >= durationSeconds * 1000.0)
        finish();
}

void SoakRunner::finish()
{
    stopTimer();
    const auto st = engine.devices().getStatus();
    const int xruns = (st.xruns >= 0 && startXruns >= 0) ? st.xruns - startXruns : -1;
    const bool pass = stalls == 0 && deviceProblems == 0 && clips == 0 && nonFinite == 0 && xruns <= 0;

    report ("Result: " + juce::String (pass ? "PASS" : "FAIL") + " - stalls " + juce::String (stalls) + ", device problems "
            + juce::String (deviceProblems) + ", clips " + juce::String (clips) + ", non-finite " + juce::String (nonFinite)
            + ", driver xruns " + (xruns >= 0 ? juce::String (xruns) : juce::String ("not reported by this driver"))
            + ", CPU peak " + juce::String (cpuPeak * 100.0, 1) + "%, " + juce::String (actions) + " control actions");
    report ("Report: " + reportFile.getFullPathName());

    juce::JUCEApplication::getInstance()->setApplicationReturnValue (pass ? 0 : 1);
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}
} // namespace hype::ui
