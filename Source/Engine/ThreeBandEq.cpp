#include "ThreeBandEq.h"
#include "EngineConfig.h"

#include <cmath>

namespace hype
{
void ThreeBandEq::Svf::setup (double sampleRate, double freq, double q) noexcept
{
    const double g = std::tan (juce::MathConstants<double>::pi * juce::jmin (freq, sampleRate * 0.49) / sampleRate);
    const double kk = 1.0 / q;
    const double aa1 = 1.0 / (1.0 + g * (g + kk));
    a1 = (float) aa1;
    a2 = (float) (g * aa1);
    a3 = (float) (g * g * aa1);
    k = (float) kk;
}

void ThreeBandEq::ChannelState::setup (double sr, double f1, double f2) noexcept
{
    const double butterworthQ = 1.0 / std::sqrt (2.0);
    for (auto* f : { &lp1a, &lp1b, &hp1a, &hp1b }) f->setup (sr, f1, butterworthQ);
    for (auto* f : { &lp2a, &lp2b, &hp2a, &hp2b, &ap2 }) f->setup (sr, f2, butterworthQ);
}

void ThreeBandEq::ChannelState::reset() noexcept
{
    for (auto* f : { &lp1a, &lp1b, &hp1a, &hp1b, &lp2a, &lp2b, &hp2a, &hp2b, &ap2 })
        f->reset();
}

ThreeBandEq::ThreeBandEq()
{
    for (auto& g : gains)
        g.setCurrentAndTargetValue (1.0f);
    prepare (48000.0);
}

void ThreeBandEq::prepare (double sampleRate, double lowMidHz, double midHighHz)
{
    for (auto& c : channels)
    {
        c.setup (sampleRate, lowMidHz, midHighHz);
        c.reset();
    }
    for (auto& g : gains)
    {
        const float target = g.getTargetValue();
        g.reset (sampleRate, config::gainSmoothingSeconds);
        g.setCurrentAndTargetValue (target);
    }
}

void ThreeBandEq::reset()
{
    for (auto& c : channels)
        c.reset();
}

void ThreeBandEq::setBandGains (float lowGain, float midGain, float highGain) noexcept
{
    gains[low].setTargetValue (lowGain);
    gains[mid].setTargetValue (midGain);
    gains[high].setTargetValue (highGain);
}

bool ThreeBandEq::isNeutral() const noexcept
{
    for (auto& g : gains)
        if (g.isSmoothing() || std::abs (g.getTargetValue() - 1.0f) > 1.0e-6f)
            return false;
    return true;
}

void ThreeBandEq::process (float* left, float* right, int numSamples) noexcept
{
    auto& cl = channels[0];
    auto& cr = channels[1];

    for (int i = 0; i < numSamples; ++i)
    {
        const float gl = gains[low].getNextValue();
        const float gm = gains[mid].getNextValue();
        const float gh = gains[high].getNextValue();

        float lo, mi, hi;
        cl.split (left[i], lo, mi, hi);
        left[i] = gl * lo + gm * mi + gh * hi;
        cr.split (right[i], lo, mi, hi);
        right[i] = gl * lo + gm * mi + gh * hi;
    }
}
} // namespace hype
