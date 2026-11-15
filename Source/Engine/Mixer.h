#pragma once

#include "EngineConfig.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace hype
{
/**
    Two-channel DJ mixer and output router.

    Signal flow per deck (Deck output is already post-trim):

        deck --+--> [channel fader] --> [crossfader A/B/thru] --> MASTER bus
               +--> (PFL cue enabled) --------------------------> CUE bus   (pre-fader)
               +--> direct output pair (external-mixer mode) ------------> device outs (pre-fader)

    MASTER = sum * master gain -> safety limiter -> master output pair
    CUE    = mix(CUE bus, MASTER, cueMix) * cue gain -> cue output pair (headphones)

    All parameters are lock-free atomics, smoothed per sample on the audio thread.
    Output channel indices of -1 disable that output.
*/
class Mixer
{
public:
    enum class CrossfaderAssign { A, Thru, B };
    enum class CrossfaderCurve  { Smooth, Linear, Cut };

    Mixer();

    //--- channel strips (any thread) ---------------------------------------
    void setChannelVolume (int deck, float fader0to1);
    void setCueEnabled (int deck, bool enabled);
    void setCrossfaderAssign (int deck, CrossfaderAssign assign);
    void setDirectOutput (int deck, int leftChannel, int rightChannel);

    float getChannelVolume (int deck) const noexcept { return channels[deck].volume.load(); }
    bool  isCueEnabled (int deck) const noexcept     { return channels[deck].cue.load(); }

    //--- master section (any thread) ---------------------------------------
    void setCrossfader (float position0to1);          // 0 = full A, 1 = full B
    void setCrossfaderCurve (CrossfaderCurve);
    void setMasterGain (float linearGain);
    void setCueMix (float cueToMaster0to1);          // 0 = cue only, 1 = master only
    void setCueGain (float linearGain);
    void setMasterOutput (int leftChannel, int rightChannel);
    void setCueOutput (int leftChannel, int rightChannel);
    void setLimiterEnabled (bool);

    float getCrossfader() const noexcept { return crossfader.load(); }

    struct Meters { float deck[config::numDecks] {}; float masterL = 0, masterR = 0, cue = 0; bool masterClipped = false; };
    /** Peak levels since the previous call. */
    Meters getAndResetMeters();

    //--- static helpers ----------------------------------------------------
    static float faderToGain (float fader0to1) noexcept;
    static void  crossfaderGains (float position, CrossfaderCurve curve, float& gainA, float& gainB) noexcept;

    //--- audio thread ------------------------------------------------------
    void prepare (double sampleRate);

    /** Mixes the decks into the device outputs. Clears every output channel first. */
    void process (const float* const* deckLeft, const float* const* deckRight,
                  float* const* outputs, int numOutputChannels, int numSamples) noexcept;

private:
    struct Channel
    {
        std::atomic<float> volume { 1.0f };
        std::atomic<bool>  cue { false };
        std::atomic<int>   assign { (int) CrossfaderAssign::Thru };
        std::atomic<int>   directL { -1 }, directR { -1 };
        juce::SmoothedValue<float> gain;   // fader * crossfader
        juce::SmoothedValue<float> cueGain;
    };

    static bool valid (int ch, int numChannels) noexcept { return ch >= 0 && ch < numChannels; }

    Channel channels[config::numDecks];

    std::atomic<float> crossfader { 0.5f }, masterGain { 1.0f }, cueMix { 0.0f }, cueGain { 1.0f };
    std::atomic<int>   curve { (int) CrossfaderCurve::Smooth };
    std::atomic<int>   masterL { 0 }, masterR { 1 }, cueL { 2 }, cueR { 3 };
    std::atomic<bool>  limiterEnabled { true };

    juce::SmoothedValue<float> masterGainSmoothed, cueMixSmoothed, cueGainSmoothed;

    // Safety limiter (instant attack, exponential release) - prevents digital overs on the master.
    float limiterGain = 1.0f, limiterRelease = 0.9999f;
    static constexpr float limiterCeiling = 0.989f; // -0.1 dBFS

    juce::AudioBuffer<float> masterBus, cueBus;

    std::atomic<float> meterDeck[config::numDecks] {}, meterMasterL { 0 }, meterMasterR { 0 }, meterCue { 0 };
    std::atomic<bool>  meterClip { false };
};
} // namespace hype
