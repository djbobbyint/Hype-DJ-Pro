#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>

namespace hype
{
/**
    DJ-style 3-band isolator EQ.

    The signal is split with 4th-order Linkwitz-Riley crossovers (default 250 Hz and 2.5 kHz):

        low  = AP2(f2) * LR4_LP(f1) * x      (all-pass keeps the low band phase-aligned)
        mid  = LR4_LP(f2) * LR4_HP(f1) * x
        high = LR4_HP(f2) * LR4_HP(f1) * x

    With all gains at unity the bands sum to a pure all-pass, so the magnitude response is
    perfectly flat. Each band goes from full kill (silence) to +6 dB. Band gains are smoothed
    per sample, so turning a knob or hitting a kill never clicks.

    Built from topology-preserving-transform state-variable filters, which stay stable and
    smooth under modulation. Real-time safe: no allocation after prepare().
*/
class ThreeBandEq
{
public:
    enum Band { low = 0, mid = 1, high = 2 };

    static constexpr float minGainDb  = -26.0f;   // at/below this the band is fully killed
    static constexpr float maxGainDb  = 6.0f;

    ThreeBandEq();

    void prepare (double sampleRate, double lowMidHz = 250.0, double midHighHz = 2500.0);
    void reset();

    /** Target band gains, linear (0 = kill). Smoothed internally. */
    void setBandGains (float lowGain, float midGain, float highGain) noexcept;

    /** True if all targets are unity and nothing is smoothing (the EQ is then only an all-pass). */
    bool isNeutral() const noexcept;

    void process (float* left, float* right, int numSamples) noexcept;

    /** Knob-friendly mapping: dB <= minGainDb is a kill (0), otherwise decibels -> linear. */
    static float dbToBandGain (float db) noexcept
    {
        return db <= minGainDb ? 0.0f : juce::Decibels::decibelsToGain (juce::jmin (db, maxGainDb));
    }

private:
    struct Svf
    {
        float a1 = 0, a2 = 0, a3 = 0, k = 0;
        float ic1 = 0, ic2 = 0;

        void setup (double sampleRate, double freq, double q) noexcept;
        void reset() noexcept { ic1 = ic2 = 0.0f; }

        // Returns low-pass, band-pass via out params; high-pass/all-pass derived by caller.
        inline void tick (float x, float& lp, float& bp) noexcept
        {
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            lp = v2;
            bp = v1;
        }
        inline float lowpass (float x) noexcept  { float lp, bp; tick (x, lp, bp); return lp; }
        inline float highpass (float x) noexcept { float lp, bp; tick (x, lp, bp); return x - k * bp - lp; }
        inline float allpass (float x) noexcept  { float lp, bp; tick (x, lp, bp); return x - 2.0f * k * bp; }
    };

    struct ChannelState
    {
        Svf lp1a, lp1b, hp1a, hp1b;   // crossover 1 (LR4 = two cascaded Butterworth 2nd order)
        Svf lp2a, lp2b, hp2a, hp2b;   // crossover 2
        Svf ap2;                      // low-band phase compensation at crossover 2

        void setup (double sr, double f1, double f2) noexcept;
        void reset() noexcept;
        inline void split (float x, float& lo, float& mi, float& hi) noexcept
        {
            const float l1 = lp1b.lowpass (lp1a.lowpass (x));
            const float h1 = hp1b.highpass (hp1a.highpass (x));
            lo = ap2.allpass (l1);
            mi = lp2b.lowpass (lp2a.lowpass (h1));
            hi = hp2b.highpass (hp2a.highpass (h1));
        }
    };

    std::array<ChannelState, 2> channels;
    std::array<juce::SmoothedValue<float>, 3> gains;
};
} // namespace hype
