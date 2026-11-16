#include "WaveformData.h"

#include <cmath>

namespace hype
{
std::shared_ptr<const WaveformData> WaveformData::build (const float* left, const float* right,
                                                         int64_t numSamples, double sampleRate)
{
    auto w = std::make_shared<WaveformData>();
    w->sampleRate = sampleRate;
    w->samplesPerBin = juce::jmax (16, (int) std::round (sampleRate / 150.0));

    const auto numBins = (size_t) ((numSamples + w->samplesPerBin - 1) / w->samplesPerBin);
    w->peak.resize (numBins);
    w->low.resize (numBins);
    w->mid.resize (numBins);
    w->high.resize (numBins);

    auto onePole = [sampleRate] (double hz) { return (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * hz / sampleRate)); };
    const float cLow = onePole (200.0), cHigh = onePole (3000.0);
    float lo1 = 0, lo2 = 0, hi1 = 0, hi2 = 0;

    auto toByte = [] (double v) { return (uint8_t) juce::jlimit (0, 255, (int) std::lround (v * 255.0)); };

    for (size_t b = 0; b < numBins; ++b)
    {
        const auto start = (int64_t) b * w->samplesPerBin;
        const auto end = juce::jmin<int64_t> (numSamples, start + w->samplesPerBin);
        float pk = 0.0f;
        double eLow = 0, eMid = 0, eHigh = 0;

        for (auto i = start; i < end; ++i)
        {
            const float x = 0.5f * (left[i] + right[i]);
            pk = juce::jmax (pk, std::abs (left[i]), std::abs (right[i]));

            lo1 += cLow * (x - lo1);   lo2 += cLow * (lo1 - lo2);      // 2-pole low-pass
            hi1 += cHigh * (x - hi1);  hi2 += cHigh * (hi1 - hi2);     // complementary for high
            const float l = lo2;
            const float h = x - hi2;
            const float m = x - l - h;
            eLow += (double) l * l;
            eMid += (double) m * m;
            eHigh += (double) h * h;
        }

        const double n = (double) juce::jmax<int64_t> (1, end - start);
        w->peak[b] = toByte (pk);
        // sqrt-ish display scaling so quieter bands remain visible
        w->low[b]  = toByte (std::sqrt (std::sqrt (eLow / n)) * 1.0);
        w->mid[b]  = toByte (std::sqrt (std::sqrt (eMid / n)) * 1.1);
        w->high[b] = toByte (std::sqrt (std::sqrt (eHigh / n)) * 1.4);
    }
    return w;
}
} // namespace hype
