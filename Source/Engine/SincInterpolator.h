#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace hype
{
/**
    Band-limited variable-rate reader for the "vinyl" (pitch follows tempo) playback path.

    16-tap Kaiser-windowed sinc, 256 polyphase sub-phases with linear interpolation between
    them. Several kernels with progressively lower cut-off are pre-computed; the kernel is
    chosen per block from the highest read increment, so playing faster than the source rate
    (speed-up and/or source SR > device SR) does not alias.

    The reader is stateless: its only "state" is the fractional read position owned by the
    caller, which is what makes sample-exact seeking and crossfading trivial.

    Everything is pre-computed in the constructor; process() never allocates.
*/
class SincInterpolator
{
public:
    static constexpr int halfTaps  = 8;
    static constexpr int numTaps   = halfTaps * 2;
    static constexpr int numPhases = 256;

    SincInterpolator();

    /** Picks the kernel appropriate for the given maximum |read increment|. */
    int chooseKernel (double maxAbsIncrement) const noexcept;

    /**
        Renders `numSamples` stereo frames.

        @param left/right      source channels (length `sourceLength`); positions outside
                               [0, sourceLength) read as silence
        @param position        fractional read position in source samples; advanced in place
        @param increments      per-output-sample read increment (may be negative or zero)
    */
    void process (const float* left, const float* right, int64_t sourceLength,
                  double& position, const float* increments,
                  float* outLeft, float* outRight, int numSamples, int kernel) const noexcept;

private:
    struct Kernel
    {
        // (numPhases + 1) rows of numTaps coefficients, plus per-row deltas to the next row.
        std::vector<float> coeffs, deltas;
    };

    static constexpr int numKernels = 6;
    static constexpr std::array<double, numKernels> kernelRatios { 1.0, 1.25, 1.5, 2.0, 3.0, 4.0 };

    std::array<Kernel, numKernels> kernels;
};
} // namespace hype
