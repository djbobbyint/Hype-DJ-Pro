#include "FFmpegDecoder.h"
#include "EngineConfig.h"

#if HYPE_HAS_FFMPEG

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <vector>

namespace hype::ffmpeg
{
namespace
{
    juce::String errorString (int code)
    {
        char buf[AV_ERROR_MAX_STRING_SIZE] {};
        av_strerror (code, buf, sizeof (buf));
        return buf;
    }

    struct FormatCloser  { void operator() (AVFormatContext* c) const { avformat_close_input (&c); } };
    struct CodecCloser   { void operator() (AVCodecContext* c)  const { avcodec_free_context (&c); } };
    struct SwrCloser     { void operator() (SwrContext* c)      const { swr_free (&c); } };
    struct PacketCloser  { void operator() (AVPacket* p)        const { av_packet_free (&p); } };
    struct FrameCloser   { void operator() (AVFrame* f)         const { av_frame_free (&f); } };

    juce::String tag (AVDictionary* dict, const char* key)
    {
        if (auto* e = av_dict_get (dict, key, nullptr, 0))
            return juce::String::fromUTF8 (e->value);
        return {};
    }
}

Result decodeFile (const juce::File& file, const std::atomic<bool>* cancelFlag)
{
    auto cancelled = [cancelFlag] { return cancelFlag != nullptr && cancelFlag->load (std::memory_order_relaxed); };

    AVFormatContext* rawFmt = nullptr;
    if (int err = avformat_open_input (&rawFmt, file.getFullPathName().toRawUTF8(), nullptr, nullptr); err < 0)
        return { nullptr, "open failed: " + errorString (err) };
    std::unique_ptr<AVFormatContext, FormatCloser> fmt (rawFmt);

    if (int err = avformat_find_stream_info (fmt.get(), nullptr); err < 0)
        return { nullptr, "no stream info: " + errorString (err) };

    const AVCodec* codec = nullptr;
    const int streamIndex = av_find_best_stream (fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (streamIndex < 0 || codec == nullptr)
        return { nullptr, "no audio stream" };

    AVStream* stream = fmt->streams[streamIndex];

    std::unique_ptr<AVCodecContext, CodecCloser> dec (avcodec_alloc_context3 (codec));
    if (dec == nullptr)
        return { nullptr, "out of memory" };

    avcodec_parameters_to_context (dec.get(), stream->codecpar);
    if (int err = avcodec_open2 (dec.get(), codec, nullptr); err < 0)
        return { nullptr, "codec open failed: " + errorString (err) };

    const int sampleRate = dec->sample_rate;
    if (sampleRate <= 0 || dec->ch_layout.nb_channels <= 0)
        return { nullptr, "invalid stream parameters" };

    // Resample/convert to planar float stereo at the native rate.
    AVChannelLayout outLayout;
    av_channel_layout_default (&outLayout, 2);

    AVChannelLayout inLayout;
    if (dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default (&inLayout, dec->ch_layout.nb_channels);
    else
        av_channel_layout_copy (&inLayout, &dec->ch_layout);

    SwrContext* rawSwr = nullptr;
    int swrErr = swr_alloc_set_opts2 (&rawSwr, &outLayout, AV_SAMPLE_FMT_FLTP, sampleRate,
                                      &inLayout, dec->sample_fmt, sampleRate, 0, nullptr);
    av_channel_layout_uninit (&inLayout);
    std::unique_ptr<SwrContext, SwrCloser> swr (rawSwr);
    if (swrErr < 0 || swr_init (swr.get()) < 0)
        return { nullptr, "resampler init failed" };

    std::unique_ptr<AVPacket, PacketCloser> packet (av_packet_alloc());
    std::unique_ptr<AVFrame, FrameCloser> frame (av_frame_alloc());

    std::vector<float> left, right;
    if (fmt->duration > 0)
    {
        const auto estimate = (size_t) ((double) fmt->duration / AV_TIME_BASE * sampleRate * 1.02) + 4096;
        left.reserve (estimate);
        right.reserve (estimate);
    }

    const size_t maxSamples = (size_t) (config::maxTrackSeconds * sampleRate);
    std::vector<float> tmpL (8192), tmpR (8192);
    bool tooLong = false;

    auto drainDecoder = [&]() -> int
    {
        for (;;)
        {
            int r = avcodec_receive_frame (dec.get(), frame.get());
            if (r == AVERROR (EAGAIN) || r == AVERROR_EOF)
                return 0;
            if (r < 0)
                return r;

            const int outCap = (int) swr_get_out_samples (swr.get(), frame->nb_samples);
            if ((int) tmpL.size() < outCap) { tmpL.resize ((size_t) outCap); tmpR.resize ((size_t) outCap); }

            uint8_t* outPlanes[2] = { reinterpret_cast<uint8_t*> (tmpL.data()), reinterpret_cast<uint8_t*> (tmpR.data()) };
            const int got = swr_convert (swr.get(), outPlanes, outCap,
                                         const_cast<const uint8_t**> (frame->extended_data), frame->nb_samples);
            av_frame_unref (frame.get());
            if (got < 0)
                return got;

            left.insert (left.end(), tmpL.begin(), tmpL.begin() + got);
            right.insert (right.end(), tmpR.begin(), tmpR.begin() + got);

            if (left.size() > maxSamples) { tooLong = true; return 0; }
        }
    };

    while (! tooLong)
    {
        if (cancelled())
            return { nullptr, "Cancelled" };

        int r = av_read_frame (fmt.get(), packet.get());
        if (r < 0)
            break; // EOF (or unrecoverable read error - keep what we decoded)

        if (packet->stream_index == streamIndex)
        {
            // A corrupt packet is skipped rather than aborting the whole track.
            if (avcodec_send_packet (dec.get(), packet.get()) == 0)
                if (int dr = drainDecoder(); dr < 0 && dr != AVERROR_INVALIDDATA)
                {
                    av_packet_unref (packet.get());
                    return { nullptr, "decode error: " + errorString (dr) };
                }
        }
        av_packet_unref (packet.get());
    }

    if (tooLong)
        return { nullptr, "Track too long" };

    // Flush decoder and resampler.
    avcodec_send_packet (dec.get(), nullptr);
    drainDecoder();
    {
        uint8_t* outPlanes[2] = { reinterpret_cast<uint8_t*> (tmpL.data()), reinterpret_cast<uint8_t*> (tmpR.data()) };
        const int got = swr_convert (swr.get(), outPlanes, (int) tmpL.size(), nullptr, 0);
        if (got > 0)
        {
            left.insert (left.end(), tmpL.begin(), tmpL.begin() + got);
            right.insert (right.end(), tmpR.begin(), tmpR.begin() + got);
        }
    }

    if (left.empty())
        return { nullptr, "no audio decoded" };

    juce::AudioBuffer<float> audio (2, (int) left.size());
    std::copy (left.begin(), left.end(), audio.getWritePointer (0));
    std::copy (right.begin(), right.end(), audio.getWritePointer (1));

    TrackInfo info;
    info.file           = file;
    info.sampleRate     = sampleRate;
    info.sourceChannels = dec->ch_layout.nb_channels;
    info.decoderName    = juce::String ("FFmpeg ") + codec->name;
    info.title          = tag (fmt->metadata, "title");
    info.artist         = tag (fmt->metadata, "artist");
    info.album          = tag (fmt->metadata, "album");
    if (info.title.isEmpty())
        info.title = file.getFileNameWithoutExtension();

    return { new LoadedTrack (std::move (audio), std::move (info)), {} };
}
} // namespace hype::ffmpeg

#else

namespace hype::ffmpeg
{
Result decodeFile (const juce::File&, const std::atomic<bool>*)
{
    return { nullptr, "FFmpeg support not compiled in" };
}
} // namespace hype::ffmpeg

#endif
