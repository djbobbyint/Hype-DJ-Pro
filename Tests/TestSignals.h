#pragma once

// Shared test fixtures: synthetic "music" tracks with known fingerprints, file writing,
// and signal measurements.

#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>
#include <random>

namespace testsig
{
constexpr double pi = juce::MathConstants<double>::pi;

/** A music-like test track: kick on every beat, a bassline, a sustained 3-note chord pad and
    off-beat hi-hats. The pad notes are the track's fingerprint (measurable with toneAmplitude). */
struct TrackSpec
{
    double bpm = 126.0;
    double chord[3] { 261.63, 329.63, 392.00 };   // C major
    double bass = 65.41;
    double lead = 783.99;          // mid-band fingerprint (G5), far from the EQ crossovers
    double seconds = 60.0;
    double sampleRate = 44100.0;
    float padLevel = 0.08f;
    bool hats = true;
    bool kick = true;
    unsigned seed = 1;
};

inline juce::AudioBuffer<float> makeMusic (const TrackSpec& t)
{
    const int n = (int) (t.seconds * t.sampleRate);
    juce::AudioBuffer<float> b (2, n);
    std::minstd_rand rng (t.seed);
    std::uniform_real_distribution<float> noise (-1.0f, 1.0f);
    const double beat = 60.0 / t.bpm;
    float hpState = 0.0f, prevNoise = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const double time = i / t.sampleRate;
        const double tb = std::fmod (time, beat);
        double v = 0.0;

        if (t.kick && tb < 0.30)
        {
            const double f0 = 50.0, f1 = 140.0, k = 35.0;   // pitch sweep 140 -> 50 Hz
            const double phase = 2.0 * pi * (f0 * tb + (f1 - f0) * (1.0 - std::exp (-k * tb)) / k);
            v += 0.45 * std::sin (phase) * std::exp (-tb * 9.0);
        }
        v += 0.18 * std::sin (2.0 * pi * t.bass * time) * (0.55 + 0.45 * std::cos (2.0 * pi * tb / beat));
        for (double f : t.chord)
            v += t.padLevel * std::sin (2.0 * pi * f * time);
        v += t.padLevel * std::sin (2.0 * pi * t.lead * time);

        if (t.hats)
        {
            const double th = std::fmod (time + beat * 0.5, beat);
            const float nz = noise (rng);
            hpState = 0.5f * (hpState + nz - prevNoise);    // crude high-pass
            prevNoise = nz;
            if (th < 0.05)
                v += 0.12 * hpState * std::exp (-th * 70.0);
        }

        b.setSample (0, i, (float) v);
        b.setSample (1, i, (float) v);
    }
    return b;
}

/** Pure, click-free tonal material (for discontinuity detection in soak tests). */
inline juce::AudioBuffer<float> makeTones (double seconds, double sampleRate, double f1, double f2, float level = 0.25f)
{
    juce::AudioBuffer<float> b (2, (int) (seconds * sampleRate));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const double t = i / sampleRate;
        const float v = level * (float) (std::sin (2.0 * pi * f1 * t) + 0.6 * std::sin (2.0 * pi * f2 * t)) / 1.6f;
        b.setSample (0, i, v);
        b.setSample (1, i, v);
    }
    return b;
}

/** Writes WAV / AIFF / FLAC with JUCE; MP3 / M4A via the ffmpeg CLI if available. */
inline bool writeAudioFile (const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& file)
{
    file.deleteFile();
    const auto ext = file.getFileExtension().toLowerCase();

    if (ext == ".mp3" || ext == ".m4a")
    {
        auto tmp = file.withFileExtension (".tmp.wav");
        if (! writeAudioFile (audio, sampleRate, tmp))
            return false;
        juce::ChildProcess p;
        if (! p.start (juce::StringArray { "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", tmp.getFullPathName(),
                                           "-c:a", ext == ".mp3" ? "libmp3lame" : "aac", "-b:a", "256k", file.getFullPathName() }))
            return false;
        p.readAllProcessOutput();
        tmp.deleteFile();
        return file.existsAsFile();
    }

    std::unique_ptr<juce::AudioFormat> format;
    if (ext == ".wav")       format = std::make_unique<juce::WavAudioFormat>();
    else if (ext == ".aiff") format = std::make_unique<juce::AiffAudioFormat>();
    else if (ext == ".flac") format = std::make_unique<juce::FlacAudioFormat>();
    else return false;

    std::unique_ptr<juce::OutputStream> os (file.createOutputStream().release());
    if (os == nullptr)
        return false;
    std::unique_ptr<juce::AudioFormatWriter> w (format->createWriterFor (os.get(), sampleRate, 2, 24, {}, 0));
    if (w == nullptr)
        return false;
    os.release();
    return w->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
}

inline bool ffmpegAvailable()
{
    juce::ChildProcess p;
    return p.start ("ffmpeg -version") && p.readAllProcessOutput().contains ("ffmpeg");
}

/** Amplitude of a sinusoid at `freq` within [start, end) samples (Goertzel / DFT bin). */
inline double toneAmplitude (const float* x, int start, int end, double freq, double sampleRate)
{
    const double w = 2.0 * pi * freq / sampleRate;
    double re = 0, im = 0;
    for (int i = start; i < end; ++i)
    {
        // Hann window to limit leakage from neighbouring tones
        const double win = 0.5 - 0.5 * std::cos (2.0 * pi * (i - start) / (end - start - 1));
        re += win * x[i] * std::cos (w * i);
        im -= win * x[i] * std::sin (w * i);
    }
    return 4.0 * std::sqrt (re * re + im * im) / (end - start);   // x2 for Hann coherent gain
}

inline double toneAmplitude (const juce::AudioBuffer<float>& b, int ch, double freq, double sampleRate)
{
    return toneAmplitude (b.getReadPointer (ch), 0, b.getNumSamples(), freq, sampleRate);
}

/** RMS of a band of frequencies [lo, hi] using a pair of 2nd-order filters (rough band energy). */
inline double bandRms (const juce::AudioBuffer<float>& b, int ch, double lo, double hi, double sampleRate)
{
    // 4th-order band-pass (two cascaded 2nd-order high- and low-passes)
    juce::IIRFilter hp1, hp2, lp1, lp2;
    hp1.setCoefficients (juce::IIRCoefficients::makeHighPass (sampleRate, lo));
    hp2.setCoefficients (juce::IIRCoefficients::makeHighPass (sampleRate, lo));
    lp1.setCoefficients (juce::IIRCoefficients::makeLowPass (sampleRate, hi));
    lp2.setCoefficients (juce::IIRCoefficients::makeLowPass (sampleRate, hi));
    juce::HeapBlock<float> tmp ((size_t) b.getNumSamples());
    std::copy (b.getReadPointer (ch), b.getReadPointer (ch) + b.getNumSamples(), tmp.get());
    for (auto* f : { &hp1, &hp2, &lp1, &lp2 })
        f->processSamples (tmp.get(), b.getNumSamples());
    double sum = 0;
    const int skip = juce::jmin (b.getNumSamples() / 4, (int) (sampleRate * 0.05));   // filter settling
    for (int i = skip; i < b.getNumSamples(); ++i)
        sum += (double) tmp[i] * tmp[i];
    return std::sqrt (sum / juce::jmax (1, b.getNumSamples() - skip));
}

inline double db (double gain) { return 20.0 * std::log10 (juce::jmax (1.0e-12, gain)); }

inline bool allFinite (const juce::AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (b.getSample (c, i)))
                return false;
    return true;
}
} // namespace testsig
