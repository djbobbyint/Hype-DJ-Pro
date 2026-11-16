#pragma once

#include "Api/HypeEngineApi.h"
#include "EngineConfig.h"
#include "ThreeBandEq.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace hype
{
/**
    MixerEngine: the two channel strips, crossfader and master section.

        deck -> [gain] -> [3-band EQ] --+--> [channel fader] -> [crossfader A/B/thru] -> MASTER bus
                                        +--> channel output (post-EQ, pre-fader): tapped by
                                             CueEngine (headphones) and OutputRouter (direct outs)

        MASTER bus -> [master volume] -> [safety limiter]

    Control side (MixerControl): any non-audio thread, lock-free atomics.
    Audio side (process): called by AudioEngine only; writes into internal buffers that the
    CueEngine and OutputRouter read during the same block. Real-time safe.
*/
class MixerEngine : public MixerControl
{
public:
    MixerEngine();

    //--- MixerControl ----------------------------------------------------------
    void setChannelGainDb (int deck, float decibels) override;
    void setEqGainDb (int deck, EqBand, float decibels) override;
    void setEqKill (int deck, EqBand, bool killed) override;
    void setChannelVolume (int deck, float fader0to1) override;
    void setCrossfaderAssign (int deck, CrossfaderAssign) override;
    void setCrossfader (float position0to1) override;
    void setCrossfaderCurveShape (float shape0to1) override;
    void setCrossfaderCurve (CrossfaderCurve) override;
    void setCrossfaderReverse (bool) override;
    void setMasterVolume (float linearGain) override;
    void setLimiterEnabled (bool) override;

    float getChannelGainDb (int d) const override            { return channels[d].gainDb.load(); }
    float getEqGainDb (int d, EqBand b) const override       { return channels[d].eqDb[(int) b].load(); }
    bool  isEqKilled (int d, EqBand b) const override        { return channels[d].eqKill[(int) b].load(); }
    float getChannelVolume (int d) const override            { return channels[d].volume.load(); }
    CrossfaderAssign getCrossfaderAssign (int d) const override { return (CrossfaderAssign) channels[d].assign.load(); }
    float getCrossfader() const override                     { return crossfader.load(); }
    float getCrossfaderCurveShape() const override           { return curveShape.load(); }
    bool  isCrossfaderReversed() const override              { return reverse.load(); }
    float getMasterVolume() const override                   { return masterGain.load(); }

    MixerMeters getAndResetMeters() override;

    //--- helpers -----------------------------------------------------------------
    static float faderToGain (float fader0to1) noexcept;
    static void  crossfaderGains (float position, float shape, bool reversed, float& gainA, float& gainB) noexcept;
    static void  crossfaderGains (float position, CrossfaderCurve, float& gainA, float& gainB) noexcept;
    static float presetShape (CrossfaderCurve) noexcept;

    //--- audio thread (AudioEngine) ----------------------------------------------
    void prepare (double sampleRate);
    void process (const float* const* deckLeft, const float* const* deckRight, int numSamples) noexcept;

    /** Results of the last process() call, valid until the next one. */
    const float* getChannelOutput (int deck, int channel) const noexcept { return channelBus.getReadPointer (deck * 2 + channel); }
    const float* getMasterOutput (int channel) const noexcept           { return masterBus.getReadPointer (channel); }

private:
    struct Channel
    {
        std::atomic<float> gainDb { 0.0f };
        std::atomic<float> volume { 1.0f };
        std::atomic<float> eqDb[3] { {0.0f}, {0.0f}, {0.0f} };
        std::atomic<bool>  eqKill[3] { {false}, {false}, {false} };
        std::atomic<int>   assign { (int) CrossfaderAssign::Thru };
        juce::SmoothedValue<float> trim;    // channel gain
        juce::SmoothedValue<float> gain;    // fader * crossfader
        ThreeBandEq eq;
    };

    float channelTargetGain (int deck, float xfA, float xfB) const noexcept;

    Channel channels[numDecks];

    std::atomic<float> crossfader { 0.5f }, curveShape { 0.0f }, masterGain { 1.0f };
    std::atomic<bool>  reverse { false }, limiterEnabled { true };

    juce::SmoothedValue<float> masterGainSmoothed;
    float limiterGain = 1.0f, limiterRelease = 0.9999f;
    static constexpr float limiterCeiling = 0.989f;   // -0.1 dBFS

    juce::AudioBuffer<float> channelBus, masterBus;

    std::atomic<float> meterChannel[numDecks] {}, meterMasterL { 0 }, meterMasterR { 0 },
                       meterRmsL { 0 }, meterRmsR { 0 }, meterMinLimiterGain { 1.0f };
    std::atomic<bool>  meterClip { false };
};

namespace meter
{
    inline void updateMax (std::atomic<float>& m, float v) noexcept
    {
        if (v > m.load (std::memory_order_relaxed))
            m.store (v, std::memory_order_relaxed);
    }

    inline float peakOf (const float* data, int n) noexcept
    {
        const auto r = juce::FloatVectorOperations::findMinAndMax (data, n);
        return juce::jmax (r.getEnd(), -r.getStart());
    }
}
} // namespace hype
