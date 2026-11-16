#pragma once

#include "Api/HypeEngineApi.h"
#include "AudioFileDecoder.h"

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <memory>

namespace hype
{
/**
    TrackLoader: audio file loading and decoding.

    Runs decode (JUCE / FFmpeg) + beat analysis + waveform analysis on low-priority background
    threads, then hands the finished LoadedTrack to a sink (the AudioEngine, which queues it to
    the deck's audio thread with a single pointer exchange). Nothing here touches the audio
    thread. A newer request for the same deck cancels an older one that is still decoding.
*/
class TrackLoader
{
public:
    /** Delivers a decoded track to `deck`. Returns false if it could not be queued. */
    using Sink = std::function<bool (int deck, LoadedTrack::Ptr)>;

    explicit TrackLoader (Sink sink);
    ~TrackLoader();   // cancels pending loads and waits for the workers

    /** Background load; `onFinished` runs on the loader thread. */
    void loadAsync (int deck, const juce::File& file, LoadCallback onFinished = {});

    /** Blocking load on the calling thread (tools, tests). */
    AudioFileDecoder::Result loadSync (int deck, const juce::File& file);

    /** Decode + beat grid + waveform, without loading into a deck. */
    static AudioFileDecoder::Result decodeAndAnalyse (AudioFileDecoder& decoder, const juce::File& file,
                                                      const std::atomic<bool>* cancelFlag = nullptr);

    static LoadResult toLoadResult (const AudioFileDecoder::Result&);
    juce::String getSupportedFormatsWildcard() const;

private:
    class Job;

    Sink sink;
    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withThreadName ("HYPE loader").withNumberOfThreads (2)
                                                     .withDesiredThreadPriority (juce::Thread::Priority::low) };
    juce::SpinLock lock;
    std::shared_ptr<std::atomic<bool>> activeCancel[numDecks];

    JUCE_DECLARE_NON_COPYABLE (TrackLoader)
};
} // namespace hype
