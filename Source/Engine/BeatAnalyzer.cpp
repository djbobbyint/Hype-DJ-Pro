#include "BeatAnalyzer.h"

#include <cmath>
#include <vector>

namespace hype
{
BeatAnalysis analyseBeats (const juce::AudioBuffer<float>& audio, double sampleRate,
                           double minBpm, double maxBpm, double maxSecondsToAnalyse)
{
    BeatAnalysis result;

    if (sampleRate <= 0 || audio.getNumSamples() < (int) (sampleRate * 4))
        return result;

    const int hop = std::max (1, (int) std::round (sampleRate / 200.0)); // ~200 Hz envelope rate
    const double frameRate = sampleRate / hop;
    const int numSamples = (int) std::min<double> (audio.getNumSamples(), maxSecondsToAnalyse * sampleRate);
    const int numFrames = numSamples / hop;

    const float* l = audio.getReadPointer (0);
    const float* r = audio.getReadPointer (audio.getNumChannels() > 1 ? 1 : 0);

    // 1) Onset strength envelope from two bands:
    //    - low  (< ~150 Hz, kick drum / bass): drives the grid, so beats land on the kick
    //      rather than on off-beat hi-hats;
    //    - full (first difference, transient-weighted): fallback for material without a kick.
    //    Each band's flux is normalised by its mean before combining.
    const float lowCoeff = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 150.0 / sampleRate));
    std::vector<float> lowEnergy ((size_t) numFrames, 0.0f), fullEnergy ((size_t) numFrames, 0.0f);
    float prev = 0.0f, low1 = 0.0f, low2 = 0.0f;
    for (int f = 0; f < numFrames; ++f)
    {
        double eFull = 0.0, eLow = 0.0;
        for (int i = f * hop, end = i + hop; i < end; ++i)
        {
            const float m = 0.5f * (l[i] + r[i]);
            const float d = m - prev;
            prev = m;
            eFull += (double) d * d;
            low1 += lowCoeff * (m - low1);      // 2-pole low-pass
            low2 += lowCoeff * (low1 - low2);
            eLow += (double) low2 * low2;
        }
        fullEnergy[(size_t) f] = (float) std::log1p (1000.0 * eFull / hop);
        lowEnergy[(size_t) f]  = (float) (eLow / hop);
    }

    // A 5 ms frame is shorter than one cycle of a bass note, so per-frame low-band energy
    // fluctuates with waveform phase. Use a trailing ~40 ms window instead: trailing, so a
    // transient still raises the energy in the frame where it occurs (no timing bias).
    {
        constexpr int window = 8;
        std::vector<float> windowed ((size_t) numFrames, 0.0f);
        double acc = 0.0;
        for (int f = 0; f < numFrames; ++f)
        {
            acc += lowEnergy[(size_t) f];
            if (f >= window) acc -= lowEnergy[(size_t) (f - window)];
            windowed[(size_t) f] = (float) std::log1p (1000.0 * juce::jmax (0.0, acc) / window);
        }
        lowEnergy.swap (windowed);
    }

    auto flux = [numFrames] (const std::vector<float>& e)
    {
        std::vector<float> x ((size_t) numFrames, 0.0f);
        double mean = 0.0;
        for (int f = 1; f < numFrames; ++f)
        {
            x[(size_t) f] = std::max (0.0f, e[(size_t) f] - e[(size_t) f - 1]);
            mean += x[(size_t) f];
        }
        mean /= numFrames;
        // Normalise to the band's mean and cap, so a few very sparse, loud transients
        // (e.g. open hi-hats) can't outweigh a steady kick pattern.
        if (mean > 1.0e-9)
            for (auto& v : x) v = (float) juce::jmin (8.0, v / mean);
        return x;
    };

    const auto lowFlux = flux (lowEnergy), fullFlux = flux (fullEnergy);
    std::vector<float> onset ((size_t) numFrames, 0.0f);
    for (int f = 0; f < numFrames; ++f)
        onset[(size_t) f] = lowFlux[(size_t) f] + 0.25f * fullFlux[(size_t) f];

    // Remove slowly varying mean so the autocorrelation is driven by periodicity.
    {
        const int w = (int) frameRate; // 1 s moving average
        std::vector<float> smoothed ((size_t) numFrames);
        double acc = 0.0;
        for (int f = 0; f < numFrames; ++f)
        {
            acc += onset[(size_t) f];
            if (f >= w) acc -= onset[(size_t) (f - w)];
            smoothed[(size_t) f] = (float) (acc / std::min (f + 1, w));
        }
        for (int f = 0; f < numFrames; ++f)
            onset[(size_t) f] = std::max (0.0f, onset[(size_t) f] - smoothed[(size_t) f]);
    }

    // 2) Autocorrelation over 60..200 BPM.
    const int minLag = (int) std::floor (frameRate * 60.0 / 200.0);
    const int maxLag = (int) std::ceil  (frameRate * 60.0 / 60.0);
    if (maxLag * 2 >= numFrames)
        return result;

    std::vector<double> ac ((size_t) maxLag + 2, 0.0);
    double zeroLag = 0.0;
    for (int f = 0; f < numFrames; ++f)
        zeroLag += (double) onset[(size_t) f] * onset[(size_t) f];
    if (zeroLag <= 0.0)
        return result;

    for (int lag = minLag; lag <= maxLag + 1; ++lag)
    {
        double s = 0.0;
        for (int f = lag; f < numFrames; ++f)
            s += (double) onset[(size_t) f] * onset[(size_t) (f - lag)];
        ac[(size_t) lag] = s / (numFrames - lag);
    }

    // Score each lag with its harmonics (lag*2, lag*4 reinforce a true beat period) and a
    // log-Gaussian tempo prior centred on 120 BPM.
    int bestLag = -1;
    double bestScore = 0.0;
    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        double s = ac[(size_t) lag];
        if (lag * 2 <= maxLag) s += 0.5 * ac[(size_t) (lag * 2)];
        const double bpm = 60.0 * frameRate / lag;
        const double prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 120.0) / 0.9, 2.0));
        s *= prior;
        if (s > bestScore) { bestScore = s; bestLag = lag; }
    }
    if (bestLag <= minLag || bestLag >= maxLag)
        return result;

    // Parabolic refinement of the peak.
    const double a = ac[(size_t) bestLag - 1], b = ac[(size_t) bestLag], c = ac[(size_t) bestLag + 1];
    const double denom = a - 2.0 * b + c;
    const double shift = std::abs (denom) > 1e-12 ? juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / denom) : 0.0;
    const double period = bestLag + shift;

    double bpm = 60.0 * frameRate / period;
    while (bpm < minBpm) bpm *= 2.0;
    while (bpm >= maxBpm) bpm *= 0.5;

    // 3) Beat-grid fit: jointly refine tempo (+/-1 %, 0.01 BPM steps) and phase by maximising
    //    the comb-filtered onset energy over the whole analysed section. The autocorrelation
    //    estimate alone is only accurate to a fraction of a BPM, which would let the grid
    //    drift by tens of milliseconds over a few minutes.
    auto onsetAt = [&onset, numFrames] (double t) noexcept
    {
        const int i = (int) t;
        if (i < 0 || i + 1 >= numFrames) return 0.0;
        const double f = t - i;
        return onset[(size_t) i] * (1.0 - f) + onset[(size_t) i + 1] * f;
    };

    double bestBpm = bpm, bestPhaseFrames = 0.0, bestGridScore = -1.0;
    for (double candidate = bpm * 0.99; candidate <= bpm * 1.01; candidate += 0.01)
    {
        const double beatFrames = 60.0 * frameRate / candidate;
        for (double phase = 0.0; phase < beatFrames; phase += 0.5)
        {
            double sum = 0.0;
            for (double t = phase; t < numFrames - 1; t += beatFrames)
                sum += onsetAt (t);
            if (sum > bestGridScore) { bestGridScore = sum; bestBpm = candidate; bestPhaseFrames = phase; }
        }
    }

    // Most electronic music is produced at an integer tempo: prefer it when it fits the
    // grid (almost) as well as the fractional optimum.
    if (const double rounded = std::round (bestBpm); std::abs (rounded - bestBpm) < 0.06)
    {
        const double beatFrames = 60.0 * frameRate / rounded;
        double roundedScore = -1.0, roundedPhase = 0.0;
        for (double phase = 0.0; phase < beatFrames; phase += 0.5)
        {
            double sum = 0.0;
            for (double t = phase; t < numFrames - 1; t += beatFrames)
                sum += onsetAt (t);
            if (sum > roundedScore) { roundedScore = sum; roundedPhase = phase; }
        }
        if (roundedScore >= 0.97 * bestGridScore)
        {
            bestBpm = rounded;
            bestPhaseFrames = roundedPhase;
        }
    }

    bpm = bestBpm;
    const double bestPhase = bestPhaseFrames;

    result.bpm = std::round (bpm * 100.0) / 100.0;
    // A transient at time t lands in frame floor(t / hop); on average that frame starts half a
    // hop before the transient, so report the frame centre. Wrap into the first beat period.
    const double beatSeconds = 60.0 / result.bpm;
    result.firstBeatSeconds = std::fmod ((bestPhase + 0.5) / frameRate, beatSeconds);
    result.confidence = juce::jlimit (0.0, 1.0, b / (zeroLag / numFrames));
    return result;
}
} // namespace hype
