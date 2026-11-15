#pragma once

#include "LoadedTrack.h"
#include <atomic>

namespace hype::ffmpeg
{
struct Result
{
    LoadedTrack::Ptr track;
    juce::String     error;
    bool wasOk() const noexcept { return track != nullptr; }
};

/** Decodes the first audio stream of any container FFmpeg understands to stereo float
    at the stream's native sample rate. Only functional when built with HYPE_HAS_FFMPEG. */
Result decodeFile (const juce::File& file, const std::atomic<bool>* cancelFlag);
} // namespace hype::ffmpeg
