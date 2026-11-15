#pragma once

#include "LoadedTrack.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>

namespace hype
{
/**
    Decodes an audio file completely into memory (see LoadedTrack).

    Decoder chain:
      1. JUCE's built-in readers: WAV, AIFF, FLAC, Ogg Vorbis, MP3, plus
         CoreAudio (macOS/iOS: AAC/M4A/ALAC) or Media Foundation (Windows: AAC/WMA).
      2. FFmpeg (if built with HYPE_HAS_FFMPEG) for anything JUCE could not open,
         notably AAC/M4A on Linux.

    Not thread-safe: create one decoder per loader thread.
*/
class AudioFileDecoder
{
public:
    struct Result
    {
        LoadedTrack::Ptr track;
        juce::String     error;
        bool wasOk() const noexcept { return track != nullptr; }
    };

    AudioFileDecoder();

    /** Blocking. `cancelFlag` may be set from another thread to abort early. */
    Result decode (const juce::File& file, const std::atomic<bool>* cancelFlag = nullptr);

    /** e.g. "*.wav;*.aiff;*.mp3;..." - everything at least one decoder claims. */
    juce::String getWildcardForAllFormats() const;

    static bool isFFmpegAvailable() noexcept;

private:
    Result decodeWithJuce (const juce::File&, const std::atomic<bool>*);

    juce::AudioFormatManager formatManager;
};
} // namespace hype
