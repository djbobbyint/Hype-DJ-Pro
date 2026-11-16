#pragma once

#include "Api/HypeEngineApi.h"
#include "EngineConfig.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace hype
{
/**
    CueEngine: headphone monitoring.

    Builds the headphone signal from the MixerEngine's per-channel taps (post gain/EQ,
    pre-fader, independent of the crossfader) and its master bus:

        CUE bus  = sum of cued channels
        phones   = blend (CUE bus, MASTER, cueMix) * level          (normal)
                 = [ CUE bus mono | MASTER mono ] * level           (split cue: left | right)

    It only reads the mixer's buffers, so pre-listening can never change the master output.
    Control side (CueControl): any non-audio thread. process(): audio thread only.
*/
class CueEngine : public CueControl
{
public:
    CueEngine();

    //--- CueControl ------------------------------------------------------------
    void setCueEnabled (int deck, bool enabled) override;
    void setCueMix (float cueToMaster0to1) override;
    void setCueLevel (float linearGain) override;
    void setSplitCue (bool enabled) override;

    bool  isCueEnabled (int d) const override  { return cued[d].load(); }
    float getCueMix() const override           { return cueMix.load(); }
    float getCueLevel() const override         { return cueLevel.load(); }
    bool  isSplitCue() const override          { return splitCue.load(); }

    CueMeters getAndResetMeters() override;

    //--- audio thread (AudioEngine) ----------------------------------------------
    void prepare (double sampleRate);

    /** channelLeft/Right: per-deck pre-fader taps; masterLeft/Right: the master bus. */
    void process (const float* const* channelLeft, const float* const* channelRight,
                  const float* masterLeft, const float* masterRight, int numSamples) noexcept;

    const float* getOutput (int channel) const noexcept { return bus.getReadPointer (channel); }
    bool isSplitActive() const noexcept                 { return splitCue.load (std::memory_order_relaxed); }

private:
    std::atomic<bool>  cued[numDecks] {};
    std::atomic<float> cueMix { 0.0f }, cueLevel { 1.0f };
    std::atomic<bool>  splitCue { false };

    juce::SmoothedValue<float> channelGain[numDecks], mixSmoothed, levelSmoothed;
    juce::AudioBuffer<float> bus;
    std::atomic<float> meterL { 0 }, meterR { 0 };
};
} // namespace hype
