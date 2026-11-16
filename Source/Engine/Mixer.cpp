#include "Mixer.h"

#include <cmath>

namespace hype
{
namespace
{
    void updateMax (std::atomic<float>& meter, float value) noexcept
    {
        if (value > meter.load (std::memory_order_relaxed))
            meter.store (value, std::memory_order_relaxed);
    }

    void updateMin (std::atomic<float>& meter, float value) noexcept
    {
        if (value < meter.load (std::memory_order_relaxed))
            meter.store (value, std::memory_order_relaxed);
    }

    float peakOf (const float* data, int n) noexcept
    {
        const auto r = juce::FloatVectorOperations::findMinAndMax (data, n);
        return juce::jmax (r.getEnd(), -r.getStart());
    }

    float rmsOf (const float* data, int n) noexcept
    {
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += (double) data[i] * data[i];
        return n > 0 ? (float) std::sqrt (sum / n) : 0.0f;
    }
}

Mixer::Mixer()
{
    prepare (48000.0);
}

void Mixer::setChannelVolume (int d, float v)    { channels[d].volume.store (juce::jlimit (0.0f, 1.0f, v)); }
void Mixer::setEqGainDb (int d, int b, float db) { channels[d].eqDb[juce::jlimit (0, 2, b)].store (juce::jlimit (-100.0f, ThreeBandEq::maxGainDb, db)); }
void Mixer::setEqKill (int d, int b, bool k)     { channels[d].eqKill[juce::jlimit (0, 2, b)].store (k); }
void Mixer::setCueEnabled (int d, bool e)        { channels[d].cue.store (e); }
void Mixer::setCrossfaderAssign (int d, CrossfaderAssign a) { channels[d].assign.store ((int) a); }
void Mixer::setDirectOutput (int d, int l, int r)
{
    channels[d].directL.store (l);
    channels[d].directR.store (r);
}

void Mixer::setCrossfader (float p)              { crossfader.store (juce::jlimit (0.0f, 1.0f, p)); }
void Mixer::setCrossfaderCurveShape (float s)    { curveShape.store (juce::jlimit (0.0f, 1.0f, s)); }
void Mixer::setCrossfaderCurve (CrossfaderCurve c) { setCrossfaderCurveShape (presetShape (c)); }
void Mixer::setCrossfaderReverse (bool r)        { reverse.store (r); }
void Mixer::setMasterGain (float g)              { masterGain.store (juce::jlimit (0.0f, 2.0f, g)); }
void Mixer::setLimiterEnabled (bool e)           { limiterEnabled.store (e); }
void Mixer::setCueMix (float m)                  { cueMix.store (juce::jlimit (0.0f, 1.0f, m)); }
void Mixer::setCueGain (float g)                 { cueGain.store (juce::jlimit (0.0f, 2.0f, g)); }
void Mixer::setSplitCue (bool e)                 { splitCue.store (e); }
void Mixer::setMasterOutput (int l, int r)       { masterL.store (l); masterR.store (r); }
void Mixer::setCueOutput (int l, int r)          { cueL.store (l); cueR.store (r); }

float Mixer::faderToGain (float x) noexcept
{
    // Audio-taper channel fader: ~-6 dB at 3/4 travel, ~-12 dB at half, silent at 0.
    x = juce::jlimit (0.0f, 1.0f, x);
    return x * x;
}

float Mixer::presetShape (CrossfaderCurve c) noexcept
{
    switch (c)
    {
        case CrossfaderCurve::Smooth: return 0.0f;     // constant power
        case CrossfaderCurve::Linear: return 0.177f;   // k = 2: dipless, both full at centre
        case CrossfaderCurve::Cut:    return 1.0f;     // scratch
    }
    return 0.0f;
}

void Mixer::crossfaderGains (float p, float shape, bool reversed, float& a, float& b) noexcept
{
    // Each side's gain depends on its distance from the far end, x in [0, 1]:
    //   g(x) = sin(pi/2 * min(1, x * k)),  k = 1 + 32 * shape^2
    // shape 0 -> k = 1: constant-power blend (equal loudness, -3 dB each at centre)
    // shape 1 -> k = 33: both channels at full level until ~3 % from the far end (scratch cut)
    p = juce::jlimit (0.0f, 1.0f, p);
    if (reversed)
        p = 1.0f - p;

    const float k = 1.0f + 32.0f * shape * shape;
    auto curve = [k] (float x) { return std::sin (juce::MathConstants<float>::halfPi * juce::jmin (1.0f, x * k)); };
    a = curve (1.0f - p);
    b = curve (p);
}

void Mixer::crossfaderGains (float p, CrossfaderCurve c, float& a, float& b) noexcept
{
    crossfaderGains (p, presetShape (c), false, a, b);
}

Mixer::Meters Mixer::getAndResetMeters()
{
    Meters m;
    for (int d = 0; d < config::numDecks; ++d)
        m.channelPeak[d] = meterChannel[d].exchange (0.0f);
    m.masterPeakL = meterMasterL.exchange (0.0f);
    m.masterPeakR = meterMasterR.exchange (0.0f);
    m.masterRmsL  = meterRmsL.exchange (0.0f);
    m.masterRmsR  = meterRmsR.exchange (0.0f);
    m.cuePeakL    = meterCueL.exchange (0.0f);
    m.cuePeakR    = meterCueR.exchange (0.0f);
    m.masterClipped = meterClip.exchange (false);
    m.limiterReductionDb = -juce::Decibels::gainToDecibels (meterMinLimiterGain.exchange (1.0f), -60.0f);
    return m;
}

float Mixer::channelTargetGain (int d, float xfA, float xfB) const noexcept
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

void Mixer::prepare (double sampleRate)
{
    float xfA = 1.0f, xfB = 1.0f;
    crossfaderGains (crossfader.load(), curveShape.load(), reverse.load(), xfA, xfB);

    for (int d = 0; d < config::numDecks; ++d)
    {
        auto& ch = channels[d];
        ch.gain.reset (sampleRate, config::gainSmoothingSeconds);
        ch.cueGain.reset (sampleRate, config::gainSmoothingSeconds);
        ch.gain.setCurrentAndTargetValue (channelTargetGain (d, xfA, xfB));
        ch.cueGain.setCurrentAndTargetValue (ch.cue.load() ? 1.0f : 0.0f);
        ch.eq.prepare (sampleRate);
    }
    masterGainSmoothed.reset (sampleRate, config::gainSmoothingSeconds);
    masterGainSmoothed.setCurrentAndTargetValue (masterGain.load());
    cueMixSmoothed.reset (sampleRate, config::gainSmoothingSeconds);
    cueMixSmoothed.setCurrentAndTargetValue (cueMix.load());
    cueGainSmoothed.reset (sampleRate, config::gainSmoothingSeconds);
    cueGainSmoothed.setCurrentAndTargetValue (cueGain.load());

    limiterGain = 1.0f;
    limiterRelease = (float) std::exp (-1.0 / (0.150 * sampleRate));

    eqBus.setSize (2, config::maxInternalBlock);
    masterBus.setSize (2, config::maxInternalBlock);
    cueBus.setSize (2, config::maxInternalBlock);
}

void Mixer::process (const float* const* deckLeft, const float* const* deckRight,
                     float* const* outputs, int numOut, int n) noexcept
{
    jassert (n <= config::maxInternalBlock);

    for (int c = 0; c < numOut; ++c)
        if (outputs[c] != nullptr)
            juce::FloatVectorOperations::clear (outputs[c], n);

    float* eL = eqBus.getWritePointer (0);
    float* eR = eqBus.getWritePointer (1);
    float* mL = masterBus.getWritePointer (0);
    float* mR = masterBus.getWritePointer (1);
    float* cL = cueBus.getWritePointer (0);
    float* cR = cueBus.getWritePointer (1);
    juce::FloatVectorOperations::clear (mL, n);
    juce::FloatVectorOperations::clear (mR, n);
    juce::FloatVectorOperations::clear (cL, n);
    juce::FloatVectorOperations::clear (cR, n);

    float xfA = 1.0f, xfB = 1.0f;
    crossfaderGains (crossfader.load (std::memory_order_relaxed),
                     curveShape.load (std::memory_order_relaxed),
                     reverse.load (std::memory_order_relaxed), xfA, xfB);

    for (int d = 0; d < config::numDecks; ++d)
    {
        auto& ch = channels[d];

        // EQ (always processed, even when flat, so its state stays continuous).
        juce::FloatVectorOperations::copy (eL, deckLeft[d], n);
        juce::FloatVectorOperations::copy (eR, deckRight[d], n);
        float bandGain[3];
        for (int b = 0; b < 3; ++b)
            bandGain[b] = ch.eqKill[b].load (std::memory_order_relaxed)
                            ? 0.0f
                            : ThreeBandEq::dbToBandGain (ch.eqDb[b].load (std::memory_order_relaxed));
        ch.eq.setBandGains (bandGain[0], bandGain[1], bandGain[2]);
        ch.eq.process (eL, eR, n);

        updateMax (meterChannel[d], juce::jmax (peakOf (eL, n), peakOf (eR, n)));

        ch.gain.setTargetValue (channelTargetGain (d, xfA, xfB));
        ch.cueGain.setTargetValue (ch.cue.load (std::memory_order_relaxed) ? 1.0f : 0.0f);

        for (int i = 0; i < n; ++i)
        {
            const float g = ch.gain.getNextValue();
            mL[i] += eL[i] * g;
            mR[i] += eR[i] * g;

            const float cg = ch.cueGain.getNextValue();
            cL[i] += eL[i] * cg;
            cR[i] += eR[i] * cg;
        }

        // Direct (external mixer) outputs: post-EQ, pre-fader.
        const int dl = ch.directL.load (std::memory_order_relaxed);
        const int dr = ch.directR.load (std::memory_order_relaxed);
        if (valid (dl, numOut) && outputs[dl] != nullptr) juce::FloatVectorOperations::add (outputs[dl], eL, n);
        if (valid (dr, numOut) && outputs[dr] != nullptr) juce::FloatVectorOperations::add (outputs[dr], eR, n);
    }

    // Master volume + safety limiter.
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

    updateMax (meterMasterL, peakOf (mL, n));
    updateMax (meterMasterR, peakOf (mR, n));
    updateMax (meterRmsL, rmsOf (mL, n));
    updateMax (meterRmsR, rmsOf (mR, n));
    updateMin (meterMinLimiterGain, minLimiterGain);
    if (clipped)
        meterClip.store (true, std::memory_order_relaxed);

    // Headphones.
    cueMixSmoothed.setTargetValue (cueMix.load (std::memory_order_relaxed));
    cueGainSmoothed.setTargetValue (cueGain.load (std::memory_order_relaxed));
    const bool split = splitCue.load (std::memory_order_relaxed);

    for (int i = 0; i < n; ++i)
    {
        const float m = cueMixSmoothed.getNextValue();
        const float g = cueGainSmoothed.getNextValue();
        if (split)
        {
            // Left ear: cue bus (mono). Right ear: master (mono).
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
    updateMax (meterCueL, peakOf (cL, n));
    updateMax (meterCueR, peakOf (cR, n));

    auto route = [&] (int ch, const float* src)
    {
        if (valid (ch, numOut) && outputs[ch] != nullptr)
            juce::FloatVectorOperations::add (outputs[ch], src, n);
    };

    const int ml = masterL.load (std::memory_order_relaxed), mr = masterR.load (std::memory_order_relaxed);
    const int hl = cueL.load (std::memory_order_relaxed),    hr = cueR.load (std::memory_order_relaxed);

    // Split cue on a single stereo pair (typical laptop setup): the split signal replaces master there.
    const bool sharedPair = split && ml == hl && mr == hr;
    if (! sharedPair)
    {
        route (ml, mL);
        route (mr, mR);
    }
    route (hl, cL);
    route (hr, cR);
}
} // namespace hype
