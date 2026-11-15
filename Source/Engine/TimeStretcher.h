#pragma once

#include <cstdint>
#include <memory>

namespace hype
{
/** Read-only view of a stereo source; indices outside [0, length) read as silence. */
struct StereoSource
{
    const float* channels[2] { nullptr, nullptr };
    int64_t      length = 0;

    float get (int ch, int64_t i) const noexcept
    {
        return (i >= 0 && i < length) ? channels[ch][i] : 0.0f;
    }
};

/**
    Key-lock architecture.

    Abstract time-stretch / pitch-shift processor used by the Deck whenever playback must
    decouple pitch from tempo (key lock enabled, or a key shift applied). The Deck owns the
    timing: it decides how many source samples to feed per output chunk and keeps the reported
    playhead aligned with what is heard, so implementations only need to provide:

      - prime():   reset and pre-roll so the next output sample corresponds exactly to
                   source sample `start` (used after play/seek/mode switch)
      - process(): consume `inputCount` source samples starting at `start` and produce
                   exactly `outputCount` output samples
      - setTransposeFactor(): frequency multiplier applied to the output

    The default implementation wraps Signalsmith Stretch (MIT). Commercial engines
    (e.g. zplane elastique, Rubber Band R3) can be dropped in by implementing this
    interface and returning them from createDefaultTimeStretcher().

    Real-time contract: prepare() may allocate; every other method is called on the audio
    thread and must not allocate, lock or block.
*/
class TimeStretcher
{
public:
    virtual ~TimeStretcher() = default;

    virtual const char* getName() const noexcept = 0;

    virtual void prepare (double sampleRate, int numChannels) = 0;

    /** Number of source samples prime() needs for the given input/output rate. */
    virtual int  getPrimeLength (double rate) const noexcept = 0;

    virtual void prime (const StereoSource& source, int64_t start, int length) noexcept = 0;

    virtual void process (const StereoSource& source, int64_t start, int inputCount,
                          float* outLeft, float* outRight, int outputCount) noexcept = 0;

    virtual void setTransposeFactor (double factor) noexcept = 0;

    /** Source samples between the newest input and the "processing time". */
    virtual int getInputLatency() const noexcept = 0;

    /** Output samples between the "processing time" and the output. */
    virtual int getOutputLatency() const noexcept = 0;
};

std::unique_ptr<TimeStretcher> createDefaultTimeStretcher();
} // namespace hype
