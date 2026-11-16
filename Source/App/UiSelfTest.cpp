#include "UiSelfTest.h"
#include "Engine/ThreeBandEq.h"

#include <iostream>

namespace hype::ui
{
namespace
{
    bool near (double a, double b, double tol) { return std::abs (a - b) <= tol; }
}

UiSelfTest::UiSelfTest (juce::Component& r, AudioEngine& e) : root (r), engine (e)
{
    build();
}

void UiSelfTest::start()
{
    std::cout << "HYPE DJ PRO UI self-test: " << steps.size() << " steps\n";
    startTimer (350);
}

juce::Component* UiSelfTest::find (const juce::String& id) const
{
    std::function<juce::Component* (juce::Component&)> search = [&] (juce::Component& c) -> juce::Component*
    {
        if (c.getComponentID() == id)
            return &c;
        for (auto* child : c.getChildren())
            if (auto* found = search (*child))
                return found;
        return nullptr;
    };
    return search (root);
}

juce::MouseEvent UiSelfTest::makeEvent (juce::Component& c, juce::Point<float> pos, bool down) const
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos,
                             down ? juce::ModifierKeys::leftButtonModifier : juce::ModifierKeys(),
                             juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f,
                             &c, &c, now, dragStart, now, 1, false);
}

void UiSelfTest::click (const juce::String& id)
{
    if (auto* b = dynamic_cast<juce::Button*> (find (id)))
        b->triggerClick();
}

void UiSelfTest::press (const juce::String& id, juce::Point<float> at)
{
    if (auto* c = find (id))
    {
        if (at.x < 0) at = c->getLocalBounds().toFloat().getCentre();
        dragStart = at;
        c->mouseDown (makeEvent (*c, at, true));
    }
}

void UiSelfTest::drag (const juce::String& id, juce::Point<float> to)
{
    if (auto* c = find (id))
        c->mouseDrag (makeEvent (*c, to, true));
}

void UiSelfTest::release (const juce::String& id, juce::Point<float> at)
{
    if (auto* c = find (id))
    {
        if (at.x < 0) at = c->getLocalBounds().toFloat().getCentre();
        c->mouseUp (makeEvent (*c, at, false));
    }
}

void UiSelfTest::setSlider (const juce::String& id, double value)
{
    if (auto* s = dynamic_cast<juce::Slider*> (find (id)))
        s->setValue (value, juce::sendNotificationSync);
}

void UiSelfTest::build()
{
    auto& a = engine.getDeck (0);
    auto& b = engine.getDeck (1);
    auto& mixer = engine.getMixer();

    auto add = [this] (juce::String name, std::function<void()> action, std::function<bool (juce::String&)> check)
    {
        steps.push_back ({ std::move (name), std::move (action), std::move (check) });
    };

    add ("Decks have tracks loaded", [] {}, [&a, &b] (juce::String& d)
    {
        d = a.getState().title + " / " + b.getState().title;
        return a.getState().loaded && b.getState().loaded;
    });

    // ---- Deck A transport & tempo
    add ("A PLAY starts deck A", [this] { click ("A.play"); }, [&a] (juce::String&) { return a.getState().playing; });
    add ("A pitch fader (down = faster) sets tempo +4 %", [this] { setSlider ("A.pitch", -0.5); },
         [&a] (juce::String& d) { d = juce::String (a.getTempoRatio(), 4); return near (a.getTempoRatio(), 1.04, 1e-6); });
    add ("A range button switches to +/-16 % and keeps tempo", [this] { click ("A.range"); },
         [&a] (juce::String& d) { d = juce::String (a.getPitchRange()); return near (a.getPitchRange(), 0.16, 1e-9) && near (a.getTempoRatio(), 1.04, 1e-6); });
    add ("A KEY LOCK enables the key-lock path", [this] { click ("A.keylock"); },
         [&a] (juce::String&) { return a.isKeyLockOn() && a.getState().keyLockPathActive; });
    add ("A pitch bend + (held) speeds up", [this] { press ("A.bendUp"); },
         [&a] (juce::String& d) { const auto s = a.getState(); d = juce::String (s.effectiveSpeed, 4); return near (s.effectiveSpeed, 1.08, 1e-3); });
    add ("A pitch bend released returns to tempo", [this] { release ("A.bendUp"); },
         [&a] (juce::String& d) { const auto s = a.getState(); d = juce::String (s.effectiveSpeed, 4); return near (s.effectiveSpeed, 1.04, 1e-3); });

    // ---- Deck B + sync
    add ("B PLAY starts deck B", [this] { click ("B.play"); }, [&b] (juce::String&) { return b.getState().playing; });
    add ("B SYNC locks B to A's tempo", [this] { click ("B.sync"); }, [this, &a, &b] (juce::String& d)
    {
        const auto sa = a.getState(), sb = b.getState();
        d = juce::String (sb.currentBpm, 3) + " vs " + juce::String (sa.currentBpm, 3);
        return engine.getSyncLockMaster (1) == 0 && near (sa.currentBpm, sb.currentBpm, 1e-3);
    });
    add ("B SYNC again releases the lock", [this] { click ("B.sync"); },
         [this] (juce::String&) { return engine.getSyncLockMaster (1) == -1; });

    // ---- Channel strips
    add ("A GAIN knob sets channel gain +3 dB", [this] { setSlider ("A.gain", 3.0); },
         [&a] (juce::String&) { return near (a.getTrimDb(), 3.0, 0.05); });
    add ("A LOW knob fully left kills the low band", [this] { setSlider ("A.eq.low", ThreeBandEq::minGainDb); },
         [&mixer] (juce::String&) { return mixer.getEqGainDb (0, ThreeBandEq::low) <= ThreeBandEq::minGainDb; });
    add ("A MID knob -6 dB", [this] { setSlider ("A.eq.mid", -6.0); },
         [&mixer] (juce::String&) { return near (mixer.getEqGainDb (0, ThreeBandEq::mid), -6.0, 0.05); });
    add ("A HI kill button", [this] { click ("A.kill.high"); },
         [&mixer] (juce::String&) { return mixer.isEqKilled (0, ThreeBandEq::high); });
    add ("B HI knob +4 dB", [this] { setSlider ("B.eq.high", 4.0); },
         [&mixer] (juce::String&) { return near (mixer.getEqGainDb (1, ThreeBandEq::high), 4.0, 0.05); });
    add ("A channel fader to half", [this] { setSlider ("A.volume", 0.5); },
         [&mixer] (juce::String&) { return near (mixer.getChannelVolume (0), 0.5, 1e-3); });
    add ("B channel fader to 0.8", [this] { setSlider ("B.volume", 0.8); },
         [&mixer] (juce::String&) { return near (mixer.getChannelVolume (1), 0.8, 1e-3); });

    // ---- Headphones
    add ("B headphone CUE button", [this] { click ("B.phones"); }, [&mixer] (juce::String&) { return mixer.isCueEnabled (1); });
    add ("A headphone CUE button (both cued)", [this] { click ("A.phones"); },
         [&mixer] (juce::String&) { return mixer.isCueEnabled (0) && mixer.isCueEnabled (1); });
    add ("Headphone cue/master mix", [this] { setSlider ("phones.mix", 0.25); },
         [&mixer] (juce::String&) { return near (mixer.getCueMix(), 0.25, 1e-3); });
    add ("Headphone level", [this] { setSlider ("phones.level", 1.5); },
         [&mixer] (juce::String&) { return near (mixer.getCueGain(), 1.5, 1e-3); });
    add ("Split cue button", [this] { click ("phones.split"); }, [&mixer] (juce::String&) { return mixer.isSplitCue(); });

    // ---- Crossfader & master
    add ("Crossfader to deck B", [this] { setSlider ("xfader", 1.0); },
         [&mixer] (juce::String&) { return near (mixer.getCrossfader(), 1.0, 1e-3); });
    add ("Crossfader curve knob", [this] { setSlider ("xfader.curve", 0.8); },
         [&mixer] (juce::String&) { return near (mixer.getCrossfaderCurveShape(), 0.8, 1e-3); });
    add ("Crossfader reverse button toggles", [this, &mixer] { reverseBefore = mixer.isCrossfaderReversed(); click ("xfader.reverse"); },
         [this, &mixer] (juce::String&) { return mixer.isCrossfaderReversed() != reverseBefore; });
    add ("Master volume knob", [this] { setSlider ("master.volume", 0.7); },
         [&mixer] (juce::String&) { return near (mixer.getMasterGain(), 0.7, 1e-3); });

    // ---- Cue / seek / jog on deck A
    add ("A CUE while playing returns to the cue point and pauses", [this] { press ("A.cue"); release ("A.cue"); },
         [&a] (juce::String& d) { const auto s = a.getState(); d = juce::String (s.elapsedSeconds, 3); return ! s.playing && near (s.elapsedSeconds, s.cueSeconds, 0.01); });
    add ("A overview click seeks to 50 %", [this] { press ("A.overview", { (float) find ("A.overview")->getWidth() * 0.5f, 5.0f }); },
         [&a] (juce::String& d) { const auto s = a.getState(); d = juce::String (s.position, 3); return near (s.position, 0.5, 0.01); });
    add ("A jog quarter turn (paused) scrubs +0.45 s", [this, &a]
         {
             jogStart = a.getState().elapsedSeconds;
             auto* jog = find ("A.jog");
             const auto c = jog->getLocalBounds().toFloat().getCentre();
             const float r = jog->getHeight() * 0.35f;
             press ("A.jog", c + juce::Point<float> (r, 0));          // angle 0 (3 o'clock)
             for (int i = 1; i <= 9; ++i)                              // clockwise to 6 o'clock
             {
                 const float ang = juce::MathConstants<float>::halfPi * i / 9.0f;
                 drag ("A.jog", c + juce::Point<float> (std::cos (ang), std::sin (ang)) * r);
             }
             release ("A.jog");
         },
         [this, &a] (juce::String& d)
         {
             const double moved = a.getState().elapsedSeconds - jogStart;
             d = juce::String (moved, 3) + " s";
             return near (moved, 0.45, 0.02);
         });
    add ("A CUE when paused away from cue sets a new cue point", [this] { press ("A.cue"); release ("A.cue"); },
         [&a] (juce::String&) { const auto s = a.getState(); return near (s.cueSeconds, s.elapsedSeconds, 0.01); });
    add ("A CUE held on the cue point previews", [this] { press ("A.cue"); },
         [&a] (juce::String&) { const auto s = a.getState(); return s.playing && s.cuePreviewing; });
    add ("A CUE released stops the preview at the cue point", [this] { release ("A.cue"); },
         [&a] (juce::String&) { const auto s = a.getState(); return ! s.playing && near (s.elapsedSeconds, s.cueSeconds, 0.01); });
    add ("B pitch fader up = slower", [this] { setSlider ("B.pitch", 0.25); },
         [&b] (juce::String& d) { d = juce::String (b.getTempoRatio(), 4); return b.getTempoRatio() < 1.0; });
    add ("A EJECT unloads the (paused) deck", [this] { click ("A.eject"); }, [&a] (juce::String&) { return ! a.getState().loaded; });
    add ("Deck B kept playing throughout", [] {}, [&b] (juce::String&) { return b.getState().playing; });
}

void UiSelfTest::timerCallback()
{
    if (current >= steps.size())
    {
        stopTimer();
        std::cout << "==== UI self-test: " << passed << " passed, " << failed << " failed ====\n" << std::flush;
        juce::JUCEApplication::getInstance()->setApplicationReturnValue (failed == 0 ? 0 : 1);
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
        return;
    }

    auto& step = steps[current];
    if (! waitingForCheck)
    {
        step.action();
        waitingForCheck = true;   // give the audio thread time to apply the command
        return;
    }

    juce::String detail;
    const bool ok = step.check (detail);
    (ok ? passed : failed)++;
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << step.name << (detail.isNotEmpty() ? "  [" + detail + "]" : juce::String()) << "\n" << std::flush;
    waitingForCheck = false;
    ++current;
}
} // namespace hype::ui
