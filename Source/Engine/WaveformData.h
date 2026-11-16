#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cstdint>
#include <vector>

namespace hype
{
/**
    Pre-computed waveform summary for display, built on the loader thread.

    One bin covers `samplesPerBin` source samples (~150 bins per second). Each bin stores the
    peak level and the RMS of three frequency bands (low < 200 Hz, mid, high > 3 kHz), each
    0..255. The UI colours the waveform from the band mix: bass red, mids green, highs blue.
    Immutable once built; shared read-only with the UI.
*/
struct WaveformData
{
    int samplesPerBin = 294;
    double sampleRate = 44100.0;
    std::vector<uint8_t> peak, low, mid, high;

    int getNumBins() const noexcept { return (int) peak.size(); }
    double binsPerSecond() const noexcept { return sampleRate / samplesPerBin; }

    static std::shared_ptr<const WaveformData> build (const float* left, const float* right,
                                                      int64_t numSamples, double sampleRate);
};
} // namespace hype
