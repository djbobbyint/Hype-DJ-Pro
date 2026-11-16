#include "MixerEngine.h"

#include <cmath>

namespace hype
{
namespace
{
    float rmsOf (const float* data, int n) noexcept
    {
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += (double) data[i] * data[i];
        return n > 0 ? (float) std::sqrt (sum / n) : 0.0f;
    }
}

MixerEngine::MixerEngine()
{
    prepare (48000.0);
}

void MixerEngine::setChannelGainDb (int d, float db)      { channels[d].gainDb.store (juce::jlimit (config::minTrimDb, config::maxTrimDb, db)); }
void MixerEngine::setEqGainDb (int d, EqBand b, float db) { channels[d].eqDb[(int) b].store (juce::jlimit (-100.0f, ThreeBandEq::maxGainDb, db)); }
void MixerEngine::setEqKill (int d, EqBand b, bool k)     { channels[d].eqKill[(int) b].store (k); }
void MixerEngine::setChannelVolume (int d, float v)       { channels[d].volume.store (juce::jlimit (0.0f, 1.0f, v)); }
void MixerEngine::setCrossfaderAssign (int d, CrossfaderAssign a) { channels[d].assign.store ((int) a); }
void MixerEngine::setCrossfader (float p)                 { crossfader.store (juce::jlimit (0.0f, 1.0f, p)); }
void MixerEngine::setCrossfaderCurveShape (float s)       { curveShape.store (juce::jlimit (0.0f, 1.0f, s)); }
void MixerEngine::setCrossfaderCurve (CrossfaderCurve c)  { setCrossfaderCurveShape (presetShape (c)); }
void MixerEngine::setCrossfaderReverse (bool r)           { reverse.store (r); }
void MixerEngine::setMasterVolume (float g)               { masterGain.store (juce::jlimit (0.0f, 2.0f, g)); }
void MixerEngine::setLimiterEnabled (bool e)              { limiterEnabled.store (e); }

float MixerEngine::faderToGain (float x) noexcept
{
    // Audio-taper channel fader: ~-6 dB at 3/4 travel, ~-12 dB at half, silent at 0.
    x = juce::jlimit (0.0f, 1.0f, x);
    return x * x;
}

float MixerEngine::presetShape (CrossfaderCurve c) noexcept
{
    switch (c)
    {
        case CrossfaderCurve::Smooth: return 0.0f;     // constant power
        case CrossfaderCurve::Linear: return 0.177f;   // k = 2: dipless, both full at centre
        case CrossfaderCurve::Cut:    return 1.0f;     // scratch
    }
    return 0.0f;
}

void MixerEngine::crossfaderGains (float p, float shape, bool reversed, float& a, float& b) noexcept
{
    // Each side's gain depends on its distance from the far end, x in [0, 1]:
    //   g(x) = sin(pi/2 * min(1, x * k)),  k = 1 + 32 * shape^2
    // shape 0 -> k = 1: constant-power blend; shape 1 -> k = 33: scratch cut (~3 % cut-in).
    p = juce::jlimit (0.0f, 1.0f, p);
    if (reversed)
        p = 1.0f - p;

    const float k = 1.0f + 32.0f * shape * shape;
    auto curve = [k] (float x) { return std::sin (juce::MathConstants<float>::halfPi * juce::jmin (1.0f, x * k)); };
    a = curve (1.0f - p);
    b = curve (p);
}

void MixerEngine::crossfaderGains (float p, CrossfaderCurve c, float& a, float& b) noexcept
{
    crossfaderGains (p, presetShape (c), false, a, b);
}

MixerMeters MixerEngine::getAndResetMeters()
{
    MixerMeters m;
    for (int d = 0; d < numDecks; ++d)
        m.channelPeak[d] = meterChannel[d].exchange (0.0f);
    m.masterPeakL = meterMasterL.exchange (0.0f);
    m.masterPeakR = meterMasterR.exchange (0.0f);
    m.masterRmsL  = meterRmsL.exchange (0.0f);
    m.masterRmsR  = meterRmsR.exchange (0.0f);
    m.masterClipped = meterClip.exchange (false);
    m.limiterReductionDb = -juce::Decibels::gainToDecibels (meterMinLimiterGain.exchange (1.0f), -60.0f);
    return m;
}

float MixerEngine::channelTargetGain (int d, float xfA, float xfB) const noexcept
{
    const auto& ch = channels[d];
    float xf = 1.0f;
    switch ((CrossfaderAssign) ch.assign.load (std::memory_order_relaxed))
    {
        case CrossfaderAssign::A:    xf = xfA; break;
        case CrossfaderAssign::B:    xf = xfB; break;
        case CrossfaderAssign::Thru: xf = 1.0f; break;
    }
    return faderToGain (ch.volume.load (std::memory_order_relaxed)) * xf;
}

void MixerEngine::prepare (double sampleRate)
{
    float xfA = 1.0f, xfB = 1.0f;
    crossfaderGains (crossfader.load(), curveShape.load(), reverse.load(), xfA, xfB);

    for (int d = 0; d < numDecks; ++d)
    {
        auto& ch = channels[d];
        ch.trim.reset (sampleRate, config::gainSmoothingSeconds);
        ch.trim.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (ch.gainDb.load()));
        ch.gain.reset (sampleRate, config::gainSmoothingSeconds);
        ch.gain.setCurrentAndTargetValue (channelTargetGain (d, xfA, xfB));
        ch.eq.prepare (sampleRate);
    }
    masterGainSmoothed.reset (sampleRate, config::gainSmoothingSeconds);
    masterGainSmoothed.setCurrentAndTargetValue (masterGain.load());

    limiterGain = 1.0f;
    limiterRelease = (float) std::exp (-1.0 / (0.150 * sampleRate));

    channelBus.setSize (numDecks * 2, config::maxInternalBlock);
    masterBus.setSize (2, config::maxInternalBlock);
}

void MixerEngine::process (const float* const* deckLeft, const float* const* deckRight, int n) noexcept
{
    jassert (n <= config::maxInternalBlock);

    float* mL = masterBus.getWritePointer (0);
    float* mR = masterBus.getWritePointer (1);
    juce::FloatVectorOperations::clear (mL, n);
    juce::FloatVectorOperations::clear (mR, n);

    float xfA = 1.0f, xfB = 1.0f;
    crossfaderGains (crossfader.load (std::memory_order_relaxed),
                     curveShape.load (std::memory_order_relaxed),
                     reverse.load (std::memory_order_relaxed), xfA, xfB);

    for (int d = 0; d < numDecks; ++d)
    {
        auto& ch = channels[d];
        float* cL = channelBus.getWritePointer (d * 2);
        float* cR = channelBus.getWritePointer (d * 2 + 1);

        // Channel gain (trim)
        ch.trim.setTargetValue (juce::Decibels::decibelsToGain (ch.gainDb.load (std::memory_order_relaxed), -100.0f));
        if (ch.trim.isSmoothing())
        {
            for (int i = 0; i < n; ++i)
            {
                const float g = ch.trim.getNextValue();
                cL[i] = deckLeft[d][i] * g;
                cR[i] = deckRight[d][i] * g;
            }
        }
        else
        {
            const float g = ch.trim.getTargetValue();
            juce::FloatVectorOperations::multiply (cL, deckLeft[d], g, n);
            juce::FloatVectorOperations::multiply (cR, deckRight[d], g, n);
        }

        // EQ (always processed, even when flat, so its state stays continuous)
        float bandGain[3];
        for (int b = 0; b < 3; ++b)
            bandGain[b] = ch.eqKill[b].load (std::memory_order_relaxed)
                            ? 0.0f
                            : ThreeBandEq::dbToBandGain (ch.eqDb[b].load (std::memory_order_relaxed));
        ch.eq.setBandGains (bandGain[0], bandGain[1], bandGain[2]);
        ch.eq.process (cL, cR, n);

        meter::updateMax (meterChannel[d], juce::jmax (meter::peakOf (cL, n), meter::peakOf (cR, n)));

        // Fader + crossfader into the master bus
        ch.gain.setTargetValue (channelTargetGain (d, xfA, xfB));
        for (int i = 0; i < n; ++i)
        {
            const float g = ch.gain.getNextValue();
            mL[i] += cL[i] * g;
            mR[i] += cR[i] * g;
        }
    }

    // Master volume + safety limiter
    masterGainSmoothed.setTargetValue (masterGain.load (std::memory_order_relaxed));
    const bool limit = limiterEnabled.load (std::memory_order_relaxed);
    bool clipped = false;
    float minLimiterGain = 1.0f;

    for (int i = 0; i < n; ++i)
    {
        const float g = masterGainSmoothed.getNextValue();
        float l = mL[i] * g, r = mR[i] * g;

        if (limit)
        {
            const float peak = juce::jmax (std::abs (l), std::abs (r));
            if (peak * limiterGain > limiterCeiling)
            {
                limiterGain = limiterCeiling / peak;
                clipped = true;
            }
            l *= limiterGain;
            r *= limiterGain;
            minLimiterGain = juce::jmin (minLimiterGain, limiterGain);
            limiterGain = 1.0f - (1.0f - limiterGain) * limiterRelease;
        }
        else if (std::abs (l) > 1.0f || std::abs (r) > 1.0f)
        {
            clipped = true;
        }

        mL[i] = l;
        mR[i] = r;
    }

    meter::updateMax (meterMasterL, meter::peakOf (mL, n));
    meter::updateMax (meterMasterR, meter::peakOf (mR, n));
    meter::updateMax (meterRmsL, rmsOf (mL, n));
    meter::updateMax (meterRmsR, rmsOf (mR, n));
    if (minLimiterGain < meterMinLimiterGain.load (std::memory_order_relaxed))
        meterMinLimiterGain.store (minLimiterGain, std::memory_order_relaxed);
    if (clipped)
        meterClip.store (true, std::memory_order_relaxed);
}
} // namespace hype

// The public API publishes these ranges to the UI; keep them in sync with the DSP.
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wfloat-equal")
static_assert (hype::ranges::eqMinDb == hype::ThreeBandEq::minGainDb, "EQ range mismatch");
static_assert (hype::ranges::eqMaxDb == hype::ThreeBandEq::maxGainDb, "EQ range mismatch");
static_assert (hype::ranges::channelGainMinDb == hype::config::minTrimDb, "gain range mismatch");
static_assert (hype::ranges::channelGainMaxDb == hype::config::maxTrimDb, "gain range mismatch");
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
