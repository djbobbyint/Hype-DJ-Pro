#include "CueEngine.h"
#include "MixerEngine.h"   // meter helpers

namespace hype
{
CueEngine::CueEngine()
{
    prepare (48000.0);
}

void CueEngine::setCueEnabled (int d, bool e) { cued[d].store (e); }
void CueEngine::setCueMix (float m)           { cueMix.store (juce::jlimit (0.0f, 1.0f, m)); }
void CueEngine::setCueLevel (float g)         { cueLevel.store (juce::jlimit (0.0f, 2.0f, g)); }
void CueEngine::setSplitCue (bool e)          { splitCue.store (e); }

CueMeters CueEngine::getAndResetMeters()
{
    return { meterL.exchange (0.0f), meterR.exchange (0.0f) };
}

void CueEngine::prepare (double sampleRate)
{
    for (int d = 0; d < numDecks; ++d)
    {
        channelGain[d].reset (sampleRate, config::gainSmoothingSeconds);
        channelGain[d].setCurrentAndTargetValue (cued[d].load() ? 1.0f : 0.0f);
    }
    mixSmoothed.reset (sampleRate, config::gainSmoothingSeconds);
    mixSmoothed.setCurrentAndTargetValue (cueMix.load());
    levelSmoothed.reset (sampleRate, config::gainSmoothingSeconds);
    levelSmoothed.setCurrentAndTargetValue (cueLevel.load());
    bus.setSize (2, config::maxInternalBlock);
}

void CueEngine::process (const float* const* chL, const float* const* chR,
                         const float* mL, const float* mR, int n) noexcept
{
    float* cL = bus.getWritePointer (0);
    float* cR = bus.getWritePointer (1);
    juce::FloatVectorOperations::clear (cL, n);
    juce::FloatVectorOperations::clear (cR, n);

    for (int d = 0; d < numDecks; ++d)
    {
        channelGain[d].setTargetValue (cued[d].load (std::memory_order_relaxed) ? 1.0f : 0.0f);
        if (! channelGain[d].isSmoothing() && channelGain[d].getTargetValue() <= 0.0f)
            continue;
        for (int i = 0; i < n; ++i)
        {
            const float g = channelGain[d].getNextValue();
            cL[i] += chL[d][i] * g;
            cR[i] += chR[d][i] * g;
        }
    }

    mixSmoothed.setTargetValue (cueMix.load (std::memory_order_relaxed));
    levelSmoothed.setTargetValue (cueLevel.load (std::memory_order_relaxed));
    const bool split = splitCue.load (std::memory_order_relaxed);

    for (int i = 0; i < n; ++i)
    {
        const float m = mixSmoothed.getNextValue();
        const float g = levelSmoothed.getNextValue();
        if (split)
        {
            const float cueMono = 0.5f * (cL[i] + cR[i]);
            const float masterMono = 0.5f * (mL[i] + mR[i]);
            cL[i] = cueMono * g;
            cR[i] = masterMono * g;
        }
        else
        {
            cL[i] = (cL[i] * (1.0f - m) + mL[i] * m) * g;
            cR[i] = (cR[i] * (1.0f - m) + mR[i] * m) * g;
        }
    }

    meter::updateMax (meterL, meter::peakOf (cL, n));
    meter::updateMax (meterR, meter::peakOf (cR, n));
}
} // namespace hype
