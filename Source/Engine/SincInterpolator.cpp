#include "SincInterpolator.h"

#include <algorithm>
#include <cmath>

namespace hype
{
namespace
{
    double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
            if (term < 1e-12 * sum)
                break;
        }
        return sum;
    }

    double kaiser (double x, double beta) // x in [-1, 1]
    {
        if (std::abs (x) >= 1.0)
            return 0.0;
        return besselI0 (beta * std::sqrt (1.0 - x * x)) / besselI0 (beta);
    }

    double sinc (double x)
    {
        if (std::abs (x) < 1e-9)
            return 1.0;
        const double px = 3.14159265358979323846 * x;   // (M_PI is not standard C++)
        return std::sin (px) / px;
    }
}

SincInterpolator::SincInterpolator()
{
    constexpr double beta = 7.5;
    constexpr double baseCutoff = 0.92; // fraction of Nyquist at unity rate

    for (int k = 0; k < numKernels; ++k)
    {
        const double cutoff = baseCutoff / kernelRatios[(size_t) k];
        auto& kernel = kernels[(size_t) k];
        kernel.coeffs.assign ((size_t) (numPhases + 1) * numTaps, 0.0f);
        kernel.deltas.assign ((size_t) (numPhases + 1) * numTaps, 0.0f);

        for (int p = 0; p <= numPhases; ++p)
        {
            const double frac = (double) p / numPhases;
            double row[numTaps];
            double sum = 0.0;

            // Tap t multiplies source sample (i0 - halfTaps + 1 + t); its distance from
            // the read position (i0 + frac) is x = t - (halfTaps - 1) - frac.
            for (int t = 0; t < numTaps; ++t)
            {
                const double x = (double) (t - (halfTaps - 1)) - frac;
                row[t] = cutoff * sinc (cutoff * x) * kaiser (x / halfTaps, beta);
                sum += row[t];
            }

            for (int t = 0; t < numTaps; ++t) // normalise for exact unity DC gain
                kernel.coeffs[(size_t) (p * numTaps + t)] = (float) (row[t] / sum);
        }

        for (int p = 0; p < numPhases; ++p)
            for (int t = 0; t < numTaps; ++t)
                kernel.deltas[(size_t) (p * numTaps + t)] = kernel.coeffs[(size_t) ((p + 1) * numTaps + t)]
                                                          - kernel.coeffs[(size_t) (p * numTaps + t)];
    }
}

int SincInterpolator::chooseKernel (double maxAbsIncrement) const noexcept
{
    for (int k = 0; k < numKernels; ++k)
        if (maxAbsIncrement <= kernelRatios[(size_t) k] * 1.0001)
            return k;
    return numKernels - 1;
}

void SincInterpolator::process (const float* left, const float* right, int64_t sourceLength,
                                double& position, const float* increments,
                                float* outLeft, float* outRight, int numSamples, int kernelIndex) const noexcept
{
    const auto& kernel = kernels[(size_t) std::clamp (kernelIndex, 0, numKernels - 1)];
    const float* coeffs = kernel.coeffs.data();
    const float* deltas = kernel.deltas.data();

    double pos = position;

    for (int i = 0; i < numSamples; ++i)
    {
        const double fl = std::floor (pos);
        const auto i0 = (int64_t) fl;
        const double frac = pos - fl;

        const double phaseF = frac * numPhases;
        const int phase = std::min ((int) phaseF, numPhases - 1);
        const float pFrac = (float) (phaseF - phase);

        const float* c = coeffs + phase * numTaps;
        const float* d = deltas + phase * numTaps;

        const int64_t first = i0 - (halfTaps - 1);
        float accL = 0.0f, accR = 0.0f;

        if (first >= 0 && first + numTaps <= sourceLength)
        {
            const float* l = left + first;
            const float* r = right + first;
            for (int t = 0; t < numTaps; ++t)
            {
                const float w = c[t] + pFrac * d[t];
                accL += w * l[t];
                accR += w * r[t];
            }
        }
        else if (first + numTaps > 0 && first < sourceLength)
        {
            for (int t = 0; t < numTaps; ++t)
            {
                const int64_t idx = first + t;
                if (idx < 0 || idx >= sourceLength)
                    continue;
                const float w = c[t] + pFrac * d[t];
                accL += w * left[idx];
                accR += w * right[idx];
            }
        }

        outLeft[i] = accL;
        outRight[i] = accR;
        pos += (double) increments[i];
    }

    position = pos;
}
} // namespace hype
