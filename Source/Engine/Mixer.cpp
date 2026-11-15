#include "Mixer.h"

#include <cmath>

namespace hype
{
namespace
{
    void updatePeak (std::atomic<float>& meter, float value) noexcept
    {
        if (value > meter.load (std::memory_order_relaxed))
            meter.store (value, std::memory_order_relaxed);
    }

    float peakOf (const float* data, int n) noexcept
    {
        const auto r = juce::FloatVectorOperations::findMinAndMax (data, n);
        return juce::jmax (r.getEnd(), -r.getStart());
    }
}

Mixer::Mixer()
{
    prepare (48000.0);
}

void Mixer::setChannelVolume (int d, float v)    { channels[d].volume.store (juce::jlimit (0.0f, 1.0f, v)); }
void Mixer::setCueEnabled (int d, bool e)        { channels[d].cue.store (e); }
void Mixer::setCrossfaderAssign (int d, CrossfaderAssign a) { channels[d].assign.store ((int) a); }
void Mixer::setDirectOutput (int d, int l, int r)
{
    channels[d].directL.store (l);
    channels[d].directR.store (r);
}

void Mixer::setCrossfader (float p)              { crossfader.store (juce::jlimit (0.0f, 1.0f, p)); }
void Mixer::setCrossfaderCurve (CrossfaderCurve c) { curve.store ((int) c); }
void Mixer::setMasterGain (float g)              { masterGain.store (juce::jlimit (0.0f, 4.0f, g)); }
void Mixer::setCueMix (float m)                  { cueMix.store (juce::jlimit (0.0f, 1.0f, m)); }
void Mixer::setCueGain (float g)                 { cueGain.store (juce::jlimit (0.0f, 4.0f, g)); }
void Mixer::setMasterOutput (int l, int r)       { masterL.store (l); masterR.store (r); }
void Mixer::setCueOutput (int l, int r)          { cueL.store (l); cueR.store (r); }
void Mixer::setLimiterEnabled (bool e)           { limiterEnabled.store (e); }

float Mixer::faderToGain (float x) noexcept
{
    // Audio-taper channel fader: ~-6 dB at 3/4 travel, ~-12 dB at half, silent at 0.
    x = juce::jlimit (0.0f, 1.0f, x);
    return x * x;
}

void Mixer::crossfaderGains (float p, CrossfaderCurve c, float& a, float& b) noexcept
{
    p = juce::jlimit (0.0f, 1.0f, p);
    switch (c)
    {
        case CrossfaderCurve::Smooth:   // constant power: no level dip in the middle
            a = std::cos (p * juce::MathConstants<float>::halfPi);
            b = std::sin (p * juce::MathConstants<float>::halfPi);
            break;
        case CrossfaderCurve::Linear:   // full level at centre, linear fade at either side ("dipless")
            a = juce::jmin (1.0f, 2.0f * (1.0f - p));
            b = juce::jmin (1.0f, 2.0f * p);
            break;
        case CrossfaderCurve::Cut:      // scratch: both full almost everywhere, ~3 % cut-in at the edges
            a = juce::jlimit (0.0f, 1.0f, (1.0f - p) / 0.03f);
            b = juce::jlimit (0.0f, 1.0f, p / 0.03f);
            break;
    }
}

Mixer::Meters Mixer::getAndResetMeters()
{
    Meters m;
    for (int d = 0; d < config::numDecks; ++d)
        m.deck[d] = meterDeck[d].exchange (0.0f);
    m.masterL = meterMasterL.exchange (0.0f);
    m.masterR = meterMasterR.exchange (0.0f);
    m.cue = meterCue.exchange (0.0f);
    m.masterClipped = meterClip.exchange (false);
    return m;
}

void Mixer::prepare (double sampleRate)
{
    float xfA = 1.0f, xfB = 1.0f;
    crossfaderGains (crossfader.load(), (CrossfaderCurve) curve.load(), xfA, xfB);

    for (auto& ch : channels)
    {
        const auto assign = (CrossfaderAssign) ch.assign.load();
        const float xf = assign == CrossfaderAssign::A ? xfA : assign == CrossfaderAssign::B ? xfB : 1.0f;
        ch.gain.reset (sampleRate, config::gainSmoothingSeconds);
        ch.cueGain.reset (sampleRate, config::gainSmoothingSeconds);
        ch.gain.setCurrentAndTargetValue (faderToGain (ch.volume.load()) * xf);
        ch.cueGain.setCurrentAndTargetValue (ch.cue.load() ? 1.0f : 0.0f);
    }
    masterGainSmoothed.reset (sampleRate, config::gainSmoothingSeconds);
    masterGainSmoothed.setCurrentAndTargetValue (masterGain.load());
    cueMixSmoothed.reset (sampleRate, config::gainSmoothingSeconds);
    cueMixSmoothed.setCurrentAndTargetValue (cueMix.load());
    cueGainSmoothed.reset (sampleRate, config::gainSmoothingSeconds);
    cueGainSmoothed.setCurrentAndTargetValue (cueGain.load());

    limiterGain = 1.0f;
    limiterRelease = (float) std::exp (-1.0 / (0.150 * sampleRate)); // 150 ms release

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
                     (CrossfaderCurve) curve.load (std::memory_order_relaxed), xfA, xfB);

    for (int d = 0; d < config::numDecks; ++d)
    {
        auto& ch = channels[d];
        const float* inL = deckLeft[d];
        const float* inR = deckRight[d];

        float xf = 1.0f;
        switch ((CrossfaderAssign) ch.assign.load (std::memory_order_relaxed))
        {
            case CrossfaderAssign::A:    xf = xfA; break;
            case CrossfaderAssign::B:    xf = xfB; break;
            case CrossfaderAssign::Thru: xf = 1.0f; break;
        }

        ch.gain.setTargetValue (faderToGain (ch.volume.load (std::memory_order_relaxed)) * xf);
        ch.cueGain.setTargetValue (ch.cue.load (std::memory_order_relaxed) ? 1.0f : 0.0f);

        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float g = ch.gain.getNextValue();
            const float l = inL[i] * g, r = inR[i] * g;
            mL[i] += l;
            mR[i] += r;
            peak = juce::jmax (peak, std::abs (l), std::abs (r));

            const float cg = ch.cueGain.getNextValue();
            cL[i] += inL[i] * cg;
            cR[i] += inR[i] * cg;
        }
        updatePeak (meterDeck[d], peak);

        // Direct (external mixer) outputs: pre-fader, post-trim.
        const int dl = ch.directL.load (std::memory_order_relaxed);
        const int dr = ch.directR.load (std::memory_order_relaxed);
        if (valid (dl, numOut) && outputs[dl] != nullptr) juce::FloatVectorOperations::add (outputs[dl], inL, n);
        if (valid (dr, numOut) && outputs[dr] != nullptr) juce::FloatVectorOperations::add (outputs[dr], inR, n);
    }

    // Master gain + safety limiter.
    masterGainSmoothed.setTargetValue (masterGain.load (std::memory_order_relaxed));
    const bool limit = limiterEnabled.load (std::memory_order_relaxed);
    bool clipped = false;

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
            limiterGain = 1.0f - (1.0f - limiterGain) * limiterRelease;
        }
        else if (std::abs (l) > 1.0f || std::abs (r) > 1.0f)
        {
            clipped = true;
        }

        mL[i] = l;
        mR[i] = r;
    }

    updatePeak (meterMasterL, peakOf (mL, n));
    updatePeak (meterMasterR, peakOf (mR, n));
    if (clipped)
        meterClip.store (true, std::memory_order_relaxed);

    // Headphone cue: blend PFL bus with master.
    cueMixSmoothed.setTargetValue (cueMix.load (std::memory_order_relaxed));
    cueGainSmoothed.setTargetValue (cueGain.load (std::memory_order_relaxed));
    for (int i = 0; i < n; ++i)
    {
        const float m = cueMixSmoothed.getNextValue();
        const float g = cueGainSmoothed.getNextValue();
        cL[i] = (cL[i] * (1.0f - m) + mL[i] * m) * g;
        cR[i] = (cR[i] * (1.0f - m) + mR[i] * m) * g;
    }
    updatePeak (meterCue, juce::jmax (peakOf (cL, n), peakOf (cR, n)));

    auto route = [&] (int ch, const float* src)
    {
        if (valid (ch, numOut) && outputs[ch] != nullptr)
            juce::FloatVectorOperations::add (outputs[ch], src, n);
    };

    route (masterL.load (std::memory_order_relaxed), mL);
    route (masterR.load (std::memory_order_relaxed), mR);
    route (cueL.load (std::memory_order_relaxed), cL);
    route (cueR.load (std::memory_order_relaxed), cR);
}
} // namespace hype
