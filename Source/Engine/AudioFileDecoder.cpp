#include "AudioFileDecoder.h"
#include "EngineConfig.h"
#include "FFmpegDecoder.h"

namespace hype
{
AudioFileDecoder::AudioFileDecoder()
{
    // WAV, AIFF, FLAC, Ogg, MP3 (JUCE_USE_MP3AUDIOFORMAT), CoreAudio on Apple,
    // Windows Media on Windows (JUCE_USE_WINDOWS_MEDIA_FORMAT).
    formatManager.registerBasicFormats();
}

bool AudioFileDecoder::isFFmpegAvailable() noexcept
{
    return HYPE_HAS_FFMPEG != 0;
}

juce::String AudioFileDecoder::getWildcardForAllFormats() const
{
    auto wildcard = formatManager.getWildcardForAllFormats();

    if (isFFmpegAvailable())
        for (auto ext : { "*.m4a", "*.aac", "*.mp4", "*.alac", "*.wma", "*.opus" })
            if (! wildcard.contains (ext))
                wildcard << ";" << ext;

    return wildcard;
}

AudioFileDecoder::Result AudioFileDecoder::decode (const juce::File& file, const std::atomic<bool>* cancelFlag)
{
    if (! file.existsAsFile())
        return { nullptr, "File not found: " + file.getFullPathName() };

    auto result = decodeWithJuce (file, cancelFlag);

   #if HYPE_HAS_FFMPEG
    if (! result.wasOk() && ! (cancelFlag != nullptr && cancelFlag->load()))
    {
        auto ff = ffmpeg::decodeFile (file, cancelFlag);
        if (ff.wasOk())
            return { ff.track, {} };

        result.error << " | FFmpeg: " << ff.error;
    }
   #endif

    return result;
}

AudioFileDecoder::Result AudioFileDecoder::decodeWithJuce (const juce::File& file, const std::atomic<bool>* cancelFlag)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr)
        return { nullptr, "No JUCE decoder for " + file.getFileName() };

    if (reader->sampleRate <= 0 || reader->numChannels == 0)
        return { nullptr, "Invalid stream parameters in " + file.getFileName() };

    const auto length = reader->lengthInSamples;

    if (length <= 0)
        return { nullptr, "Empty or unreadable audio in " + file.getFileName() };

    if ((double) length / reader->sampleRate > config::maxTrackSeconds || length > std::numeric_limits<int>::max())
        return { nullptr, "Track too long: " + file.getFileName() };

    juce::AudioBuffer<float> audio (2, (int) length);
    audio.clear();

    // Decode in chunks so a cancel request is honoured quickly.
    constexpr int chunk = 1 << 16;
    for (juce::int64 pos = 0; pos < length; pos += chunk)
    {
        if (cancelFlag != nullptr && cancelFlag->load (std::memory_order_relaxed))
            return { nullptr, "Cancelled" };

        const auto num = (int) std::min<juce::int64> (chunk, length - pos);

        // useLeft/RightChan: mono sources are duplicated into both channels by the reader.
        if (! reader->read (&audio, (int) pos, num, pos, true, true))
            return { nullptr, "Read error in " + file.getFileName() };
    }

    TrackInfo info;
    info.file           = file;
    info.title          = file.getFileNameWithoutExtension();
    info.sampleRate     = reader->sampleRate;
    info.sourceChannels = (int) reader->numChannels;
    info.decoderName    = "JUCE " + reader->getFormatName();

    const auto& meta = reader->metadataValues;
    for (auto key : { "title", "TITLE", "id3title", "INAM" })
        if (meta.containsKey (key) && meta[key].isNotEmpty()) { info.title = meta[key]; break; }
    for (auto key : { "artist", "ARTIST", "id3artist", "IART" })
        if (meta.containsKey (key) && meta[key].isNotEmpty()) { info.artist = meta[key]; break; }

    return { new LoadedTrack (std::move (audio), std::move (info)), {} };
}
} // namespace hype
