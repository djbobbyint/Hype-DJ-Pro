// HYPE DJ PRO - Phase 1 acceptance tests.
//
// End-to-end: real audio files on disk -> TrackLoader -> DeckEngine -> MixerEngine -> CueEngine ->
// OutputRouter -> the audio device callback of a simulated multi-output interface (FakeAudioDevice).
// Controls go through the public EngineInterface, exactly as the UI does. Outputs are captured
// per *physical* device channel: master on outputs 1/2, headphones on outputs 3/4.
//
// Each numbered test corresponds to an item of the Phase 1 test plan (docs/TESTING.md).
//
//   hype_phase1_tests                 run everything (soak: HYPE_SOAK_MINUTES, default 30 audio-minutes)
//   hype_phase1_tests 13              run only test 13
//   HYPE_SOAK_MINUTES=240 hype_phase1_tests 13

#include "AllocationTracker.h"
#include "FakeAudioDevice.h"
#include "TestSignals.h"
#include "Engine/AudioEngine.h"

#include <iostream>
#include <map>

using namespace hype;
using namespace testsig;

namespace
{
constexpr double fpA = 783.99;   // fingerprint (lead tone) of track A, mid band
constexpr double fpB = 932.33;   // fingerprint (lead tone) of track B, mid band
constexpr float  padLevel = 0.08f;
constexpr int masterL = 0, masterR = 1, phonesL = 2, phonesR = 3;

std::map<int, bool> results;   // test number -> passed (for the summary)

//==============================================================================
struct Fixtures
{
    juce::File dir, trackA, trackB;
    juce::File tone[3];               // 440 Hz tone at 44.1 / 48 / 96 kHz
    juce::File soak[4];               // clean tonal tracks for click detection
    static constexpr double toneRates[3] { 44100.0, 48000.0, 96000.0 };

    Fixtures()
    {
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hype_phase1");
        dir.createDirectory();
        const bool ff = ffmpegAvailable();

        TrackSpec a;
        a.bpm = 126.0; a.sampleRate = 44100.0; a.seconds = 60.0; a.seed = 11;
        trackA = dir.getChildFile (ff ? "Track A - 126 BPM v2.mp3" : "Track A - 126 BPM v2.wav");
        if (! trackA.existsAsFile()) writeAudioFile (makeMusic (a), a.sampleRate, trackA);

        TrackSpec b;
        b.bpm = 122.0; b.sampleRate = 48000.0; b.seconds = 60.0; b.seed = 22;
        b.chord[0] = 293.66; b.chord[1] = 369.99; b.chord[2] = 440.0; b.bass = 73.42; b.lead = 932.33;
        trackB = dir.getChildFile ("Track B - 122 BPM v2.flac");
        if (! trackB.existsAsFile()) writeAudioFile (makeMusic (b), b.sampleRate, trackB);

        for (int i = 0; i < 3; ++i)
        {
            tone[i] = dir.getChildFile ("tone440_" + juce::String ((int) toneRates[i]) + ".wav");
            if (! tone[i].existsAsFile()) writeAudioFile (makeTones (10.0, toneRates[i], 440.0, 0.0, 0.4f), toneRates[i], tone[i]);
        }

        const double soakFreqs[4][2] { { 110.0, 165.0 }, { 98.0, 147.0 }, { 123.47, 185.0 }, { 130.81, 196.0 } };
        for (int i = 0; i < 4; ++i)
        {
            soak[i] = dir.getChildFile ("soak" + juce::String (i) + ".wav");
            const double sr = i % 2 == 0 ? 44100.0 : 48000.0;
            if (! soak[i].existsAsFile()) writeAudioFile (makeTones (180.0, sr, soakFreqs[i][0], soakFreqs[i][1]), sr, soak[i]);
        }
    }
};

Fixtures& fixtures() { static Fixtures f; return f; }

//==============================================================================
/** An engine driven by a simulated 8-output audio interface. */
struct Rig
{
    AudioEngine engine;
    EngineInterface& api = engine;
    FakeAudioDeviceType* fake = nullptr;
    double rate = 48000.0;

    explicit Rig (double sampleRate = 48000.0, int buffer = 256)
    {
        auto type = std::make_unique<FakeAudioDeviceType>();
        fake = type.get();
        engine.getDevices().getJuceDeviceManager().addAudioDeviceType (std::move (type));
        engine.getDevices().setWatchdogEnabled (false);

        DeviceSettings s;
        s.typeName = FakeAudioDeviceType::typeName;
        s.deviceName = "Fake Interface 8ch";
        s.sampleRate = sampleRate;
        s.bufferSize = buffer;
        s.masterFirstChannel = 0;     // outputs 1/2
        s.cueFirstChannel = 2;        // outputs 3/4
        error = api.devices().apply (s);
        rate = api.devices().getStatus().sampleRate;
        api.mixer().setLimiterEnabled (false);   // measure the raw mix
    }

    ~Rig() { engine.getDevices().setWatchdogEnabled (false); }

    juce::String error;

    FakeAudioDeviceType::Device* device()
    {
        auto* d = engine.getDevices().getJuceDeviceManager().getCurrentAudioDevice();
        return d != nullptr ? fake->findOpenDevice (d->getName()) : nullptr;
    }

    /** Runs the device callback for `seconds`, returns the physical outputs. */
    juce::AudioBuffer<float> run (double seconds, bool trackAllocations = false)
    {
        juce::AudioBuffer<float> out;
        if (auto* d = device())
        {
            const int blocks = juce::jmax (1, (int) std::ceil (seconds * d->sampleRate / d->bufferSize));
            d->pump (blocks, &out, trackAllocations);
        }
        return out;
    }

    /** Loads through the public API (background loader) and waits until the deck is live. */
    LoadResult load (int deck, const juce::File& f)
    {
        std::atomic<bool> done { false };
        LoadResult result;
        api.loadTrack (deck, f, [&] (int, const LoadResult& r) { result = r; done = true; });
        for (int i = 0; i < 3000 && ! done; ++i)
            juce::Thread::sleep (10);
        for (int i = 0; i < 10 && result.ok && api.deck (deck).getState().title != result.title; ++i)
            run (0.01);
        return result;
    }

    double fp (const juce::AudioBuffer<float>& b, int ch, double f) const
    {
        return toneAmplitude (b, ch, f, rate);
    }
};

juce::UnitTestRunner* theRunner = nullptr;

//==============================================================================
class Phase1Tests : public juce::UnitTest
{
public:
    Phase1Tests() : juce::UnitTest ("Phase 1 acceptance", "Phase1") {}

    juce::StringArray only;

    bool selected (int n) const { return only.isEmpty() || only.contains (juce::String (n)); }

    void begin (int n, const juce::String& title)
    {
        current = n;
        failuresAtStart = getNumFailures();
        beginTest (juce::String (n) + ". " + title);
    }

    void end()
    {
        results[current] = getNumFailures() == failuresAtStart;
    }

    int getNumFailures()
    {
        int f = 0;
        if (auto* r = theRunner)
            for (int i = 0; i < r->getNumResults(); ++i)
                f += r->getResult (i)->failures;
        return f;
    }

    void runTest() override
    {
        auto& fx = fixtures();
        logMessage ("Fixtures: " + fx.trackA.getFileName() + ", " + fx.trackB.getFileName());

        //==========================================================================
        // Tests 1-10 share one session, like a DJ would use the app.
        if (selected (1) || selected (2) || selected (3) || selected (4) || selected (5)
            || selected (6) || selected (7) || selected (9) || selected (10))
        {
            Rig rig;
            auto& api = rig.api;
            auto& A = api.deck (0);
            auto& B = api.deck (1);
            auto& mixer = api.mixer();
            auto& cue = api.cue();

            begin (1, "Load a track into Deck A");
            {
                expectEquals (rig.error, juce::String(), "device opened");
                const auto r = rig.load (0, fx.trackA);
                expect (r.ok, r.error);
                const auto s = A.getState();
                expect (s.loaded);
                expectEquals (s.title, fx.trackA.getFileNameWithoutExtension());
                expectWithinAbsoluteError (s.durationSeconds, 60.0, 0.1);
                expectWithinAbsoluteError (s.originalBpm, 126.0, 0.1);
                expect (A.getWaveform() != nullptr && A.getWaveform()->getNumBins() > 1000, "waveform");
                expect (! B.getState().loaded, "deck B untouched");
                logMessage ("    A: " + r.decoderName + ", " + juce::String (r.bpm, 2) + " BPM, " + juce::String (r.durationSeconds, 2) + " s");
            }
            end();

            begin (2, "Load another track into Deck B");
            {
                const auto r = rig.load (1, fx.trackB);
                expect (r.ok, r.error);
                const auto s = B.getState();
                expect (s.loaded);
                expectEquals (s.title, fx.trackB.getFileNameWithoutExtension());
                expectWithinAbsoluteError (s.originalBpm, 122.0, 0.1);
                expectEquals (A.getState().title, fx.trackA.getFileNameWithoutExtension(), "deck A still has track A");
                expect (B.getWaveform() != nullptr && B.getWaveform() != A.getWaveform());
                logMessage ("    B: " + r.decoderName + ", " + juce::String (r.bpm, 2) + " BPM");
            }
            end();

            // Reference level of each fingerprint with only that deck audible at unity.
            double refA = 0, refB = 0;

            begin (3, "Play both tracks simultaneously");
            {
                mixer.setCrossfader (0.5f);    // smooth curve: each deck at -3 dB
                A.play();
                B.play();
                rig.run (0.3);
                const auto t0a = A.getState().elapsedSeconds, t0b = B.getState().elapsedSeconds;
                auto out = rig.run (2.0);
                const auto sa = A.getState(), sb = B.getState();
                expect (sa.playing && sb.playing);
                expectWithinAbsoluteError (sa.elapsedSeconds - t0a, 2.0, 0.02, "deck A advances in real time");
                expectWithinAbsoluteError (sb.elapsedSeconds - t0b, 2.0, 0.02, "deck B advances in real time");
                refA = rig.fp (out, masterL, fpA) / 0.7071;
                refB = rig.fp (out, masterL, fpB) / 0.7071;
                expectWithinAbsoluteError (refA, (double) padLevel, padLevel * 0.1, "track A audible on master");
                expectWithinAbsoluteError (refB, (double) padLevel, padLevel * 0.1, "track B audible on master");
                expectWithinAbsoluteError (rig.fp (out, masterR, fpA), rig.fp (out, masterL, fpA), 1e-4, "stereo");
                expect (allFinite (out));
                logMessage ("    both playing; fingerprints A " + juce::String (db (rig.fp (out, masterL, fpA)), 1)
                            + " dB, B " + juce::String (db (rig.fp (out, masterL, fpB)), 1) + " dB");
            }
            end();

            auto silentDb = [] (double amp, double ref) { return db (amp / ref); };

            begin (4, "Pause Deck A without stopping Deck B");
            {
                A.pause();
                rig.run (0.1);
                const auto a0 = A.getState().elapsedSeconds, b0 = B.getState().elapsedSeconds;
                auto out = rig.run (1.0);
                expect (! A.getState().playing && B.getState().playing);
                expectWithinAbsoluteError (A.getState().elapsedSeconds, a0, 1e-9, "A frozen");
                expectWithinAbsoluteError (B.getState().elapsedSeconds - b0, 1.0, 0.02, "B keeps playing");
                expectLessThan (silentDb (rig.fp (out, masterL, fpA), refA), -40.0, "A silent");
                expectWithinAbsoluteError (rig.fp (out, masterL, fpB) / 0.7071, refB, refB * 0.05, "B unchanged");
            }
            end();

            begin (5, "Pause Deck B without stopping Deck A");
            {
                A.play();
                B.pause();
                rig.run (0.1);
                const auto a0 = A.getState().elapsedSeconds, b0 = B.getState().elapsedSeconds;
                auto out = rig.run (1.0);
                expect (A.getState().playing && ! B.getState().playing);
                expectWithinAbsoluteError (B.getState().elapsedSeconds, b0, 1e-9, "B frozen");
                expectWithinAbsoluteError (A.getState().elapsedSeconds - a0, 1.0, 0.02, "A keeps playing");
                expectLessThan (silentDb (rig.fp (out, masterL, fpB), refB), -40.0, "B silent");
                expectWithinAbsoluteError (rig.fp (out, masterL, fpA) / 0.7071, refA, refA * 0.05, "A unchanged");
                B.play();
                rig.run (0.1);
            }
            end();

            begin (6, "Adjust each deck's volume independently");
            {
                mixer.setChannelVolume (0, 0.5f);       // audio taper: 0.25 = -12 dB
                rig.run (0.1);
                auto out = rig.run (1.0);
                expectWithinAbsoluteError (db (rig.fp (out, masterL, fpA) / 0.7071 / refA), -12.04, 0.3, "A at -12 dB");
                expectWithinAbsoluteError (db (rig.fp (out, masterL, fpB) / 0.7071 / refB), 0.0, 0.3, "B unchanged");

                mixer.setChannelVolume (0, 1.0f);
                mixer.setChannelVolume (1, 0.0f);
                rig.run (0.1);
                out = rig.run (1.0);
                expectWithinAbsoluteError (db (rig.fp (out, masterL, fpA) / 0.7071 / refA), 0.0, 0.3, "A back to unity");
                expectLessThan (silentDb (rig.fp (out, masterL, fpB), refB), -40.0, "B fader closed");

                mixer.setChannelVolume (1, 0.75f);      // -5 dB
                rig.run (0.1);
                out = rig.run (1.0);
                expectWithinAbsoluteError (db (rig.fp (out, masterL, fpB) / 0.7071 / refB), 20.0 * std::log10 (0.5625), 0.3, "B at 3/4");
                expectWithinAbsoluteError (db (rig.fp (out, masterL, fpA) / 0.7071 / refA), 0.0, 0.3, "A unaffected by B");
                mixer.setChannelVolume (1, 1.0f);
                rig.run (0.1);
            }
            end();

            begin (7, "Move the crossfader from left to right");
            {
                double prevA = 1e9, prevB = -1.0;
                juce::String table;
                for (int step = 0; step <= 8; ++step)
                {
                    const float p = step / 8.0f;
                    mixer.setCrossfader (p);
                    rig.run (0.08);
                    auto out = rig.run (0.5);
                    const double a = rig.fp (out, masterL, fpA) / refA, b = rig.fp (out, masterL, fpB) / refB;
                    float ga, gb;
                    MixerEngine::crossfaderGains (p, 0.0f, false, ga, gb);
                    expectWithinAbsoluteError (a, (double) ga, 0.02, "A gain at " + juce::String (p));
                    expectWithinAbsoluteError (b, (double) gb, 0.02, "B gain at " + juce::String (p));
                    expectLessOrEqual (a, prevA + 0.005, "A decreases");
                    expectGreaterOrEqual (b, prevB - 0.005, "B increases");
                    prevA = a; prevB = b;
                    table << juce::String (p, 3) << ": A " << juce::String (db (a), 1) << " dB, B " << juce::String (db (b), 1) << " dB\n";
                }
                logMessage ("    crossfader sweep (smooth curve):\n" + table.trimEnd());

                // Continuous sweep (as a hand would do it), checked for NaN/overs.
                juce::AudioBuffer<float> sweep;
                for (int i = 0; i <= 100; ++i)
                {
                    mixer.setCrossfader (i / 100.0f);
                    auto out = rig.run (0.01);
                    expect (allFinite (out));
                    expectLessThan (out.getMagnitude (0, out.getNumSamples()), 1.5f);
                }
                mixer.setCrossfader (0.5f);
                rig.run (0.1);
            }
            end();

            begin (9, "EQ controls");
            {
                B.pause();
                mixer.setCrossfader (0.0f);
                // Every measurement starts at the same track position and spans exactly 2 beats,
                // so the musical content is identical and only the EQ differs.
                auto measure = [&] (double& low, double& mid, double& high)
                {
                    A.seekToSeconds (10.0);
                    rig.run (0.25);
                    auto out = rig.run (2.0 * 60.0 / 126.0);
                    low = bandRms (out, masterL, 30.0, 80.0, rig.rate);
                    mid = rig.fp (out, masterL, fpA);
                    high = bandRms (out, masterL, 7000.0, 18000.0, rig.rate);
                };
                double low0, mid0, high0;
                measure (low0, mid0, high0);
                double l, m, h;

                mixer.setEqKill (0, EqBand::low, true);
                measure (l, m, h);
                expectLessThan (db (l / low0), -25.0, "low kill removes bass");
                expectWithinAbsoluteError (db (m / mid0), 0.0, 1.0, "low kill keeps mids");
                expectWithinAbsoluteError (db (h / high0), 0.0, 1.0, "low kill keeps highs");
                mixer.setEqKill (0, EqBand::low, false);

                mixer.setEqKill (0, EqBand::high, true);
                measure (l, m, h);
                expectLessThan (db (h / high0), -25.0, "high kill removes hats");
                expectWithinAbsoluteError (db (l / low0), 0.0, 1.0, "high kill keeps bass");
                mixer.setEqKill (0, EqBand::high, false);

                mixer.setEqKill (0, EqBand::mid, true);
                measure (l, m, h);
                expectLessThan (db (m / mid0), -20.0, "mid kill removes the pad");
                expectWithinAbsoluteError (db (l / low0), 0.0, 1.5, "mid kill keeps bass");
                mixer.setEqKill (0, EqBand::mid, false);

                mixer.setEqGainDb (0, EqBand::low, 6.0f);
                measure (l, m, h);
                expectWithinAbsoluteError (db (l / low0), 6.0, 1.0, "low +6 dB");
                mixer.setEqGainDb (0, EqBand::low, -12.0f);
                measure (l, m, h);
                expectWithinAbsoluteError (db (l / low0), -12.0, 1.0, "low -12 dB");
                mixer.setEqGainDb (0, EqBand::low, 0.0f);

                mixer.setEqGainDb (0, EqBand::high, ranges::eqMinDb);   // knob fully left = kill
                measure (l, m, h);
                expectLessThan (db (h / high0), -25.0, "high knob fully left kills");
                mixer.setEqGainDb (0, EqBand::high, 0.0f);

                measure (l, m, h);
                expectWithinAbsoluteError (db (l / low0), 0.0, 0.5, "restored low");
                expectWithinAbsoluteError (db (m / mid0), 0.0, 0.5, "restored mid");
                expectWithinAbsoluteError (db (h / high0), 0.0, 0.5, "restored high");

                // Deck B's EQ is independent of deck A's.
                B.play();
                mixer.setCrossfader (1.0f);
                rig.run (0.2);
                const double b0 = rig.fp (rig.run (1.0), masterL, fpB);
                mixer.setEqKill (0, EqBand::mid, true);
                const double b1 = rig.fp ((rig.run (0.15), rig.run (1.0)), masterL, fpB);
                expectWithinAbsoluteError (db (b1 / b0), 0.0, 0.3, "killing A's mids doesn't touch B");
                mixer.setEqKill (1, EqBand::mid, true);
                const double b2 = rig.fp ((rig.run (0.15), rig.run (1.0)), masterL, fpB);
                expectLessThan (db (b2 / b0), -20.0, "B's own mid kill works");
                mixer.setEqKill (0, EqBand::mid, false);
                mixer.setEqKill (1, EqBand::mid, false);
                mixer.setCrossfader (0.5f);
                rig.run (0.2);
            }
            end();

            begin (10, "Independent headphone monitoring");
            {
                // Audience hears deck A only; the DJ pre-listens to deck B.
                mixer.setCrossfader (0.0f);
                cue.setCueEnabled (1, true);
                rig.run (0.2);
                auto out = rig.run (1.0);
                expectGreaterThan (db (rig.fp (out, masterL, fpA) / refA), -0.5, "master: A");
                expectLessThan (db (rig.fp (out, masterL, fpB) / refB), -40.0, "master: no B");
                expectWithinAbsoluteError (db (rig.fp (out, phonesL, fpB) / refB), 0.0, 0.5, "headphones: B");
                expectWithinAbsoluteError (db (rig.fp (out, phonesR, fpB) / refB), 0.0, 0.5, "headphones R: B");
                expectLessThan (db (rig.fp (out, phonesL, fpA) / refA), -40.0, "headphones: no A");

                // Pre-fader: closing B's channel fader doesn't affect the headphones.
                mixer.setChannelVolume (1, 0.0f);
                rig.run (0.1);
                out = rig.run (1.0);
                expectWithinAbsoluteError (db (rig.fp (out, phonesL, fpB) / refB), 0.0, 0.5, "cue is pre-fader");
                mixer.setChannelVolume (1, 1.0f);

                // Both decks cued.
                cue.setCueEnabled (0, true);
                rig.run (0.1);
                out = rig.run (1.0);
                expectWithinAbsoluteError (db (rig.fp (out, phonesL, fpA) / refA), 0.0, 0.5, "both cued: A");
                expectWithinAbsoluteError (db (rig.fp (out, phonesL, fpB) / refB), 0.0, 0.5, "both cued: B");
                cue.setCueEnabled (0, false);

                // Headphone level is independent of master volume, and vice versa.
                cue.setCueLevel (0.5f);
                mixer.setMasterVolume (0.25f);
                rig.run (0.1);
                out = rig.run (1.0);
                expectWithinAbsoluteError (db (rig.fp (out, phonesL, fpB) / refB), -6.02, 0.3, "headphone level");
                expectWithinAbsoluteError (db (rig.fp (out, masterL, fpA) / refA), -12.04, 0.3, "master volume");
                cue.setCueLevel (1.0f);
                mixer.setMasterVolume (1.0f);

                // Cue/master blend.
                cue.setCueMix (1.0f);
                rig.run (0.1);
                out = rig.run (1.0);
                expectGreaterThan (db (rig.fp (out, phonesL, fpA) / refA), -0.5, "phones = master");
                expectLessThan (db (rig.fp (out, phonesL, fpB) / refB), -40.0);
                cue.setCueMix (0.0f);

                // Split cue: cue left, master right.
                cue.setSplitCue (true);
                rig.run (0.1);
                out = rig.run (1.0);
                expectGreaterThan (rig.fp (out, phonesL, fpB), refB * 0.9, "split: left = cue (B)");
                expectLessThan (db (rig.fp (out, phonesL, fpA) / refA), -40.0);
                expectGreaterThan (rig.fp (out, phonesR, fpA), refA * 0.9, "split: right = master (A)");
                expectLessThan (db (rig.fp (out, phonesR, fpB) / refB), -40.0);
                cue.setSplitCue (false);
                cue.setCueEnabled (1, false);

                // Bit-exact: the master is identical with and without headphone cueing.
                auto masterWith = [&] (bool cueB)
                {
                    AudioEngine e;
                    e.prepareToPlay (48000.0, 256);
                    for (int d = 0; d < 2; ++d)
                        e.getDeck (d).loadTrack (TrackLoader::decodeAndAnalyse (*std::make_unique<AudioFileDecoder>(),
                                                                                d == 0 ? fx.trackA : fx.trackB).track);
                    e.mixer().setCrossfader (0.3f);
                    e.cue().setCueEnabled (1, cueB);
                    e.cue().setCueLevel (2.0f);
                    e.deck (0).play();
                    e.deck (1).play();
                    juce::AudioBuffer<float> buf (4, 48000);
                    for (int pos = 0; pos < buf.getNumSamples(); pos += 256)
                    {
                        float* p[4];
                        for (int c = 0; c < 4; ++c) p[c] = buf.getWritePointer (c) + pos;
                        e.processBlock (p, 4, juce::jmin (256, buf.getNumSamples() - pos));
                    }
                    return buf;
                };
                auto m0 = masterWith (false), m1 = masterWith (true);
                float diff = 0.0f;
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < m0.getNumSamples(); ++i)
                        diff = juce::jmax (diff, std::abs (m0.getSample (c, i) - m1.getSample (c, i)));
                expectEquals (diff, 0.0f, "master bit-identical with/without cue");
                expectGreaterThan (m1.getMagnitude (2, 0, m1.getNumSamples()), 0.1f, "cue signal present");
                mixer.setCrossfader (0.5f);
            }
            end();
        }

        //==========================================================================
        if (selected (8))
        {
            begin (8, "Verify actual audio blending (sample-exact superposition)");
            {
                AudioFileDecoder dec;
                auto ta = TrackLoader::decodeAndAnalyse (dec, fx.trackA).track;
                auto tb = TrackLoader::decodeAndAnalyse (dec, fx.trackB).track;
                expect (ta != nullptr && tb != nullptr);

                // Three identical engines: both decks / only A audible / only B audible.
                auto render = [&] (float volA, float volB, float xf)
                {
                    AudioEngine e;
                    e.mixer().setLimiterEnabled (false);
                    e.mixer().setCrossfader (xf);
                    e.mixer().setChannelVolume (0, volA);
                    e.mixer().setChannelVolume (1, volB);
                    e.prepareToPlay (48000.0, 256);   // start from settled (not gliding) gains
                    e.getDeck (0).loadTrack (ta);
                    e.getDeck (1).loadTrack (tb);
                    e.deck (0).seekToSeconds (5.0);
                    e.deck (1).seekToSeconds (9.0);
                    e.deck (0).play();
                    e.deck (1).play();
                    juce::AudioBuffer<float> buf (2, 48000 * 3);
                    for (int pos = 0; pos < buf.getNumSamples(); pos += 256)
                    {
                        float* p[2] { buf.getWritePointer (0) + pos, buf.getWritePointer (1) + pos };
                        e.processBlock (p, 2, juce::jmin (256, buf.getNumSamples() - pos));
                    }
                    return buf;
                };

                for (float xf : { 0.0f, 0.25f, 0.5f, 0.8f, 1.0f })
                {
                    auto mix = render (1.0f, 1.0f, xf);
                    auto onlyA = render (1.0f, 0.0f, xf);
                    auto onlyB = render (0.0f, 1.0f, xf);
                    float err = 0.0f, mixPeak = 0.0f;
                    for (int c = 0; c < 2; ++c)
                        for (int i = 0; i < mix.getNumSamples(); ++i)
                        {
                            err = juce::jmax (err, std::abs (mix.getSample (c, i) - onlyA.getSample (c, i) - onlyB.getSample (c, i)));
                            mixPeak = juce::jmax (mixPeak, std::abs (mix.getSample (c, i)));
                        }
                    expectLessThan (err, 1.0e-5f, "output = A contribution + B contribution at xf " + juce::String (xf));

                    float ga, gb;
                    MixerEngine::crossfaderGains (xf, 0.0f, false, ga, gb);
                    const double a = toneAmplitude (mix, 0, fpA, 48000.0) / padLevel;
                    const double b = toneAmplitude (mix, 0, fpB, 48000.0) / padLevel;
                    expectWithinAbsoluteError (a, (double) ga, 0.02, "A share at " + juce::String (xf));
                    expectWithinAbsoluteError (b, (double) gb, 0.02, "B share at " + juce::String (xf));
                    logMessage ("    xf " + juce::String (xf, 2) + ": superposition error " + juce::String (err, 8)
                                + ", A share " + juce::String (a, 3) + ", B share " + juce::String (b, 3));
                }
            }
            end();
        }

        //==========================================================================
        if (selected (11))
        {
            begin (11, "Different sample rates (files 44.1/48/96 kHz x device 44.1/48/96 kHz)");
            for (double deviceRate : Fixtures::toneRates)
            {
                Rig rig (deviceRate, 256);
                expectEquals (rig.rate, deviceRate, "device rate applied");
                rig.api.mixer().setCrossfader (0.0f);   // deck A alone at unity
                for (int i = 0; i < 3; ++i)
                {
                    const auto r = rig.load (0, fx.tone[i]);
                    expect (r.ok, r.error);
                    auto& d = rig.api.deck (0);
                    expectWithinAbsoluteError (d.getState().durationSeconds, 10.0, 0.001);
                    d.setKeyLock (false);
                    d.setTempoRatio (1.0);
                    d.play();
                    rig.run (0.2);
                    const double t0 = d.getState().elapsedSeconds;
                    auto out = rig.run (1.0);
                    const double freq = [&]
                    {
                        // zero-crossing frequency estimate
                        const float* x = out.getReadPointer (masterL);
                        double first = -1, last = -1; int n = 0;
                        for (int k = 1; k < out.getNumSamples(); ++k)
                            if (x[k - 1] < 0 && x[k] >= 0)
                            {
                                const double t = (k - 1) + x[k - 1] / (double) (x[k - 1] - x[k]);
                                if (first < 0) first = t;
                                last = t; ++n;
                            }
                        return n > 1 ? (n - 1) * deviceRate / (last - first) : 0.0;
                    }();
                    const auto label = juce::String (Fixtures::toneRates[i] / 1000.0, 1) + " kHz file on "
                                     + juce::String (deviceRate / 1000.0, 1) + " kHz device";
                    expectWithinAbsoluteError (freq, 440.0, 0.05, "pitch: " + label);
                    expectWithinAbsoluteError (d.getState().elapsedSeconds - t0, out.getNumSamples() / deviceRate, 0.005, "timing: " + label);
                    expectWithinAbsoluteError (out.getMagnitude (masterL, 0, out.getNumSamples()), 0.25f, 0.003f, "level: " + label);

                    // Key-lock path at this rate: tempo +10 %, pitch must stay 440 Hz.
                    d.setKeyLock (true);
                    d.setTempoRatio (1.1);
                    rig.run (0.5);
                    out = rig.run (1.0);
                    expectWithinAbsoluteError (rig.fp (out, masterL, 440.0), 0.25, 0.02, "key lock keeps 440 Hz at full level: " + label);
                    expectLessThan (rig.fp (out, masterL, 484.0), 0.005, "no 484 Hz with key lock: " + label);
                    d.pause();
                    rig.run (0.05);
                    logMessage ("    " + label + ": " + juce::String (freq, 3) + " Hz");
                }
            }
            end();
        }

        //==========================================================================
        if (selected (12))
        {
            begin (12, "Audio device changes during playback");
            Rig rig (48000.0, 256);
            auto& api = rig.api;
            expect (rig.load (0, fx.trackA).ok);
            expect (rig.load (1, fx.trackB).ok);
            api.mixer().setCrossfader (0.5f);
            api.cue().setCueEnabled (1, true);
            api.deck (0).play();
            api.deck (1).play();
            rig.run (0.5);

            auto checkContinues = [&] (const juce::String& what, int expectMasterCh, int expectPhonesCh)
            {
                const double a0 = api.deck (0).getState().elapsedSeconds, b0 = api.deck (1).getState().elapsedSeconds;
                rig.run (0.1);
                auto out = rig.run (1.0);
                const auto st = api.devices().getStatus();
                expect (st.state == DeviceState::running || st.state == DeviceState::fallback, what + ": " + st.message);
                expect (api.deck (0).getState().playing && api.deck (1).getState().playing, what + ": both still playing");
                const double outSecs = out.getNumSamples() / juce::jmax (1.0, st.sampleRate);
                expectWithinAbsoluteError (api.deck (0).getState().elapsedSeconds - a0, 0.1 + outSecs, 0.03, what + ": A position continues");
                expectWithinAbsoluteError (api.deck (1).getState().elapsedSeconds - b0, 0.1 + outSecs, 0.03, what + ": B position continues");
                if (out.getNumChannels() > expectMasterCh)
                {
                    expectGreaterThan (toneAmplitude (out, expectMasterCh, fpA, st.sampleRate), padLevel * 0.6, what + ": master A");
                    expectGreaterThan (toneAmplitude (out, expectMasterCh, fpB, st.sampleRate), padLevel * 0.6, what + ": master B");
                }
                if (expectPhonesCh >= 0 && out.getNumChannels() > expectPhonesCh)
                    expectGreaterThan (toneAmplitude (out, expectPhonesCh, fpB, st.sampleRate), padLevel * 0.9, what + ": headphones B");
                logMessage ("    " + what + ": " + st.deviceName + " @ " + juce::String (st.sampleRate / 1000.0, 1) + " kHz / "
                            + juce::String (st.bufferSize) + " smp - OK");
            };

            auto settings = api.devices().getSettings();
            settings.bufferSize = 64;   expectEquals (api.devices().apply (settings), juce::String());
            checkContinues ("buffer 256 -> 64", masterL, phonesL);
            settings.bufferSize = 512;  expectEquals (api.devices().apply (settings), juce::String());
            checkContinues ("buffer 64 -> 512", masterL, phonesL);
            settings.sampleRate = 96000.0; expectEquals (api.devices().apply (settings), juce::String());
            checkContinues ("rate 48 -> 96 kHz", masterL, phonesL);
            settings.sampleRate = 44100.0; expectEquals (api.devices().apply (settings), juce::String());
            checkContinues ("rate 96 -> 44.1 kHz", masterL, phonesL);

            expectEquals (api.devices().setOutputAssignment (4, 6), juce::String());
            checkContinues ("master -> outs 5/6, headphones -> outs 7/8", 4, 6);
            expectEquals (api.devices().setOutputAssignment (0, 2), juce::String());

            auto laptop = api.devices().getSettings();
            laptop.deviceName = "Fake Laptop 2ch";
            expectEquals (api.devices().apply (laptop), juce::String());
            checkContinues ("switch to 2-channel device", masterL, -1);
            expectEquals (rig.engine.getRouter().getCueOutputLeft(), -1, "no headphone output on a 2ch device");

            auto back = api.devices().getSettings();
            back.deviceName = "Fake Interface 8ch";
            expectEquals (api.devices().apply (back), juce::String());
            checkContinues ("switch back to the 8-channel interface", masterL, phonesL);

            rig.fake->setPlugged ("Fake Interface 8ch", false);
            rig.engine.getDevices().poll();
            expect (api.devices().getStatus().state == DeviceState::fallback, api.devices().getStatus().message);
            checkContinues ("interface unplugged -> fallback device", masterL, -1);

            rig.fake->setPlugged ("Fake Interface 8ch", true);
            for (int i = 0; i < 6; ++i) rig.engine.getDevices().poll();
            expect (api.devices().getStatus().state == DeviceState::running);
            expectEquals (api.devices().getStatus().deviceName, juce::String ("Fake Interface 8ch"));
            checkContinues ("interface reconnected", masterL, phonesL);

            rig.device()->raiseError();
            for (int i = 0; i < 6; ++i) rig.engine.getDevices().poll();
            checkContinues ("driver error recovered", masterL, phonesL);
            end();
        }

        //==========================================================================
        if (selected (13))
        {
            const double minutes = juce::jmax (0.1, juce::SystemStats::getEnvironmentVariable ("HYPE_SOAK_MINUTES", "30").getDoubleValue());
            begin (13, "Long-duration playback stability (" + juce::String (minutes, 1) + " minutes of audio)");
            runSoak (minutes);
            end();
        }
    }

    //==========================================================================
    /** Both decks play for `minutes` of audio through the device callback while a seeded "virtual
        DJ" constantly operates every control and reloads tracks. Checks every block for NaN/Inf,
        overs, clicks and audio-thread allocations; checks clock, positions and memory. */
    void runSoak (double minutes)
    {
        auto& fx = fixtures();
        Rig rig (48000.0, 256);
        rig.api.mixer().setLimiterEnabled (true);
        auto& api = rig.api;
        std::minstd_rand rng (2024);
        auto rnd = [&rng] (double lo, double hi) { return std::uniform_real_distribution<double> (lo, hi) (rng); };

        int nextFile[2] { 0, 1 };
        for (int d = 0; d < 2; ++d)
            expect (rig.load (d, fx.soak[d]).ok);
        api.deck (0).play();
        api.deck (1).play();

        const int liveBefore = LoadedTrack::getNumLiveInstances();
        const auto clockStart = api.getSampleClock();
        const double totalSeconds = minutes * 60.0;
        const double step = 0.5;           // the "DJ" acts every half second of audio
        double t = 0.0;
        long actions = 0, reloads = 0, clicks = 0, nonFinite = 0, overs = 0;
        float worstStep = 0.0f, worstStepAllowed = 0.12f;
        int stalls = 0;
        double lastPos[2] { -1, -1 };
        alloctrack::allocations = 0;
        std::atomic<int> pendingLoads { 0 };

        const auto wallStart = juce::Time::getMillisecondCounterHiRes();
        double nextReport = 60.0;

        while (t < totalSeconds)
        {
            // One "DJ action" per step.
            const int action = (int) rnd (0, 12);
            const int d = (int) rnd (0, 2);
            auto& dk = api.deck (d);
            switch (action)
            {
                case 0: api.mixer().setCrossfader ((float) rnd (0, 1)); break;
                case 1: api.mixer().setEqGainDb (d, (EqBand) (int) rnd (0, 3), (float) rnd (-26, 6)); break;
                case 2: api.mixer().setEqKill (d, (EqBand) (int) rnd (0, 3), rnd (0, 1) < 0.5); break;
                case 3: api.mixer().setChannelVolume (d, (float) rnd (0.3, 1)); break;
                case 4: dk.setPitchFader (rnd (-1, 1)); break;
                case 5: dk.setKeyLock (rnd (0, 1) < 0.5); break;
                case 6: dk.seekToNormalised (rnd (0, 0.9)); break;
                case 7: api.cue().setCueEnabled (d, rnd (0, 1) < 0.5); api.cue().setCueMix ((float) rnd (0, 1)); break;
                case 8: dk.setPitchBend (rnd (-0.04, 0.04)); break;
                case 9: dk.setPitchBend (0.0); break;
                case 10: if (rnd (0, 1) < 0.5) api.setSyncLock (d, 1 - d); else api.setSyncLock (d, -1); break;
                case 11:
                    if (pendingLoads == 0 && rnd (0, 1) < 0.25)   // occasionally load the other track
                    {
                        dk.pause();
                        nextFile[d] = (nextFile[d] + 2) % 4;
                        ++pendingLoads;
                        api.loadTrack (d, fx.soak[nextFile[d]], [&pendingLoads] (int, const LoadResult&) { --pendingLoads; });
                        ++reloads;
                    }
                    break;
            }
            ++actions;

            // Keep both decks playing (a load stops the deck; the end of a track too).
            for (int k = 0; k < 2; ++k)
            {
                const auto s = api.deck (k).getState();
                if (s.loaded && ! s.playing)
                {
                    if (s.remainingSeconds < 5.0) api.deck (k).seekToSeconds (1.0);
                    api.deck (k).play();
                }
            }

            auto out = rig.run (step, true);
            t += out.getNumSamples() / rig.rate;

            // Per-block checks on master and headphones.
            for (int ch : { masterL, masterR, phonesL, phonesR })
            {
                const float* x = out.getReadPointer (ch);
                for (int i = 1; i < out.getNumSamples(); ++i)
                {
                    if (! std::isfinite (x[i])) { ++nonFinite; continue; }
                    if (ch <= masterR && std::abs (x[i]) > 1.0f) ++overs;
                    const float stepSize = std::abs (x[i] - x[i - 1]);
                    worstStep = juce::jmax (worstStep, stepSize);
                    if (stepSize > worstStepAllowed) ++clicks;
                }
            }

            // Playing decks must advance.
            for (int k = 0; k < 2; ++k)
            {
                const auto s = api.deck (k).getState();
                if (s.playing && s.effectiveSpeed > 0.5 && std::abs (s.elapsedSeconds - lastPos[k]) < 1.0e-6)
                    ++stalls;
                lastPos[k] = s.elapsedSeconds;
            }

            if (t >= nextReport)
            {
                nextReport += 60.0;
                std::cout << "      soak: " << juce::String (t / 60.0, 0) << "/" << juce::String (minutes, 0) << " min, "
                          << actions << " actions, " << reloads << " reloads, worst step " << juce::String (worstStep, 4)
                          << ", allocations " << alloctrack::allocations.load() << "\n" << std::flush;
            }
        }

        // Let a final in-flight load land, restart any deck it paused, then render a little more.
        for (int i = 0; i < 300 && pendingLoads > 0; ++i) juce::Thread::sleep (10);
        rig.run (0.05);
        for (int k = 0; k < 2; ++k)
            if (! api.deck (k).getState().playing)
                api.deck (k).play();
        rig.run (0.15);
        rig.engine.collectGarbage();
        const double wallSeconds = (juce::Time::getMillisecondCounterHiRes() - wallStart) / 1000.0;
        const auto renderedSamples = api.getSampleClock() - clockStart;

        logMessage ("    " + juce::String (t / 60.0, 1) + " min of audio in " + juce::String (wallSeconds, 1) + " s ("
                    + juce::String (t / wallSeconds, 1) + "x real time); " + juce::String (actions) + " control actions, "
                    + juce::String (reloads) + " track reloads");
        logMessage ("    worst sample step " + juce::String (worstStep, 4) + " (click threshold " + juce::String (worstStepAllowed)
                    + "), clicks " + juce::String (clicks) + ", non-finite " + juce::String (nonFinite) + ", overs " + juce::String (overs)
                    + ", stalls " + juce::String (stalls) + ", audio-thread allocations " + juce::String (alloctrack::allocations.load())
                    + ", decoded tracks in memory " + juce::String (LoadedTrack::getNumLiveInstances()));

        expectEquals ((int) nonFinite, 0, "no NaN/Inf");
        expectEquals ((int) overs, 0, "limiter prevents overs");
        expectEquals ((int) clicks, 0, "no discontinuities");
        expectEquals (stalls, 0, "playing decks always advance");
       #if ! defined (__SANITIZE_ADDRESS__) && ! defined (__SANITIZE_THREAD__)
        expectEquals ((int) alloctrack::allocations.load(), 0, "no allocation on the audio thread");
        expectGreaterThan (t / wallSeconds, 2.0, "faster than real time with full automation");
       #endif
        expectWithinAbsoluteError ((double) renderedSamples / rig.rate, t + 0.2, 0.01, "sample clock matches rendered audio");
        expectLessOrEqual (LoadedTrack::getNumLiveInstances(), juce::jmax (liveBefore, 2), "replaced tracks are freed (no leak)");
        expect (api.deck (0).getState().playing && api.deck (1).getState().playing, "both decks still playing at the end");
    }

private:
    int current = 0;
    int failuresAtStart = 0;
};

Phase1Tests phase1Tests;
} // namespace

int main (int argc, char** argv)
{
    juce::MessageManager::getInstance();

    // Test logs to stdout on every platform (JUCE's default logger uses the debugger on Windows).
    struct StdoutLogger : juce::Logger
    {
        void logMessage (const juce::String& m) override { std::cout << m << std::endl; }
    } stdoutLogger;
    juce::Logger::setCurrentLogger (&stdoutLogger);

    for (int i = 1; i < argc; ++i)
        phase1Tests.only.add (argv[i]);

    juce::UnitTestRunner runner;
    theRunner = &runner;
    runner.setAssertOnFailure (false);
    runner.runTests ({ &phase1Tests });

    const char* names[] = { "", "Load a track into Deck A", "Load another track into Deck B", "Play both tracks simultaneously",
                            "Pause Deck A without stopping Deck B", "Pause Deck B without stopping Deck A",
                            "Adjust each deck's volume independently", "Move the crossfader from left to right",
                            "Verify actual audio blending", "EQ controls", "Independent headphone monitoring",
                            "Different sample rates", "Audio device changes", "Long-duration playback stability" };

    std::cout << "\n================ PHASE 1 ACCEPTANCE SUMMARY ================\n";
    bool allPassed = true;
    for (int n = 1; n <= 13; ++n)
    {
        if (results.count (n) == 0)
            continue;
        std::cout << (results[n] ? "  PASS  " : "  FAIL  ") << juce::String (n).paddedLeft (' ', 2) << ". " << names[n] << "\n";
        allPassed = allPassed && results[n];
    }
    int failures = 0, passes = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        failures += runner.getResult (i)->failures;
        passes += runner.getResult (i)->passes;
    }
    std::cout << "  (" << passes << " checks passed, " << failures << " failed)\n";
    std::cout << "=============================================================\n";

    juce::Logger::setCurrentLogger (nullptr);
    juce::DeletedAtShutdown::deleteAll();
    juce::MessageManager::deleteInstance();
    return allPassed && failures == 0 ? 0 : 1;
}
