#pragma once

#include "EngineConfig.h"
#include "ThreeBandEq.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace hype
{
/**
    Two-channel DJ mixer and output router.

    Per channel (the Deck output is already post-trim/"gain"):

        deck -> [3-band EQ] --+--> [channel fader] --> [crossfader A/B/thru] --> MASTER bus
                              +--> (CUE on) --------------------------------------> CUE bus (pre-fader)
                              +--> direct output pair (external mixer mode) --------> device outs

    MASTER = sum * master volume -> safety limiter -> master output pair
    CUE    = blend (CUE bus, MASTER, cueMix) * headphone volume -> headphone output pair
             (split-cue mode: CUE bus in mono on the left, MASTER in mono on the right)

    The CUE bus is independent of the channel faders and crossfader: pre-listening a deck
    never changes what the audience hears, and both decks can be cued at once.

    Output channel numbers are *physical* device channels (0-based); -1 disables an output.
    All parameters are lock-free atomics smoothed per sample on the audio thread.
*/
class Mixer
{
public:
    enum class CrossfaderAssign { A, Thru, B };
    /** Presets for the continuous crossfader curve (see setCrossfaderCurveShape). */
    enum class CrossfaderCurve  { Smooth, Linear, Cut };

    Mixer();

    //--- channel strips (any thread) ---------------------------------------
    void setChannelVolume (int deck, float fader0to1);
    void setEqGainDb (int deck, int band, float decibels);   // band: ThreeBandEq::low/mid/high
    void setEqKill (int deck, int band, bool killed);
    void setCueEnabled (int deck, bool enabled);
    void setCrossfaderAssign (int deck, CrossfaderAssign assign);
    void setDirectOutput (int deck, int leftChannel, int rightChannel);

    float getChannelVolume (int deck) const noexcept      { return channels[deck].volume.load(); }
    float getEqGainDb (int deck, int band) const noexcept { return channels[deck].eqDb[band].load(); }
    bool  isEqKilled (int deck, int band) const noexcept  { return channels[deck].eqKill[band].load(); }
    bool  isCueEnabled (int deck) const noexcept          { return channels[deck].cue.load(); }
    CrossfaderAssign getCrossfaderAssign (int deck) const noexcept { return (CrossfaderAssign) channels[deck].assign.load(); }

    //--- crossfader / master (any thread) ----------------------------------
    void setCrossfader (float position0to1);          // 0 = full A, 1 = full B
    /** 0 = smooth constant-power blend ... 1 = sharp scratch cut (~3 % cut-in). */
    void setCrossfaderCurveShape (float shape0to1);
    void setCrossfaderCurve (CrossfaderCurve preset);
    void setCrossfaderReverse (bool reversed);        // "hamster" mode
    void setMasterGain (float linearGain);            // 0..2 (+6 dB)
    void setLimiterEnabled (bool);

    float getCrossfader() const noexcept          { return crossfader.load(); }
    float getCrossfaderCurveShape() const noexcept { return curveShape.load(); }
    bool  isCrossfaderReversed() const noexcept   { return reverse.load(); }
    float getMasterGain() const noexcept          { return masterGain.load(); }

    //--- headphones (any thread) -------------------------------------------
    void setCueMix (float cueToMaster0to1);          // 0 = cue only, 1 = master only
    void setCueGain (float linearGain);              // headphone volume, 0..2
    void setSplitCue (bool enabled);

    float getCueMix() const noexcept   { return cueMix.load(); }
    float getCueGain() const noexcept  { return cueGain.load(); }
    bool  isSplitCue() const noexcept  { return splitCue.load(); }

    //--- output routing (any thread) ---------------------------------------
    void setMasterOutput (int leftChannel, int rightChannel);
    void setCueOutput (int leftChannel, int rightChannel);
    int  getMasterOutputLeft() const noexcept  { return masterL.load(); }
    int  getMasterOutputRight() const noexcept { return masterR.load(); }
    int  getCueOutputLeft() const noexcept     { return cueL.load(); }
    int  getCueOutputRight() const noexcept    { return cueR.load(); }

    //--- metering ----------------------------------------------------------
    struct Meters
    {
        float channelPeak[config::numDecks] {};   // post-EQ, pre-fader (gain staging)
        float masterPeakL = 0, masterPeakR = 0;   // post limiter
        float masterRmsL = 0, masterRmsR = 0;
        float cuePeakL = 0, cuePeakR = 0;
        bool  masterClipped = false;              // limiter engaged (or >0 dBFS with limiter off)
        float limiterReductionDb = 0;             // max gain reduction since last read
    };
    /** Peak/RMS since the previous call (call from one UI thread). */
    Meters getAndResetMeters();

    //--- static helpers ----------------------------------------------------
    static float faderToGain (float fader0to1) noexcept;
    static void  crossfaderGains (float position, float shape, bool reversed, float& gainA, float& gainB) noexcept;
    static void  crossfaderGains (float position, CrossfaderCurve curve, float& gainA, float& gainB) noexcept;
    static float presetShape (CrossfaderCurve) noexcept;

    //--- audio thread ------------------------------------------------------
    void prepare (double sampleRate);

    /** Mixes the decks into the device outputs. `outputs` is indexed by physical channel;
        entries may be nullptr (inactive channel). Every non-null output is overwritten. */
    void process (const float* const* deckLeft, const float* const* deckRight,
                  float* const* outputs, int numOutputChannels, int numSamples) noexcept;

private:
    struct Channel
    {
        std::atomic<float> volume { 1.0f };
        std::atomic<float> eqDb[3] { {0.0f}, {0.0f}, {0.0f} };
        std::atomic<bool>  eqKill[3] { {false}, {false}, {false} };
        std::atomic<bool>  cue { false };
        std::atomic<int>   assign { (int) CrossfaderAssign::Thru };
        std::atomic<int>   directL { -1 }, directR { -1 };
        juce::SmoothedValue<float> gain;   // fader * crossfader
        juce::SmoothedValue<float> cueGain;
        ThreeBandEq eq;
    };

    static bool valid (int ch, int numChannels) noexcept { return ch >= 0 && ch < numChannels; }
    float channelTargetGain (int deck, float xfA, float xfB) const noexcept;

    Channel channels[config::numDecks];

    std::atomic<float> crossfader { 0.5f }, curveShape { 0.0f }, masterGain { 1.0f }, cueMix { 0.0f }, cueGain { 1.0f };
    std::atomic<bool>  reverse { false }, splitCue { false }, limiterEnabled { true };
    std::atomic<int>   masterL { 0 }, masterR { 1 }, cueL { 2 }, cueR { 3 };

    juce::SmoothedValue<float> masterGainSmoothed, cueMixSmoothed, cueGainSmoothed;

    // Safety limiter: instant attack, 150 ms release, -0.1 dBFS ceiling.
    float limiterGain = 1.0f, limiterRelease = 0.9999f;
    static constexpr float limiterCeiling = 0.989f;

    juce::AudioBuffer<float> eqBus, masterBus, cueBus;

    std::atomic<float> meterChannel[config::numDecks] {}, meterMasterL { 0 }, meterMasterR { 0 },
                       meterRmsL { 0 }, meterRmsR { 0 }, meterCueL { 0 }, meterCueR { 0 }, meterMinLimiterGain { 1.0f };
    std::atomic<bool>  meterClip { false };
};
} // namespace hype
