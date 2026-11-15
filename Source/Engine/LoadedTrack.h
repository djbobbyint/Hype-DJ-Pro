#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace hype
{
struct TrackInfo
{
    juce::File   file;
    juce::String title, artist, album;
    juce::String decoderName;          // "JUCE WAV", "FFmpeg aac", ...
    double       sampleRate        = 44100.0;
    juce::int64  lengthSamples     = 0;
    int          sourceChannels    = 2;
    double       bpm               = 0.0;  // 0 = unknown (to be filled by analysis / user)
    double       firstBeatSeconds  = 0.0;

    double getLengthSeconds() const noexcept { return sampleRate > 0 ? (double) lengthSamples / sampleRate : 0.0; }
};

/**
    A fully decoded track held in RAM at its native sample rate, always as
    2 channels (mono sources are duplicated, extra channels beyond 2 are dropped).

    Instances are immutable once handed to a Deck and are reference counted. The
    audio thread never releases the last reference: retired tracks are handed back
    to a background thread for destruction.
*/
class LoadedTrack : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<LoadedTrack>;

    LoadedTrack (juce::AudioBuffer<float>&& stereoAudio, TrackInfo trackInfo)
        : audio (std::move (stereoAudio)), info (std::move (trackInfo))
    {
        jassert (audio.getNumChannels() == 2);
        info.lengthSamples = audio.getNumSamples();
    }

    /** Builds a track from any buffer (mono/stereo/multichannel). Handy for tests and generated audio. */
    static Ptr fromBuffer (const juce::AudioBuffer<float>& source, double sampleRate, juce::String title = "Untitled")
    {
        juce::AudioBuffer<float> stereo (2, source.getNumSamples());
        stereo.copyFrom (0, 0, source, 0, 0, source.getNumSamples());
        stereo.copyFrom (1, 0, source, source.getNumChannels() > 1 ? 1 : 0, 0, source.getNumSamples());

        TrackInfo ti;
        ti.title = std::move (title);
        ti.sampleRate = sampleRate;
        ti.sourceChannels = source.getNumChannels();
        ti.decoderName = "memory";
        return new LoadedTrack (std::move (stereo), std::move (ti));
    }

    const float* getChannel (int ch) const noexcept { return audio.getReadPointer (ch); }
    juce::int64  getLength() const noexcept         { return audio.getNumSamples(); }
    double       getSampleRate() const noexcept     { return info.sampleRate; }
    const TrackInfo& getInfo() const noexcept       { return info; }

    /** Only valid before the track is handed to a Deck (the loader sets analysis results here). */
    void setBeatGrid (double bpm, double firstBeatSeconds) noexcept
    {
        info.bpm = bpm;
        info.firstBeatSeconds = firstBeatSeconds;
    }

private:
    juce::AudioBuffer<float> audio;
    TrackInfo info;

    JUCE_DECLARE_NON_COPYABLE (LoadedTrack)
};
} // namespace hype
