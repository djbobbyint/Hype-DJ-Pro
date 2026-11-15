#include "TimeStretcher.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <cmath>
#include <random>
#include <vector>

namespace hype
{
namespace
{
    /** Adapts StereoSource to the `inputs[channel][index]` shape Signalsmith expects. */
    struct SourceAdapter
    {
        struct Channel
        {
            const StereoSource* src;
            int ch;
            int64_t offset;
            float operator[] (int i) const noexcept { return src->get (ch, offset + i); }
        };

        const StereoSource* src;
        int64_t offset;
        Channel operator[] (int ch) const noexcept { return { src, ch, offset }; }
    };

    class SignalsmithTimeStretcher final : public TimeStretcher
    {
    public:
        const char* getName() const noexcept override { return "Signalsmith Stretch"; }

        void prepare (double sampleRate, int numChannels) override
        {
            // splitComputation spreads the spectral work evenly across callbacks, avoiding
            // periodic CPU spikes (costs one extra interval of latency).
            stretch.presetDefault (numChannels, (float) sampleRate, true);
            stretch.setTransposeFactor (1.0f);

            // Warm up with noise so every internal vector reaches its working capacity now,
            // rather than on the audio thread later.
            std::vector<float> noise ((size_t) (sampleRate * 2.0));
            std::minstd_rand rng (1234);
            std::uniform_real_distribution<float> dist (-0.5f, 0.5f);
            for (auto& s : noise) s = dist (rng);

            StereoSource src;
            src.channels[0] = src.channels[1] = noise.data();
            src.length = (int64_t) noise.size();

            std::vector<float> outL (4096), outR (4096);
            for (double rate : { 4.0, 1.0, 0.25 })
            {
                const int primeLen = getPrimeLength (rate);
                prime (src, 0, primeLen);
                int64_t feed = primeLen;
                for (int block = 0; block < 16; ++block)
                {
                    const int inCount = (int) std::ceil (rate * 256);
                    process (src, feed, inCount, outL.data(), outR.data(), 256);
                    feed += inCount;
                }
            }
            stretch.reset();
        }

        int getPrimeLength (double rate) const noexcept override
        {
            return stretch.outputSeekLength ((float) rate);
        }

        void prime (const StereoSource& source, int64_t start, int length) noexcept override
        {
            stretch.outputSeek (SourceAdapter { &source, start }, length);
        }

        void process (const StereoSource& source, int64_t start, int inputCount,
                      float* outLeft, float* outRight, int outputCount) noexcept override
        {
            float* outs[2] = { outLeft, outRight };
            stretch.process (SourceAdapter { &source, start }, inputCount, outs, outputCount);
        }

        void setTransposeFactor (double factor) noexcept override
        {
            if (std::abs (factor - lastTranspose) > 1.0e-12)
            {
                lastTranspose = factor;
                stretch.setTransposeFactor ((float) factor);
            }
        }

        int getInputLatency() const noexcept override  { return stretch.inputLatency(); }
        int getOutputLatency() const noexcept override { return stretch.outputLatency(); }

    private:
        signalsmith::stretch::SignalsmithStretch<float> stretch { 0x48595045 /* fixed seed: deterministic output */ };
        double lastTranspose = 1.0;
    };
}

std::unique_ptr<TimeStretcher> createDefaultTimeStretcher()
{
    return std::make_unique<SignalsmithTimeStretcher>();
}
} // namespace hype
