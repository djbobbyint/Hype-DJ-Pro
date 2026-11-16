#include "TrackLoader.h"
#include "BeatAnalyzer.h"

namespace hype
{
class TrackLoader::Job : public juce::ThreadPoolJob
{
public:
    Job (TrackLoader& l, int d, juce::File f, std::shared_ptr<std::atomic<bool>> c, LoadCallback cb)
        : juce::ThreadPoolJob ("HYPE load " + f.getFileName()),
          loader (l), deck (d), file (std::move (f)), cancel (std::move (c)), callback (std::move (cb)) {}

    JobStatus runJob() override
    {
        AudioFileDecoder decoder;
        auto result = TrackLoader::decodeAndAnalyse (decoder, file, cancel.get());

        if (cancel->load())
        {
            result.track = nullptr;
            result.error = "Cancelled (superseded by a newer load)";
        }
        else if (result.wasOk() && ! loader.sink (deck, result.track))
        {
            result.track = nullptr;
            result.error = "Deck command queue full";
        }

        if (callback)
            callback (deck, TrackLoader::toLoadResult (result));
        return jobHasFinished;
    }

private:
    TrackLoader& loader;
    const int deck;
    const juce::File file;
    std::shared_ptr<std::atomic<bool>> cancel;
    LoadCallback callback;
};

TrackLoader::TrackLoader (Sink s) : sink (std::move (s)) {}

TrackLoader::~TrackLoader()
{
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        for (auto& c : activeCancel)
            if (c != nullptr)
                c->store (true);
    }
    pool.removeAllJobs (true, 10000);
}

void TrackLoader::loadAsync (int deck, const juce::File& file, LoadCallback onFinished)
{
    jassert (deck >= 0 && deck < numDecks);
    auto cancel = std::make_shared<std::atomic<bool>> (false);
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        if (activeCancel[deck] != nullptr)
            activeCancel[deck]->store (true);
        activeCancel[deck] = cancel;
    }
    pool.addJob (new Job (*this, deck, file, std::move (cancel), std::move (onFinished)), true);
}

AudioFileDecoder::Result TrackLoader::loadSync (int deck, const juce::File& file)
{
    AudioFileDecoder decoder;
    auto result = decodeAndAnalyse (decoder, file);
    if (result.wasOk() && ! sink (deck, result.track))
        return { nullptr, "Deck command queue full" };
    return result;
}

AudioFileDecoder::Result TrackLoader::decodeAndAnalyse (AudioFileDecoder& decoder, const juce::File& file,
                                                        const std::atomic<bool>* cancelFlag)
{
    auto result = decoder.decode (file, cancelFlag);
    if (! result.wasOk())
        return result;

    auto& track = *result.track;

    if (track.getInfo().bpm <= 0.0)
    {
        // The track isn't shared with any deck yet; wrap its channels without copying.
        float* channels[2] = { const_cast<float*> (track.getChannel (0)), const_cast<float*> (track.getChannel (1)) };
        const juce::AudioBuffer<float> view (channels, 2, (int) track.getLength());
        const auto beats = analyseBeats (view, track.getSampleRate());
        if (beats.bpm > 0.0)
            track.setBeatGrid (beats.bpm, beats.firstBeatSeconds);
    }

    if (track.getWaveform() == nullptr)
        track.setWaveform (WaveformData::build (track.getChannel (0), track.getChannel (1),
                                                track.getLength(), track.getSampleRate()));
    return result;
}

LoadResult TrackLoader::toLoadResult (const AudioFileDecoder::Result& r)
{
    LoadResult out;
    out.ok = r.wasOk();
    out.error = r.error;
    if (r.track != nullptr)
    {
        const auto& info = r.track->getInfo();
        out.title = info.title;
        out.artist = info.artist;
        out.decoderName = info.decoderName;
        out.bpm = info.bpm;
        out.durationSeconds = info.getLengthSeconds();
        out.sampleRate = info.sampleRate;
    }
    return out;
}

juce::String TrackLoader::getSupportedFormatsWildcard() const
{
    return AudioFileDecoder().getWildcardForAllFormats();
}
} // namespace hype
