#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace hype
{
/**
    Lightweight offline tempo / beat-grid estimator, run on the loader thread after decoding.

    Method: onset-strength envelope (log energy of the differentiated signal, half-wave
    rectified flux) -> autocorrelation over 60..200 BPM with a mild prior around 120 BPM ->
    parabolic refinement -> octave folding into [minBpm, maxBpm) -> comb-filter phase search
    for the first downbeat.

    It is deliberately simple: good for steady 4/4 dance music, and the result is always
    overridable via Deck::setBeatGrid(). A full analysis pipeline (beat tracking with tempo
    drift, key detection, waveform overview) belongs in a dedicated analysis module.
*/
struct BeatAnalysis
{
    double bpm = 0.0;              // 0 = no confident estimate
    double firstBeatSeconds = 0.0;
    double confidence = 0.0;       // 0..1
};

BeatAnalysis analyseBeats (const juce::AudioBuffer<float>& stereo, double sampleRate,
                           double minBpm = 85.0, double maxBpm = 175.0,
                           double maxSecondsToAnalyse = 180.0);
} // namespace hype
