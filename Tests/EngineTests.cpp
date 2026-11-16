// HYPE DJ PRO - engine test suite.
// Drives the engine offline (no audio device) and verifies playback, tempo/pitch,
// key lock, transport, routing, synchronisation, real-time safety and decoding.

#include "AllocationTracker.h"
#include "Engine/AudioEngine.h"
#include "Engine/AudioDeviceController.h"
#include "Engine/BeatAnalyzer.h"
#include "FakeAudioDevice.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <iostream>

using namespace hype;

namespace
{
constexpr double deviceRate = 48000.0;
constexpr int numOuts = 8;  // 0/1 master, 2/3 cue, 4..7 spare for direct outs

//------------------------------------------------------------------------------
juce::AudioBuffer<float> sine (double freq, double seconds, double sr, float amp = 0.5f)
{
    juce::AudioBuffer<float> b (2, (int) (seconds * sr));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const auto v = amp * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / sr);
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }
    return b;
}

juce::AudioBuffer<float> clickTrack (double bpm, double seconds, double sr, double firstBeat = 0.0)
{
    juce::AudioBuffer<float> b (2, (int) (seconds * sr));
    b.clear();
    const double beatLen = 60.0 / bpm;
    for (double t = firstBeat; t < seconds; t += beatLen)
    {
        const int start = (int) std::llround (t * sr);
        for (int i = 0; i < (int) (0.03 * sr) && start + i < b.getNumSamples(); ++i)
        {
            const float v = 0.8f * std::exp (-i / (0.005f * (float) sr))
                          * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / sr);
            b.setSample (0, start + i, v);
            b.setSample (1, start + i, v);
        }
    }
    return b;
}

LoadedTrack::Ptr makeTrack (const juce::AudioBuffer<float>& b, double sr, double bpm = 0.0, double firstBeat = 0.0)
{
    auto t = LoadedTrack::fromBuffer (b, sr);
    t->setBeatGrid (bpm, firstBeat);
    return t;
}

/** Offline host: renders the engine in device-sized blocks into a growing buffer. */
struct Host
{
    AudioEngine engine;
    int blockSize = 256;

    Host()
    {
        engine.prepareToPlay (deviceRate, blockSize);
        // Unity routing by default for measurements.
        for (int d = 0; d < 2; ++d)
            engine.getMixer().setCrossfaderAssign (d, Mixer::CrossfaderAssign::Thru);
        engine.getMixer().setLimiterEnabled (false);
    }

    juce::AudioBuffer<float> render (double seconds, bool trackAllocations = false)
    {
        const int total = (int) std::llround (seconds * deviceRate);
        juce::AudioBuffer<float> out (numOuts, total);
        out.clear();
        float* ptrs[numOuts];

        for (int pos = 0; pos < total; pos += blockSize)
        {
            const int n = juce::jmin (blockSize, total - pos);
            for (int c = 0; c < numOuts; ++c)
                ptrs[c] = out.getWritePointer (c) + pos;

            if (trackAllocations)
            {
                alloctrack::Scope scope;
                engine.processBlock (ptrs, numOuts, n);
            }
            else
            {
                engine.processBlock (ptrs, numOuts, n);
            }
        }
        return out;
    }

    /** Lets queued commands reach the audio thread. */
    void tick() { render (0.001); }

    Deck& deck (int i) { return engine.getDeck (i); }
};

/** Frequency from positive-going zero crossings (sub-sample accurate). */
double measureFrequency (const float* x, int start, int end, double sr)
{
    double first = -1, last = -1;
    int crossings = 0;
    for (int i = start + 1; i < end; ++i)
    {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = (i - 1) + x[i - 1] / (double) (x[i - 1] - x[i]);
            if (first < 0) first = t;
            last = t;
            ++crossings;
        }
    }
    return crossings > 1 ? (crossings - 1) * sr / (last - first) : 0.0;
}

double measureFrequency (const juce::AudioBuffer<float>& b, int ch, double fromSec, double toSec)
{
    return measureFrequency (b.getReadPointer (ch), (int) (fromSec * deviceRate), (int) (toSec * deviceRate), deviceRate);
}

float peak (const juce::AudioBuffer<float>& b, int ch, double fromSec, double toSec)
{
    return b.getMagnitude (ch, (int) (fromSec * deviceRate), (int) ((toSec - fromSec) * deviceRate));
}

/** Largest sample-to-sample jump: a click shows up as a jump far above the signal's natural slope. */
float maxStep (const juce::AudioBuffer<float>& b, int ch)
{
    const float* x = b.getReadPointer (ch);
    float m = 0.0f;
    for (int i = 1; i < b.getNumSamples(); ++i)
        m = juce::jmax (m, std::abs (x[i] - x[i - 1]));
    return m;
}

juce::File tempDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hype_engine_tests");
    d.createDirectory();
    return d;
}

//==============================================================================
class PlaybackTests : public juce::UnitTest
{
public:
    PlaybackTests() : juce::UnitTest ("Playback, tempo & pitch", "HYPE") {}

    void runTest() override
    {
        beginTest ("Plays a 44.1 kHz track on a 48 kHz device at the correct pitch and level");
        {
            Host h;
            h.deck (0).loadTrack (makeTrack (sine (440.0, 10.0, 44100.0), 44100.0));
            h.deck (0).play();
            auto out = h.render (2.0);
            expectWithinAbsoluteError (measureFrequency (out, 0, 0.5, 2.0), 440.0, 0.05);
            expectWithinAbsoluteError (peak (out, 0, 0.5, 2.0), 0.5f, 0.01f);
            expectWithinAbsoluteError (h.deck (0).getState().elapsedSeconds, 2.0, 0.01);
        }

        beginTest ("Vinyl mode: pitch follows tempo");
        {
            Host h;
            h.deck (0).loadTrack (makeTrack (sine (440.0, 10.0, 44100.0), 44100.0));
            h.deck (0).setTempoRatio (1.1);
            h.deck (0).play();
            auto out = h.render (2.0);
            expectWithinAbsoluteError (measureFrequency (out, 0, 0.5, 2.0), 484.0, 0.1);
            auto s = h.deck (0).getState();
            expect (! s.keyLockPathActive);
            expectWithinAbsoluteError (s.elapsedSeconds, 2.2, 0.01);
        }

        beginTest ("Key lock: tempo changes, pitch stays");
        {
            Host h;
            h.deck (0).loadTrack (makeTrack (sine (440.0, 20.0, 44100.0), 44100.0));
            h.deck (0).setKeyLock (true);
            h.deck (0).setTempoRatio (1.2);
            h.deck (0).play();
            auto out = h.render (3.0);
            expectWithinAbsoluteError (measureFrequency (out, 0, 1.0, 3.0), 440.0, 1.0);
            auto s = h.deck (0).getState();
            expect (s.keyLockPathActive);
            expectWithinAbsoluteError (s.elapsedSeconds, 3.6, 0.02);
            expectGreaterThan (peak (out, 0, 1.0, 3.0), 0.3f);
        }

        beginTest ("Key shift: pitch changes, tempo stays");
        {
            Host h;
            h.deck (0).loadTrack (makeTrack (sine (440.0, 20.0, 48000.0), 48000.0));
            h.deck (0).setKeyShiftSemitones (12.0);
            h.deck (0).play();
            auto out = h.render (3.0);
            expectWithinAbsoluteError (measureFrequency (out, 0, 1.0, 3.0), 880.0, 2.0);
            expectWithinAbsoluteError (h.deck (0).getState().elapsedSeconds, 3.0, 0.02);
        }

        beginTest ("Pitch fader range and pitch bend");
        {
            Host h;
            auto& d = h.deck (0);
            d.loadTrack (makeTrack (sine (440.0, 10.0, 48000.0), 48000.0, 120.0));
            d.setPitchRange (0.08);
            d.setPitchFader (1.0);
            expectWithinAbsoluteError (d.getTempoRatio(), 1.08, 1e-9);
            d.setPitchRange (0.16);
            expectWithinAbsoluteError (d.getTempoRatio(), 1.16, 1e-9);
            d.setPitchFader (-0.5);
            expectWithinAbsoluteError (d.getTempoRatio(), 0.92, 1e-9);
            d.setTempoRatio (1.04);
            expectWithinAbsoluteError (d.getPitchFader(), 0.25, 1e-9);

            d.setPitchBend (0.02);
            d.play();
            h.render (0.2);
            auto s = d.getState();
            expectWithinAbsoluteError (s.effectiveSpeed, 1.06, 1e-6);
            expectWithinAbsoluteError (s.tempoRatio, 1.04, 1e-6);
            expectWithinAbsoluteError (s.currentBpm, 127.2, 1e-3);
            expectWithinAbsoluteError (s.pitchPercent, 6.0, 1e-3);

            d.setPitchBend (0.0);
            h.render (0.2);
            expectWithinAbsoluteError (d.getState().effectiveSpeed, 1.04, 1e-6);
        }

        beginTest ("Track ends cleanly and reports it");
        {
            Host h;
            h.deck (0).loadTrack (makeTrack (sine (440.0, 1.0, 48000.0), 48000.0));
            h.deck (0).play();
            auto out = h.render (2.0);
            auto s = h.deck (0).getState();
            expect (! s.playing);
            expect (s.reachedEnd);
            expectWithinAbsoluteError (s.remainingSeconds, 0.0, 1e-6);
            expectLessThan (peak (out, 0, 1.05, 2.0), 1e-6f);   // allow 50 ms for the EQ filters' tail
        }

        beginTest ("Deterministic output regardless of device block size (vinyl path)");
        {
            auto renderWith = [] (int block)
            {
                Host h;
                h.blockSize = block;
                h.deck (0).loadTrack (makeTrack (sine (330.0, 5.0, 44100.0), 44100.0));
                h.deck (0).setTempoRatio (0.97);
                h.deck (0).play();
                return h.render (1.0);
            };
            auto ref = renderWith (256);
            for (int block : { 1, 7, 333, 2048 })
            {
                auto other = renderWith (block);
                float maxDiff = 0.0f;
                for (int i = 0; i < ref.getNumSamples(); ++i)
                    maxDiff = juce::jmax (maxDiff, std::abs (ref.getSample (0, i) - other.getSample (0, i)));
                expectLessThan (maxDiff, 1.0e-5f, "block size " + juce::String (block));
            }
        }
    }
};

//==============================================================================
class TransportTests : public juce::UnitTest
{
public:
    TransportTests() : juce::UnitTest ("Transport & cue", "HYPE") {}

    void runTest() override
    {
        beginTest ("Play / pause / stop");
        {
            Host h;
            auto& d = h.deck (0);
            d.loadTrack (makeTrack (sine (440.0, 30.0, 48000.0), 48000.0));
            h.tick();
            auto s = d.getState();
            expect (s.loaded);
            expectWithinAbsoluteError (s.durationSeconds, 30.0, 1e-9);
            expectWithinAbsoluteError (s.remainingSeconds, 30.0, 1e-3);

            d.play();
            h.render (1.0);
            d.pause();
            h.tick();
            const double pausedAt = d.getState().elapsedSeconds;
            expectWithinAbsoluteError (pausedAt, 1.0, 0.01);
            auto silent = h.render (0.5);
            expectLessThan (peak (silent, 0, 0.05, 0.5), 1e-6f);
            expectWithinAbsoluteError (d.getState().elapsedSeconds, pausedAt, 1e-9);

            d.togglePlayPause();
            h.render (1.0);
            s = d.getState();
            expect (s.playing);
            expectWithinAbsoluteError (s.elapsedSeconds, pausedAt + 1.0, 0.01);
            expectWithinAbsoluteError (s.elapsedSeconds + s.remainingSeconds, 30.0, 1e-9);
            expectWithinAbsoluteError (s.position, s.elapsedSeconds / 30.0, 1e-9);

            d.setCuePointSeconds (5.0);
            d.stop();
            h.tick();
            s = d.getState();
            expect (! s.playing);
            expectWithinAbsoluteError (s.elapsedSeconds, 5.0, 1e-6);
        }

        beginTest ("CDJ cue behaviour");
        {
            Host h;
            auto& d = h.deck (0);
            d.loadTrack (makeTrack (sine (440.0, 30.0, 48000.0), 48000.0));
            d.seekToSeconds (10.0);
            d.cueDown();           // paused away from cue -> sets cue here
            d.cueUp();
            h.tick();
            auto s = d.getState();
            expectWithinAbsoluteError (s.cueSeconds, 10.0, 1e-6);
            expect (! s.playing);

            d.cueDown();           // paused on the cue -> preview while held
            h.render (0.5);
            s = d.getState();
            expect (s.playing && s.cuePreviewing);
            d.cueUp();             // release -> back to cue, paused
            h.tick();
            s = d.getState();
            expect (! s.playing && ! s.cuePreviewing);
            expectWithinAbsoluteError (s.elapsedSeconds, 10.0, 1e-6);

            d.cueDown();           // preview, then PLAY latches playback
            h.render (0.2);
            d.play();
            d.cueUp();
            h.render (0.5);
            s = d.getState();
            expect (s.playing && ! s.cuePreviewing);

            d.cueDown();           // while playing -> back to cue and pause
            d.cueUp();
            h.tick();
            s = d.getState();
            expect (! s.playing);
            expectWithinAbsoluteError (s.elapsedSeconds, 10.0, 1e-6);
        }

        beginTest ("Set cue point / return to cue keeps play state / seek");
        {
            Host h;
            auto& d = h.deck (0);
            d.loadTrack (makeTrack (sine (440.0, 60.0, 48000.0), 48000.0));
            d.seekToSeconds (20.0);
            d.setCuePoint();
            d.play();
            h.render (2.0);
            d.returnToCue();
            h.render (0.5);
            auto s = d.getState();
            expect (s.playing);
            expectWithinAbsoluteError (s.elapsedSeconds, 20.5, 0.01);

            d.seekToNormalised (0.5);
            h.tick();
            expectWithinAbsoluteError (d.getState().elapsedSeconds, 30.0, 0.01);
            d.seekRelativeSeconds (-10.0);
            h.tick();
            expectWithinAbsoluteError (d.getState().elapsedSeconds, 20.0, 0.01);
            d.seekToSeconds (1000.0);   // clamped
            h.tick();
            expectLessOrEqual (d.getState().elapsedSeconds, 60.0);
        }

        beginTest ("Eject and load while playing");
        {
            Host h;
            auto& d = h.deck (0);
            d.loadTrack (makeTrack (sine (440.0, 10.0, 48000.0), 48000.0));
            d.play();
            h.render (0.5);
            d.loadTrack (makeTrack (sine (660.0, 10.0, 48000.0), 48000.0));
            auto out = h.render (0.2);
            auto s = d.getState();
            expect (s.loaded && ! s.playing);
            expectWithinAbsoluteError (s.elapsedSeconds, 0.0, 1e-9);
            expectLessThan (peak (out, 0, 0.05, 0.2), 1e-6f);
            expectLessThan (maxStep (out, 0), 0.06f);

            d.eject();
            h.tick();
            expect (! d.getState().loaded);
        }

        beginTest ("Sample-accurate scheduled start");
        {
            Host h;
            auto& d = h.deck (0);
            d.loadTrack (makeTrack (sine (440.0, 10.0, 48000.0, 0.5f), 48000.0));
            h.tick();
            const auto startAt = h.engine.getSampleClock() + 10007;  // not a block multiple
            const auto clockBefore = h.engine.getSampleClock();
            d.play (startAt);
            auto out = h.render (0.5);
            const int onset = (int) (startAt - clockBefore);
            expectLessThan (out.getMagnitude (0, 0, onset), 1e-9f);
            expectGreaterThan (out.getMagnitude (0, onset, 300), 0.1f);
        }
    }
};

//==============================================================================
class ClickFreeTests : public juce::UnitTest
{
public:
    ClickFreeTests() : juce::UnitTest ("Click-free operation", "HYPE") {}

    void runTest() override
    {
        // A 440 Hz sine at 0.5 has a natural max step of ~0.029 at 48 kHz (~0.035 at +20 %).
        // A discontinuity would produce a step of up to 1.0.
        constexpr float limit = 0.06f;

        for (bool keyLock : { false, true })
        {
            beginTest (juce::String ("Transport edits are de-clicked, key lock ") + (keyLock ? "on" : "off"));
            Host h;
            auto& d = h.deck (0);
            d.loadTrack (makeTrack (sine (440.0, 60.0, 44100.0), 44100.0));
            d.setKeyLock (keyLock);

            juce::AudioBuffer<float> all (numOuts, 0);
            auto append = [&] (double secs)
            {
                auto b = h.render (secs);
                const int old = all.getNumSamples();
                all.setSize (numOuts, old + b.getNumSamples(), true);
                for (int c = 0; c < numOuts; ++c)
                    all.copyFrom (c, old, b, c, 0, b.getNumSamples());
            };

            d.play();                      append (0.3);
            d.pause();                     append (0.2);
            d.play();                      append (0.3);
            d.seekToSeconds (17.123);      append (0.3);
            d.setTempoRatio (1.2);         append (0.3);   // tempo jump (smoothed)
            d.setTempoRatio (0.9);         append (0.3);
            d.setKeyLock (! keyLock);      append (0.3);   // path switch while playing
            d.setKeyLock (keyLock);        append (0.3);
            d.setPitchBend (0.08);         append (0.2);
            d.setPitchBend (0.0);          append (0.2);
            d.returnToCue();               append (0.3);
            d.setTrimDb (-12.0f);          append (0.2);
            d.setTrimDb (0.0f);            append (0.2);
            d.cueDown();                   append (0.2);   // playing -> back to cue, pause
            d.cueUp();
            d.cueDown();                   append (0.3);   // preview
            d.cueUp();                     append (0.2);
            d.stop();                      append (0.2);

            expectLessThan (maxStep (all, 0), limit);
            expectLessThan (maxStep (all, 1), limit);
        }

        beginTest ("Crossfader and fader moves are smoothed");
        {
            Host h;
            h.engine.getMixer().setCrossfaderAssign (0, Mixer::CrossfaderAssign::A);
            h.deck (0).loadTrack (makeTrack (sine (440.0, 10.0, 48000.0), 48000.0));
            h.deck (0).play();
            auto a = h.render (0.2);
            h.engine.getMixer().setCrossfader (1.0f);  // hard cut
            auto b = h.render (0.2);
            h.engine.getMixer().setChannelVolume (0, 1.0f);
            h.engine.getMixer().setCrossfader (0.0f);
            h.engine.getMixer().setChannelVolume (0, 0.0f);
            auto c = h.render (0.2);
            expectLessThan (maxStep (a, 0), limit);
            expectLessThan (maxStep (b, 0), limit);
            expectLessThan (maxStep (c, 0), limit);
            expectLessThan (peak (b, 0, 0.1, 0.2), 1e-6f);
        }
    }
};

//==============================================================================
class MixerRoutingTests : public juce::UnitTest
{
public:
    MixerRoutingTests() : juce::UnitTest ("Mixer & routing", "HYPE") {}

    void runTest() override
    {
        beginTest ("Two decks play independently and simultaneously");
        {
            Host h;
            auto& mixer = h.engine.getMixer();
            h.deck (0).loadTrack (makeTrack (sine (440.0, 30.0, 44100.0), 44100.0));
            h.deck (1).loadTrack (makeTrack (sine (660.0, 30.0, 48000.0), 48000.0));
            mixer.setDirectOutput (0, 4, 5);
            mixer.setDirectOutput (1, 6, 7);

            h.deck (0).setTempoRatio (1.05);
            h.deck (1).setKeyLock (true);
            h.deck (1).setTempoRatio (0.9);
            h.deck (1).seekToSeconds (10.0);
            h.deck (0).play();
            h.deck (1).play();
            auto out = h.render (3.0);

            expectWithinAbsoluteError (measureFrequency (out, 4, 1.0, 3.0), 462.0, 0.2);
            expectWithinAbsoluteError (measureFrequency (out, 6, 1.0, 3.0), 660.0, 1.5);

            auto a = h.deck (0).getState(), b = h.deck (1).getState();
            expectWithinAbsoluteError (a.elapsedSeconds, 3.15, 0.01);
            expectWithinAbsoluteError (b.elapsedSeconds, 12.7, 0.02);

            h.deck (0).pause();          // stopping A must not affect B
            h.render (1.0);
            expect (! h.deck (0).getState().playing);
            expect (h.deck (1).getState().playing);
            expectWithinAbsoluteError (h.deck (1).getState().elapsedSeconds, 13.6, 0.02);
        }

        beginTest ("Channel fader, trim gain and crossfader");
        {
            Host h;
            auto& mixer = h.engine.getMixer();
            h.deck (0).loadTrack (makeTrack (sine (440.0, 10.0, 48000.0), 48000.0));
            h.deck (0).play();
            h.render (0.1);

            mixer.setChannelVolume (0, 0.5f);
            auto out = h.render (0.5);
            expectWithinAbsoluteError (peak (out, 0, 0.1, 0.5), 0.5f * Mixer::faderToGain (0.5f), 0.005f);

            mixer.setChannelVolume (0, 1.0f);
            h.deck (0).setTrimDb (-6.0f);
            out = h.render (0.5);
            expectWithinAbsoluteError (peak (out, 0, 0.1, 0.5), 0.5f * juce::Decibels::decibelsToGain (-6.0f), 0.005f);

            h.deck (0).setTrimDb (0.0f);
            mixer.setCrossfaderAssign (0, Mixer::CrossfaderAssign::A);
            mixer.setCrossfader (0.5f);
            out = h.render (0.5);
            expectWithinAbsoluteError (peak (out, 0, 0.1, 0.5), 0.5f * std::cos (juce::MathConstants<float>::pi / 4), 0.005f);
            mixer.setCrossfader (0.0f);
            out = h.render (0.5);
            expectWithinAbsoluteError (peak (out, 0, 0.1, 0.5), 0.5f, 0.005f);

            float a, b;
            Mixer::crossfaderGains (0.5f, Mixer::CrossfaderCurve::Linear, a, b);
            expectEquals (a, 1.0f); expectEquals (b, 1.0f);
            Mixer::crossfaderGains (0.1f, Mixer::CrossfaderCurve::Cut, a, b);
            expectEquals (a, 1.0f); expectEquals (b, 1.0f);
            Mixer::crossfaderGains (0.0f, Mixer::CrossfaderCurve::Cut, a, b);
            expectEquals (b, 0.0f);
        }

        beginTest ("Headphone cue (PFL) routing and cue mix");
        {
            Host h;
            auto& mixer = h.engine.getMixer();
            h.deck (0).loadTrack (makeTrack (sine (440.0, 10.0, 48000.0), 48000.0));
            h.deck (1).loadTrack (makeTrack (sine (660.0, 10.0, 48000.0), 48000.0));
            mixer.setChannelVolume (1, 0.0f);   // B is pre-listened only
            mixer.setCueEnabled (1, true);
            h.deck (0).play();
            h.deck (1).play();
            h.render (0.1);
            auto out = h.render (1.0);
            expectWithinAbsoluteError (measureFrequency (out, 0, 0.1, 1.0), 440.0, 0.2);   // master = A
            expectWithinAbsoluteError (measureFrequency (out, 2, 0.1, 1.0), 660.0, 0.2);   // cue = B

            mixer.setCueMix (1.0f);  // headphones hear master only
            h.render (0.1);
            out = h.render (1.0);
            expectWithinAbsoluteError (measureFrequency (out, 2, 0.1, 1.0), 440.0, 0.2);

            mixer.setCueOutput (-1, -1);
            out = h.render (0.5);
            expectLessThan (peak (out, 2, 0.0, 0.5), 1e-9f);
        }

        beginTest ("Safety limiter keeps the master below 0 dBFS");
        {
            Host h;
            h.engine.getMixer().setLimiterEnabled (true);
            h.engine.getMixer().setMasterGain (2.0f);
            for (int d = 0; d < 2; ++d)
            {
                h.deck (d).loadTrack (makeTrack (sine (440.0, 5.0, 48000.0, 0.9f), 48000.0));
                h.deck (d).setTrimDb (6.0f);
                h.deck (d).play();
            }
            auto out = h.render (1.0);
            expectLessOrEqual (out.getMagnitude (0, 0, out.getNumSamples()), 0.99f);
            expect (h.engine.getMixer().getAndResetMeters().masterClipped);
        }
    }
};

//==============================================================================
class SyncTests : public juce::UnitTest
{
public:
    SyncTests() : juce::UnitTest ("Beat analysis & sync", "HYPE") {}

    void runTest() override
    {
        beginTest ("BPM and first beat detection");
        for (double bpm : { 96.0, 122.0, 128.0, 140.0, 174.0 })
        {
            auto b = clickTrack (bpm, 40.0, 44100.0, 0.25);
            auto r = analyseBeats (b, 44100.0);
            expectWithinAbsoluteError (r.bpm, bpm, 0.5, "bpm " + juce::String (bpm));
            double phaseErr = std::fmod (std::abs (r.firstBeatSeconds - 0.25), 60.0 / bpm);
            phaseErr = juce::jmin (phaseErr, 60.0 / bpm - phaseErr);
            expectLessThan (phaseErr, 0.012, "first beat " + juce::String (bpm));
        }

        beginTest ("One-shot sync matches tempo and beat phase");
        {
            Host h;
            h.deck (0).loadTrack (makeTrack (clickTrack (120.0, 60.0, 44100.0), 44100.0, 120.0, 0.0));
            h.deck (1).loadTrack (makeTrack (clickTrack (128.0, 60.0, 48000.0, 0.1), 48000.0, 128.0, 0.1));
            h.deck (0).setTempoRatio (1.02);
            h.deck (0).play();
            h.deck (1).seekToSeconds (7.77);
            h.deck (1).play();
            h.render (1.0);

            h.engine.syncDeck (1, 0, true);
            h.render (5.0);
            auto a = h.deck (0).getState(), b = h.deck (1).getState();
            expectWithinAbsoluteError (b.currentBpm, a.currentBpm, 1e-6);
            expectWithinAbsoluteError (b.tempoRatio, 120.0 * 1.02 / 128.0, 1e-9);
            double dp = std::abs (a.beatPhase - b.beatPhase);
            dp = juce::jmin (dp, 1.0 - dp);
            expectLessThan (dp, 0.002);
        }

        beginTest ("Half/double tempo matching");
        {
            Host h;
            h.deck (0).loadTrack (makeTrack (clickTrack (87.0, 20.0, 48000.0), 48000.0, 87.0));
            h.deck (1).loadTrack (makeTrack (clickTrack (172.0, 20.0, 48000.0), 48000.0, 172.0));
            h.tick();
            h.engine.syncDeck (1, 0, false);
            h.tick();
            expectWithinAbsoluteError (h.deck (1).getTempoRatio(), 174.0 / 172.0, 1e-9);
        }

        beginTest ("Sync lock follows the master's tempo changes and pitch bend");
        {
            Host h;
            h.deck (0).loadTrack (makeTrack (clickTrack (124.0, 60.0, 48000.0), 48000.0, 124.0));
            h.deck (1).loadTrack (makeTrack (clickTrack (130.0, 60.0, 48000.0), 48000.0, 130.0));
            h.engine.setSyncLock (1, 0);
            h.deck (0).play();
            h.deck (1).play();
            h.render (0.5);
            h.deck (0).setTempoRatio (1.05);
            h.render (0.5);
            expectWithinAbsoluteError (h.deck (1).getState().currentBpm, 124.0 * 1.05, 1e-6);
            h.deck (0).setPitchBend (0.03);
            h.render (0.5);
            expectWithinAbsoluteError (h.deck (1).getState().currentBpm, h.deck (0).getState().currentBpm, 1e-6);
            h.deck (0).setPitchBend (0.0);
            h.engine.setSyncLock (1, -1);
            h.tick();
            h.deck (0).setTempoRatio (1.0);
            h.render (0.5);
            // Released: the slave keeps the tempo it had at release and no longer follows.
            expectWithinAbsoluteError (h.deck (1).getState().currentBpm, 124.0 * 1.05, 1e-6);
            expectWithinAbsoluteError (h.deck (0).getState().currentBpm, 124.0, 1e-6);
        }

        beginTest ("Quantised start lands on the master's beat");
        {
            Host h;
            h.deck (0).loadTrack (makeTrack (clickTrack (126.0, 60.0, 48000.0), 48000.0, 126.0, 0.0));
            h.deck (1).loadTrack (makeTrack (clickTrack (122.0, 60.0, 44100.0, 0.2), 44100.0, 122.0, 0.2));
            h.engine.getMixer().setDirectOutput (0, 4, 5);
            h.engine.getMixer().setDirectOutput (1, 6, 7);
            h.deck (0).play();
            h.render (1.3);
            h.deck (1).seekToSeconds (3.31);
            h.engine.playInSync (1, 0);
            h.render (3.0);
            auto a = h.deck (0).getState(), b = h.deck (1).getState();
            expect (b.playing);
            double dp = std::abs (a.beatPhase - b.beatPhase);
            dp = juce::jmin (dp, 1.0 - dp);
            expectLessThan (dp, 0.002);
            expectWithinAbsoluteError (b.currentBpm, 126.0, 1e-6);
        }
    }
};

//==============================================================================
class RealtimeSafetyTests : public juce::UnitTest
{
public:
    RealtimeSafetyTests() : juce::UnitTest ("Real-time safety & performance", "HYPE") {}

    void runTest() override
    {
        beginTest ("No heap allocation on the audio thread");
        {
            Host h;
            auto t1 = makeTrack (sine (440.0, 60.0, 44100.0), 44100.0, 120.0);
            auto t2 = makeTrack (clickTrack (128.0, 60.0, 48000.0), 48000.0, 128.0);
            h.deck (0).loadTrack (t1);
            h.deck (1).loadTrack (t2);
            h.render (0.1);

            alloctrack::allocations = 0;
            auto& a = h.deck (0);
            auto& b = h.deck (1);
            a.play();                      h.render (0.3, true);
            b.setKeyLock (true); b.play(); h.render (0.3, true);
            a.seekToSeconds (12.0);        h.render (0.3, true);
            b.setTempoRatio (1.3);         h.render (0.3, true);
            a.setKeyShiftSemitones (-3.0); h.render (0.3, true);
            h.engine.syncDeck (0, 1, true);h.render (0.3, true);
            h.engine.setSyncLock (0, 1);   h.render (0.3, true);
            a.cueDown(); a.cueUp();        h.render (0.3, true);
            a.cueDown();                   h.render (0.3, true);
            a.cueUp(); a.stop();           h.render (0.3, true);
            b.loadTrack (makeTrack (sine (220.0, 5.0, 48000.0), 48000.0)); // swap on the audio thread
                                           h.render (0.3, true);
            b.play(); b.eject();           h.render (0.3, true);
            h.engine.getMixer().setCrossfader (0.1f);
            h.engine.getMixer().setCueEnabled (0, true);
                                           h.render (0.3, true);
            expectEquals ((int) alloctrack::allocations.load(), 0);
        }

        beginTest ("CPU headroom: both decks in key-lock");
        {
            Host h;
            h.blockSize = 128;
            for (int d = 0; d < 2; ++d)
            {
                h.deck (d).loadTrack (makeTrack (clickTrack (124.0 + d, 120.0, 44100.0), 44100.0));
                h.deck (d).setKeyLock (true);
                h.deck (d).setTempoRatio (1.06);
                h.deck (d).play();
            }
            h.render (0.5);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            h.render (30.0);
            const double ms = juce::Time::getMillisecondCounterHiRes() - start;
            const double realtimeFactor = 30000.0 / ms;
            logMessage ("    30 s of 2-deck key-lock audio rendered in " + juce::String (ms, 1)
                        + " ms (" + juce::String (realtimeFactor, 1) + "x real time)");
           #if defined (__SANITIZE_ADDRESS__) || defined (__SANITIZE_THREAD__)
            expectGreaterThan (realtimeFactor, 0.5);   // sanitizers cost 5-15x
           #else
            expectGreaterThan (realtimeFactor, 5.0);
           #endif
        }
    }
};

//==============================================================================
class DecoderTests : public juce::UnitTest
{
public:
    DecoderTests() : juce::UnitTest ("File decoding & loading", "HYPE") {}

    void runTest() override
    {
        auto dir = tempDir();
        auto src = sine (440.0, 3.0, 44100.0, 0.5f);

        auto writeWithJuce = [&] (juce::AudioFormat& format, const juce::String& fileName, int bits)
        {
            auto f = dir.getChildFile (fileName);
            f.deleteFile();
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> w (format.createWriterFor (os.get(), 44100.0, 2, bits, {}, 0));
            if (w == nullptr)
                return juce::File();
            os.release();
            w->writeFromAudioSampleBuffer (src, 0, src.getNumSamples());
            return f;
        };

        juce::WavAudioFormat wav;
        juce::AiffAudioFormat aiff;
        juce::FlacAudioFormat flac;
        juce::Array<juce::File> files { writeWithJuce (wav, "t.wav", 24),
                                        writeWithJuce (aiff, "t.aiff", 16),
                                        writeWithJuce (flac, "t.flac", 16) };

        // Lossy formats via the ffmpeg CLI if available.
        const auto wavPath = files[0].getFullPathName();
        juce::ChildProcess probe;
        const bool started = probe.start ("ffmpeg -version");
        const auto probeOut = started ? probe.readAllProcessOutput() : juce::String();
        const bool haveFfmpegCli = probeOut.contains ("ffmpeg");
        if (haveFfmpegCli)
        {
            for (auto [fileName, codec] : { std::pair { "t.mp3", "libmp3lame" }, std::pair { "t.m4a", "aac" } })
            {
                auto f = dir.getChildFile (fileName);
                f.deleteFile();
                juce::ChildProcess p;
                p.start (juce::StringArray { "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", wavPath,
                                             "-c:a", codec, "-b:a", "192k", f.getFullPathName() });
                const auto log = p.readAllProcessOutput();
                if (f.existsAsFile())
                    files.add (f);
                else
                    logMessage ("    ffmpeg failed for " + juce::String (fileName) + ": " + log);
            }
        }
        else
        {
            logMessage ("    ffmpeg CLI not found - skipping MP3/AAC fixtures");
        }

        for (auto& f : files)
        {
            if (f == juce::File())
                continue;

            beginTest ("Decode and play " + f.getFileExtension());
            Host h;
            auto result = h.engine.loadTrackSync (0, f);
            expect (result.wasOk(), result.error);
            if (! result.wasOk())
                continue;

            logMessage ("    " + f.getFileName() + " -> " + result.track->getInfo().decoderName);
            expectWithinAbsoluteError (result.track->getInfo().getLengthSeconds(), 3.0, 0.08);
            expectEquals (result.track->getSampleRate(), 44100.0);

            h.deck (0).play();
            auto out = h.render (2.0);
            expectWithinAbsoluteError (measureFrequency (out, 0, 0.5, 2.0), 440.0, 0.2);
            expectWithinAbsoluteError (peak (out, 0, 0.5, 2.0), 0.5f, 0.03f);
        }

        beginTest ("Missing / corrupt files fail gracefully");
        {
            AudioFileDecoder decoder;
            expect (! decoder.decode (dir.getChildFile ("does_not_exist.mp3")).wasOk());
            auto junk = dir.getChildFile ("junk.mp3");
            junk.replaceWithText ("this is not audio");
            auto r = decoder.decode (junk);
            expect (! r.wasOk());
            expect (r.error.isNotEmpty());
        }

        beginTest ("Async loading: a newer request supersedes an older one");
        {
            Host h;
            auto a = files[0], b = files[1];
            std::atomic<int> done { 0 };
            h.engine.loadTrackAsync (0, a, [&] (int, const AudioFileDecoder::Result&) { ++done; });
            h.engine.loadTrackAsync (0, b, [&] (int, const AudioFileDecoder::Result&) { ++done; });
            for (int i = 0; i < 500 && done.load() < 2; ++i)
                juce::Thread::sleep (10);
            expectEquals (done.load(), 2);
            h.tick();
            auto s = h.deck (0).getState();
            expect (s.loaded);
            expectEquals (s.title, b.getFileNameWithoutExtension());
        }

        beginTest ("Supported formats");
        {
            auto wildcard = AudioFileDecoder().getWildcardForAllFormats();
            logMessage ("    " + wildcard);
            for (auto ext : { "*.wav", "*.aiff", "*.flac", "*.mp3" })
                expect (wildcard.contains (ext), ext);
           #if HYPE_HAS_FFMPEG || JUCE_MAC || JUCE_WINDOWS
            expect (wildcard.contains ("*.m4a"), "m4a");
           #endif
        }
    }
};


//==============================================================================
/** Amplitude of a sinusoid at `freq` (Goertzel), robust to other content. */
double toneAmplitude (const juce::AudioBuffer<float>& b, int ch, double freq, double fromSec, double toSec)
{
    const int start = (int) (fromSec * deviceRate), end = (int) (toSec * deviceRate);
    const double w = 2.0 * juce::MathConstants<double>::pi * freq / deviceRate;
    double re = 0, im = 0;
    const float* x = b.getReadPointer (ch);
    for (int i = start; i < end; ++i)
    {
        re += x[i] * std::cos (w * i);
        im -= x[i] * std::sin (w * i);
    }
    return 2.0 * std::sqrt (re * re + im * im) / (end - start);
}

double db (double gain) { return 20.0 * std::log10 (juce::jmax (1.0e-12, gain)); }

class MixerDspTests : public juce::UnitTest
{
public:
    MixerDspTests() : juce::UnitTest ("Mixer: EQ, crossfader, headphones, metering", "HYPE") {}

    double eqResponseDb (float lowDb, float midDb, float highDb, double freq, bool killMid = false)
    {
        Host h;
        auto& mixer = h.engine.getMixer();
        mixer.setEqGainDb (0, ThreeBandEq::low, lowDb);
        mixer.setEqGainDb (0, ThreeBandEq::mid, midDb);
        mixer.setEqGainDb (0, ThreeBandEq::high, highDb);
        mixer.setEqKill (0, ThreeBandEq::mid, killMid);
        h.deck (0).loadTrack (makeTrack (sine (freq, 3.0, 48000.0, 0.25f), 48000.0));
        h.deck (0).play();
        auto out = h.render (1.0);
        return db (toneAmplitude (out, 0, freq, 0.4, 1.0) / 0.25);
    }

    void runTest() override
    {
        beginTest ("EQ at unity is flat (isolator bands sum to an all-pass)");
        for (double f : { 40.0, 120.0, 250.0, 700.0, 2500.0, 6000.0, 14000.0 })
            expectWithinAbsoluteError (eqResponseDb (0, 0, 0, f), 0.0, 0.05, juce::String (f) + " Hz");

        beginTest ("EQ kills and boosts");
        expectLessThan (eqResponseDb (-100, 0, 0, 50.0), -40.0, "low kill @50 Hz");
        expectWithinAbsoluteError (eqResponseDb (-100, 0, 0, 8000.0), 0.0, 0.5, "low kill leaves highs");
        expectLessThan (eqResponseDb (0, 0, -100, 10000.0), -40.0, "high kill @10 kHz");
        expectWithinAbsoluteError (eqResponseDb (0, 0, -100, 60.0), 0.0, 0.5, "high kill leaves lows");
        expectLessThan (eqResponseDb (0, 0, 0, 800.0, true), -25.0, "mid kill @800 Hz");
        expectWithinAbsoluteError (eqResponseDb (6, 0, 0, 40.0), 6.0, 0.3, "low +6 dB");
        expectWithinAbsoluteError (eqResponseDb (0, -12, 0, 800.0), -12.0, 1.0, "mid -12 dB");
        expectEquals (ThreeBandEq::dbToBandGain (ThreeBandEq::minGainDb), 0.0f);

        beginTest ("EQ moves and kills are click-free");
        {
            Host h;
            auto& mixer = h.engine.getMixer();
            h.deck (0).loadTrack (makeTrack (sine (90.0, 5.0, 48000.0, 0.5f), 48000.0));
            h.deck (0).play();
            h.render (0.2);
            float worst = 0.0f;
            for (int i = 0; i < 6; ++i)
            {
                mixer.setEqKill (0, ThreeBandEq::low, i % 2 == 0);
                mixer.setEqGainDb (0, ThreeBandEq::low, i % 3 == 0 ? 6.0f : -10.0f);
                worst = juce::jmax (worst, maxStep (h.render (0.1), 0));
            }
            expectLessThan (worst, 0.02f);   // 90 Hz @ 0.5 has a natural step of ~0.006
        }

        beginTest ("Adjustable crossfader curve");
        for (float shape : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            float a, b;
            Mixer::crossfaderGains (0.0f, shape, false, a, b);
            expectEquals (a, 1.0f); expectWithinAbsoluteError (b, 0.0f, 1e-6f);
            Mixer::crossfaderGains (1.0f, shape, false, a, b);
            expectWithinAbsoluteError (a, 0.0f, 1e-6f); expectEquals (b, 1.0f);

            float prevA = 2.0f, prevB = -1.0f;
            for (float p = 0.0f; p <= 1.0f; p += 0.01f)
            {
                Mixer::crossfaderGains (p, shape, false, a, b);
                expectLessOrEqual (a, prevA + 1e-6f);
                expectGreaterOrEqual (b, prevB - 1e-6f);
                prevA = a; prevB = b;
            }
        }
        {
            float a, b, a2, b2;
            Mixer::crossfaderGains (0.5f, 0.0f, false, a, b);
            expectWithinAbsoluteError (a * a + b * b, 1.0f, 1e-5f);     // constant power
            Mixer::crossfaderGains (0.5f, 1.0f, false, a, b);
            expectEquals (a, 1.0f); expectEquals (b, 1.0f);             // scratch: both full
            Mixer::crossfaderGains (0.97f, 1.0f, false, a, b);
            expectGreaterThan (a, 0.9f);                                // cut-in within last ~3 %
            Mixer::crossfaderGains (0.2f, 0.4f, false, a, b);
            Mixer::crossfaderGains (0.2f, 0.4f, true, a2, b2);
            expectEquals (a, b2); expectEquals (b, a2);                 // reverse swaps sides
        }

        beginTest ("Crossfader blends the real deck signals");
        {
            Host h;
            auto& mixer = h.engine.getMixer();
            mixer.setCrossfaderAssign (0, Mixer::CrossfaderAssign::A);
            mixer.setCrossfaderAssign (1, Mixer::CrossfaderAssign::B);
            h.deck (0).loadTrack (makeTrack (sine (440.0, 10.0, 48000.0, 0.3f), 48000.0));
            h.deck (1).loadTrack (makeTrack (sine (660.0, 10.0, 48000.0, 0.3f), 48000.0));
            h.deck (0).play();
            h.deck (1).play();

            mixer.setCrossfader (0.0f); h.render (0.1);
            auto out = h.render (0.5);
            expectWithinAbsoluteError (toneAmplitude (out, 0, 440.0, 0.0, 0.5), 0.3, 0.005);
            expectLessThan (toneAmplitude (out, 0, 660.0, 0.0, 0.5), 1e-4);

            mixer.setCrossfader (1.0f); h.render (0.1);
            out = h.render (0.5);
            expectLessThan (toneAmplitude (out, 0, 440.0, 0.0, 0.5), 1e-4);
            expectWithinAbsoluteError (toneAmplitude (out, 0, 660.0, 0.0, 0.5), 0.3, 0.005);

            mixer.setCrossfader (0.5f); h.render (0.1);
            out = h.render (0.5);
            expectWithinAbsoluteError (toneAmplitude (out, 0, 440.0, 0.0, 0.5), 0.3 * 0.7071, 0.005);
            expectWithinAbsoluteError (toneAmplitude (out, 0, 660.0, 0.0, 0.5), 0.3 * 0.7071, 0.005);
        }

        beginTest ("Headphone cue: preview a deck without touching the master");
        {
            Host h;
            auto& mixer = h.engine.getMixer();
            mixer.setCrossfaderAssign (0, Mixer::CrossfaderAssign::A);
            mixer.setCrossfaderAssign (1, Mixer::CrossfaderAssign::B);
            mixer.setCrossfader (0.0f);          // audience hears deck A only
            h.deck (0).loadTrack (makeTrack (sine (440.0, 10.0, 48000.0, 0.3f), 48000.0));
            h.deck (1).loadTrack (makeTrack (sine (660.0, 10.0, 48000.0, 0.3f), 48000.0));
            h.deck (0).play();
            h.deck (1).play();
            h.render (0.1);
            auto before = h.render (0.5);

            // Re-render identically with deck B cued: master must be bit-identical.
            Host h2;
            auto& mixer2 = h2.engine.getMixer();
            mixer2.setCrossfaderAssign (0, Mixer::CrossfaderAssign::A);
            mixer2.setCrossfaderAssign (1, Mixer::CrossfaderAssign::B);
            mixer2.setCrossfader (0.0f);
            mixer2.setCueEnabled (1, true);
            h2.deck (0).loadTrack (makeTrack (sine (440.0, 10.0, 48000.0, 0.3f), 48000.0));
            h2.deck (1).loadTrack (makeTrack (sine (660.0, 10.0, 48000.0, 0.3f), 48000.0));
            h2.deck (0).play();
            h2.deck (1).play();
            h2.render (0.1);
            auto after = h2.render (0.5);

            float diff = 0.0f;
            for (int i = 0; i < before.getNumSamples(); ++i)
                diff = juce::jmax (diff, std::abs (before.getSample (0, i) - after.getSample (0, i)),
                                         std::abs (before.getSample (1, i) - after.getSample (1, i)));
            expectEquals (diff, 0.0f, "master unaffected by cue");
            expectLessThan (toneAmplitude (after, 0, 660.0, 0.0, 0.5), 1e-4, "next track not on master");
            expectWithinAbsoluteError (toneAmplitude (after, 2, 660.0, 0.0, 0.5), 0.3, 0.005, "next track in headphones");
            expectLessThan (toneAmplitude (after, 2, 440.0, 0.0, 0.5), 1e-4);

            // Headphone cue is also independent of the channel fader.
            mixer2.setChannelVolume (1, 0.0f);
            h2.render (0.1);
            auto faderDown = h2.render (0.5);
            expectWithinAbsoluteError (toneAmplitude (faderDown, 2, 660.0, 0.0, 0.5), 0.3, 0.005);

            // Both decks cued simultaneously.
            mixer2.setCueEnabled (0, true);
            h2.render (0.1);
            auto both = h2.render (0.5);
            expectWithinAbsoluteError (toneAmplitude (both, 2, 440.0, 0.0, 0.5), 0.3, 0.005);
            expectWithinAbsoluteError (toneAmplitude (both, 2, 660.0, 0.0, 0.5), 0.3, 0.005);

            // Headphone volume.
            mixer2.setCueGain (0.5f);
            h2.render (0.1);
            auto quieter = h2.render (0.5);
            expectWithinAbsoluteError (toneAmplitude (quieter, 3, 660.0, 0.0, 0.5), 0.15, 0.005);
        }

        beginTest ("Split cue: headphone cue left, master right");
        {
            Host h;
            auto& mixer = h.engine.getMixer();
            mixer.setCrossfaderAssign (1, Mixer::CrossfaderAssign::B);
            mixer.setCrossfader (0.0f);
            mixer.setCueEnabled (1, true);
            mixer.setSplitCue (true);
            h.deck (0).loadTrack (makeTrack (sine (440.0, 10.0, 48000.0, 0.3f), 48000.0));
            h.deck (1).loadTrack (makeTrack (sine (660.0, 10.0, 48000.0, 0.3f), 48000.0));
            h.deck (0).play();
            h.deck (1).play();
            h.render (0.1);
            auto out = h.render (0.5);
            expectWithinAbsoluteError (toneAmplitude (out, 2, 660.0, 0.0, 0.5), 0.3, 0.005);
            expectLessThan (toneAmplitude (out, 2, 440.0, 0.0, 0.5), 1e-4);
            expectWithinAbsoluteError (toneAmplitude (out, 3, 440.0, 0.0, 0.5), 0.3, 0.005);
            expectLessThan (toneAmplitude (out, 3, 660.0, 0.0, 0.5), 1e-4);

            // Single stereo device: split signal replaces the master on the shared pair.
            mixer.setCueOutput (0, 1);
            h.render (0.1);
            out = h.render (0.5);
            expectWithinAbsoluteError (toneAmplitude (out, 0, 660.0, 0.0, 0.5), 0.3, 0.005);
            expectWithinAbsoluteError (toneAmplitude (out, 1, 440.0, 0.0, 0.5), 0.3, 0.005);
            expectLessThan (toneAmplitude (out, 0, 440.0, 0.0, 0.5), 1e-4);
        }

        beginTest ("Master volume and master metering");
        {
            Host h;
            auto& mixer = h.engine.getMixer();
            h.deck (0).loadTrack (makeTrack (sine (440.0, 10.0, 48000.0, 0.8f), 48000.0));
            h.deck (0).play();
            mixer.setMasterGain (0.5f);
            h.render (0.1);
            mixer.getAndResetMeters();
            auto out = h.render (0.5);
            auto m = mixer.getAndResetMeters();
            expectWithinAbsoluteError (peak (out, 0, 0.0, 0.5), 0.4f, 0.005f);
            expectWithinAbsoluteError (m.masterPeakL, 0.4f, 0.005f);
            expectWithinAbsoluteError (m.masterPeakR, 0.4f, 0.005f);
            expectWithinAbsoluteError (m.masterRmsL, 0.4f / std::sqrt (2.0f), 0.02f);
            expectWithinAbsoluteError (m.channelPeak[0], 0.8f, 0.01f);   // pre-fader
            expect (! m.masterClipped);
            expectEquals (m.channelPeak[1], 0.0f);
        }
    }
};

//==============================================================================
class DeviceManagementTests : public juce::UnitTest
{
public:
    DeviceManagementTests() : juce::UnitTest ("Audio device management & recovery", "HYPE") {}

    struct Rig
    {
        AudioEngine engine;
        FakeAudioDeviceType* fake = nullptr;
        std::unique_ptr<AudioDeviceController> controller;

        Rig()
        {
            auto type = std::make_unique<FakeAudioDeviceType>();
            fake = type.get();
            engine.getDeviceManager().addAudioDeviceType (std::move (type));
            controller = std::make_unique<AudioDeviceController> (engine);
            controller->setWatchdogEnabled (false);
        }
        ~Rig() { controller->setWatchdogEnabled (false); engine.closeDevice(); }

        FakeAudioDeviceType::Device* device() { return fake->findOpenDevice (currentName()); }
        juce::String currentName()
        {
            auto* d = engine.getDeviceManager().getCurrentAudioDevice();
            return d != nullptr ? d->getName() : juce::String();
        }
    };

    void runTest() override
    {
        beginTest ("Enumerate, open with chosen rate / buffer / outputs");
        {
            Rig rig;
            expect (rig.controller->getDeviceTypes().contains (FakeAudioDeviceType::typeName));
            auto names = rig.controller->getDeviceNames (FakeAudioDeviceType::typeName);
            expect (names.contains ("Fake Interface 8ch") && names.contains ("Fake Laptop 2ch"));

            AudioDeviceController::Settings s;
            s.typeName = FakeAudioDeviceType::typeName;
            s.deviceName = "Fake Interface 8ch";
            s.sampleRate = 96000.0;
            s.bufferSize = 128;
            s.masterFirstChannel = 0;
            s.cueFirstChannel = 2;
            expectEquals (rig.controller->apply (s), juce::String());

            auto st = rig.controller->getStatus();
            expect (st.state == AudioDeviceController::State::running);
            expectEquals (st.deviceName, juce::String ("Fake Interface 8ch"));
            expectEquals (st.sampleRate, 96000.0);
            expectEquals (st.bufferSize, 128);
            expectEquals (st.numOutputChannels, 8);
            expectEquals (st.numActiveOutputs, 4);
            expectEquals (rig.controller->getOutputChannelNames().size(), 8);
            expect (rig.controller->getAvailableBufferSizes().contains (64));
            expectEquals (rig.engine.getSampleRate(), 96000.0);
        }

        beginTest ("Master and headphone outputs land on the assigned physical channels");
        {
            Rig rig;
            AudioDeviceController::Settings s;
            s.typeName = FakeAudioDeviceType::typeName;
            s.deviceName = "Fake Interface 8ch";
            s.masterFirstChannel = 4;   // outputs 5/6
            s.cueFirstChannel = 0;      // outputs 1/2
            expectEquals (rig.controller->apply (s), juce::String());

            auto& mixer = rig.engine.getMixer();
            mixer.setCrossfaderAssign (0, Mixer::CrossfaderAssign::Thru);
            mixer.setCrossfaderAssign (1, Mixer::CrossfaderAssign::Thru);
            mixer.setChannelVolume (1, 0.0f);   // deck B in headphones only
            mixer.setCueEnabled (1, true);
            rig.engine.getDeck (0).loadTrack (makeTrack (sine (440.0, 10.0, 48000.0, 0.3f), 48000.0));
            rig.engine.getDeck (1).loadTrack (makeTrack (sine (660.0, 10.0, 48000.0, 0.3f), 48000.0));
            rig.engine.getDeck (0).play();
            rig.engine.getDeck (1).play();

            juce::AudioBuffer<float> cap;
            rig.device()->pump (200, &cap);   // ~1 s at 256 / 48k
            expectWithinAbsoluteError (toneAmplitude (cap, 4, 440.0, 0.2, 1.0), 0.3, 0.01, "master L on out 5");
            expectWithinAbsoluteError (toneAmplitude (cap, 5, 440.0, 0.2, 1.0), 0.3, 0.01, "master R on out 6");
            expectWithinAbsoluteError (toneAmplitude (cap, 0, 660.0, 0.2, 1.0), 0.3, 0.01, "headphones on out 1");
            expectLessThan (toneAmplitude (cap, 0, 440.0, 0.2, 1.0), 1e-4);
            expectLessThan (toneAmplitude (cap, 4, 660.0, 0.2, 1.0), 1e-4);
            for (int ch : { 2, 3, 6, 7 })
                expectEquals (cap.getMagnitude (ch, 0, cap.getNumSamples()), 0.0f);

            // Reassign headphones to outputs 3/4 on the fly.
            expectEquals (rig.controller->setOutputAssignment (4, 2), juce::String());
            rig.device()->pump (100, &cap);
            expectWithinAbsoluteError (toneAmplitude (cap, 2, 660.0, 0.1, 0.5), 0.3, 0.01, "headphones moved to out 3");
            expectEquals (cap.getMagnitude (0, 0, cap.getNumSamples()), 0.0f);
        }

        beginTest ("Disconnection: falls back to the default device, then reconnects");
        {
            Rig rig;
            AudioDeviceController::Settings s;
            s.typeName = FakeAudioDeviceType::typeName;
            s.deviceName = "Fake Interface 8ch";
            s.cueFirstChannel = 2;
            rig.controller->apply (s);
            rig.engine.getDeck (0).loadTrack (makeTrack (sine (440.0, 30.0, 48000.0, 0.3f), 48000.0));
            rig.engine.getDeck (0).play();
            rig.device()->pump (20);
            const double posBefore = rig.engine.getDeck (0).getState().elapsedSeconds;

            rig.fake->setPlugged ("Fake Interface 8ch", false);
            rig.controller->poll();
            auto st = rig.controller->getStatus();
            expect (st.state == AudioDeviceController::State::fallback, st.message);
            expectEquals (st.deviceName, juce::String ("Fake Laptop 2ch"));
            expectEquals (rig.engine.getMixer().getCueOutputLeft(), -1, "no outputs 3/4 on a 2ch device");

            // Playback continues on the fallback device without losing position.
            rig.device()->pump (20);
            expectGreaterThan (rig.engine.getDeck (0).getState().elapsedSeconds, posBefore);
            expect (rig.engine.getDeck (0).getState().playing);

            rig.fake->setPlugged ("Fake Interface 8ch", true);
            for (int i = 0; i < 6; ++i)
                rig.controller->poll();
            st = rig.controller->getStatus();
            expect (st.state == AudioDeviceController::State::running, st.message);
            expectEquals (st.deviceName, juce::String ("Fake Interface 8ch"));
            expectEquals (rig.engine.getMixer().getCueOutputLeft(), 2, "headphone routing restored");
        }

        beginTest ("Disconnection without fallback: waits, then reconnects");
        {
            Rig rig;
            AudioDeviceController::Settings s;
            s.typeName = FakeAudioDeviceType::typeName;
            s.deviceName = "Fake Interface 8ch";
            s.fallbackToDefaultDevice = false;
            rig.controller->apply (s);

            rig.fake->setPlugged ("Fake Interface 8ch", false);
            rig.controller->poll();
            auto st = rig.controller->getStatus();
            expect (st.state == AudioDeviceController::State::recovering, st.message);
            expect (rig.engine.getDeviceManager().getCurrentAudioDevice() == nullptr, "must not play through another device");

            for (int i = 0; i < 6; ++i) rig.controller->poll();
            expect (rig.controller->getStatus().state == AudioDeviceController::State::recovering);

            rig.fake->setPlugged ("Fake Interface 8ch", true);
            for (int i = 0; i < 6; ++i) rig.controller->poll();
            expect (rig.controller->getStatus().state == AudioDeviceController::State::running);
        }

        beginTest ("Stalled audio callback is detected and the device restarted");
        {
            Rig rig;
            AudioDeviceController::Settings s;
            s.typeName = FakeAudioDeviceType::typeName;
            s.deviceName = "Fake Interface 8ch";
            rig.controller->apply (s);
            rig.device()->pump (4);
            rig.controller->poll();
            expect (rig.controller->getStatus().state == AudioDeviceController::State::running);

            for (int i = 0; i < 4; ++i)   // no callbacks for ~2 s of watchdog ticks
                rig.controller->poll();
            for (int i = 0; i < 6; ++i)
                rig.controller->poll();
            auto st = rig.controller->getStatus();
            expect (st.state == AudioDeviceController::State::running, st.message);
            expect (st.message.contains ("Reconnected"), st.message);
        }

        beginTest ("Driver error triggers recovery");
        {
            Rig rig;
            AudioDeviceController::Settings s;
            s.typeName = FakeAudioDeviceType::typeName;
            s.deviceName = "Fake Interface 8ch";
            rig.controller->apply (s);
            rig.device()->raiseError();
            rig.controller->poll();
            for (int i = 0; i < 6; ++i) rig.controller->poll();
            expect (rig.controller->getStatus().state == AudioDeviceController::State::running);
            expect (! rig.engine.hasDeviceError());
        }

        beginTest ("Silent clock keeps the engine running without a device, and hands over cleanly");
        {
            Rig rig;
            rig.engine.prepareToPlay (48000.0);
            rig.engine.getDeck (0).loadTrack (makeTrack (sine (440.0, 30.0, 48000.0, 0.3f), 48000.0));
            rig.engine.getDeck (0).play();
            rig.engine.setSilentClockEnabled (true);
            expect (rig.engine.isRunningOnSilentClock());
            juce::Thread::sleep (600);
            const double t1 = rig.engine.getDeck (0).getState().elapsedSeconds;
            expectWithinAbsoluteError (t1, 0.6, 0.25, "advances in real time");

            // Opening a device takes over: from now on only device callbacks advance the playhead.
            AudioDeviceController::Settings s;
            s.typeName = FakeAudioDeviceType::typeName;
            s.deviceName = "Fake Interface 8ch";
            expectEquals (rig.controller->apply (s), juce::String());
            expect (! rig.engine.isRunningOnSilentClock());
            const double t2 = rig.engine.getDeck (0).getState().elapsedSeconds;
            juce::Thread::sleep (300);
            expectWithinAbsoluteError (rig.engine.getDeck (0).getState().elapsedSeconds, t2, 1e-9, "silent clock idle");
            rig.device()->pump (94);   // ~0.5 s
            expectWithinAbsoluteError (rig.engine.getDeck (0).getState().elapsedSeconds - t2, 0.5, 0.02);

            // Device stops (unplugged, no fallback): the silent clock resumes.
            rig.controller->setFallbackToDefaultDevice (false);
            rig.fake->setPlugged ("Fake Interface 8ch", false);
            rig.controller->poll();
            expect (rig.engine.isRunningOnSilentClock());
            const double t3 = rig.engine.getDeck (0).getState().elapsedSeconds;
            juce::Thread::sleep (400);
            expectGreaterThan (rig.engine.getDeck (0).getState().elapsedSeconds, t3 + 0.2);
            rig.engine.setSilentClockEnabled (false);
        }

        beginTest ("Settings persist through XML");
        {
            std::unique_ptr<juce::XmlElement> xml;
            {
                Rig rig;
                AudioDeviceController::Settings s;
                s.typeName = FakeAudioDeviceType::typeName;
                s.deviceName = "Fake Interface 8ch";
                s.sampleRate = 44100.0;
                s.bufferSize = 64;
                s.masterFirstChannel = 2;
                s.cueFirstChannel = 6;
                rig.controller->apply (s);
                xml = rig.controller->createStateXml();
            }
            Rig rig;
            expectEquals (rig.controller->restoreFromXml (xml.get()), juce::String());
            auto st = rig.controller->getStatus();
            expectEquals (st.deviceName, juce::String ("Fake Interface 8ch"));
            expectEquals (st.sampleRate, 44100.0);
            expectEquals (st.bufferSize, 64);
            expectEquals (rig.engine.getMixer().getMasterOutputLeft(), 2);
            expectEquals (rig.engine.getMixer().getCueOutputLeft(), 6);
        }
    }
};

//==============================================================================
class WaveformTests : public juce::UnitTest
{
public:
    WaveformTests() : juce::UnitTest ("Waveform analysis", "HYPE") {}

    void runTest() override
    {
        beginTest ("Bins, peaks and frequency bands");
        juce::AudioBuffer<float> b (2, 48000 * 4);
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const double t = i / 48000.0;
            const float v = t < 2.0 ? 0.8f * (float) std::sin (2 * juce::MathConstants<double>::pi * 60.0 * t)
                                    : 0.4f * (float) std::sin (2 * juce::MathConstants<double>::pi * 6000.0 * t);
            b.setSample (0, i, v);
            b.setSample (1, i, v);
        }
        auto w = WaveformData::build (b.getReadPointer (0), b.getReadPointer (1), b.getNumSamples(), 48000.0);
        expectWithinAbsoluteError (w->binsPerSecond(), 150.0, 1.0);
        expectWithinAbsoluteError (w->getNumBins(), 600, 2);

        const int lowBin = (int) (1.0 * w->binsPerSecond()), highBin = (int) (3.0 * w->binsPerSecond());
        expectWithinAbsoluteError ((int) w->peak[(size_t) lowBin], 204, 4);    // 0.8
        expectWithinAbsoluteError ((int) w->peak[(size_t) highBin], 102, 4);   // 0.4
        expectGreaterThan ((int) w->low[(size_t) lowBin], 3 * (int) w->high[(size_t) lowBin]);
        expectGreaterThan ((int) w->high[(size_t) highBin], 3 * (int) w->low[(size_t) highBin]);

        beginTest ("Loader attaches waveform and the deck exposes it for display");
        {
            Host h;
            auto f = tempDir().getChildFile ("wf.wav");
            f.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
            std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (os.get(), 48000.0, 2, 16, {}, 0));
            os.release();
            writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            writer.reset();

            expect (h.deck (0).getDisplayTrack() == nullptr);
            auto r = h.engine.loadTrackSync (0, f);
            expect (r.wasOk() && r.track->getWaveform() != nullptr);
            expect (h.deck (0).getDisplayTrack() == nullptr, "not shown before the audio thread swaps it in");
            h.tick();
            auto shown = h.deck (0).getDisplayTrack();
            expect (shown != nullptr && shown->getWaveform() != nullptr);
            h.deck (0).eject();
            h.tick();
            expect (h.deck (0).getDisplayTrack() == nullptr);
        }
    }
};

PlaybackTests       playbackTests;
TransportTests      transportTests;
ClickFreeTests      clickFreeTests;
MixerRoutingTests   mixerRoutingTests;
SyncTests           syncTests;
RealtimeSafetyTests realtimeSafetyTests;
DecoderTests        decoderTests;
MixerDspTests       mixerDspTests;
DeviceManagementTests deviceManagementTests;
WaveformTests       waveformTests;
} // namespace

int main (int argc, char** argv)
{
    juce::MessageManager::getInstance();   // the device manager needs a message thread (this one)
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);

    if (argc > 1)
    {
        // Run only tests whose name contains the argument, e.g. "Sync".
        juce::Array<juce::UnitTest*> selected;
        for (auto* t : juce::UnitTest::getAllTests())
            if (t->getName().containsIgnoreCase (argv[1]))
                selected.add (t);
        runner.runTests (selected);
    }
    else
    {
        runner.runAllTests();
    }

    int failures = 0, passes = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        failures += runner.getResult (i)->failures;
        passes += runner.getResult (i)->passes;
    }

    std::cout << "\n==== " << passes << " checks passed, " << failures << " failed ====\n";
    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();
    return failures == 0 ? 0 : 1;
}
