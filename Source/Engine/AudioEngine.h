#pragma once

#include "Api/HypeEngineApi.h"
#include "AudioDeviceManager.h"
#include "CueEngine.h"
#include "DeckEngine.h"
#include "MixerEngine.h"
#include "OutputRouter.h"
#include "TrackLoader.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <memory>

namespace hype
{
/**
    AudioEngine: owns the processing callback and composes the engine components.

        device callback (or silent clock)
          -> DeckEngine A/B render          (playback)
          -> MixerEngine                    (gain, EQ, faders, crossfader, master, limiter)
          -> CueEngine                      (headphone bus from the mixer's pre-fader taps)
          -> OutputRouter                   (physical master / headphone / direct outputs)

        AudioDeviceManager                  (hardware selection, output configuration, recovery)
        TrackLoader                         (background decode + analysis -> DeckEngine)

    Implements EngineInterface, the only thing the UI sees. The concrete component accessors
    below (getDeck(), getMixer()...) are for engine-side tools and tests.

    Real-time rules: the callback never allocates, locks, frees or waits. Both decks render from
    the same sample clock; transport commands can be scheduled to an exact engine sample.
    Any device buffer size is processed in chunks of at most config::maxInternalBlock frames.
*/
class AudioEngine : public EngineInterface,
                    private juce::AudioIODeviceCallback,
                    private AudioDeviceHost
{
public:
    AudioEngine();
    ~AudioEngine() override;

    static constexpr int maxOutputChannels = 64;

    //--- EngineInterface ----------------------------------------------------------
    DeckControl&   deck (int index) override  { return *decks[(size_t) index]; }
    MixerControl&  mixer() override           { return mixerEngine; }
    CueControl&    cue() override             { return cueEngine; }
    DeviceControl& devices() override         { return *deviceManager; }

    void loadTrack (int deck, const juce::File&, LoadCallback onFinished = {}) override;
    juce::String getSupportedFormatsWildcard() const override { return trackLoader.getSupportedFormatsWildcard(); }

    bool syncDeck (int slave, int master, bool alignPhase = true) override;
    bool setSyncLock (int slave, int master) override;
    int  getSyncLockMaster (int slave) const override { return publishedSyncMaster[slave].load (std::memory_order_relaxed); }
    bool playInSync (int slave, int master) override;

    int64_t getSampleClock() const override   { return sampleClock.load (std::memory_order_acquire); }
    double  getSampleRate() const override    { return currentSampleRate.load(); }
    void    setRunWithoutOutput (bool shouldRun) override;

    //--- concrete components (engine tools / tests) --------------------------------
    DeckEngine&         getDeck (int index) noexcept  { return *decks[(size_t) index]; }
    MixerEngine&        getMixer() noexcept           { return mixerEngine; }
    CueEngine&          getCue() noexcept             { return cueEngine; }
    OutputRouter&       getRouter() noexcept          { return router; }
    AudioDeviceManager& getDevices() noexcept         { return *deviceManager; }
    TrackLoader&        getTrackLoader() noexcept     { return trackLoader; }

    /** Blocking load (decodes on the calling thread; the swap still happens on the audio thread). */
    AudioFileDecoder::Result loadTrackSync (int deck, const juce::File& file) { return trackLoader.loadSync (deck, file); }

    bool isRunningOnSilentClock() const noexcept { return isRunningWithoutOutput(); }

    //--- manual / offline driving ---------------------------------------------------
    void prepareToPlay (double sampleRate, int expectedBlockSize = config::maxInternalBlock);
    /** Renders into `outputs` indexed by physical channel (any block size). Real-time safe. */
    void processBlock (float* const* outputs, int numOutputChannels, int numSamples) noexcept;
    /** Releases retired tracks (the housekeeping thread does this every 50 ms). */
    void collectGarbage();

private:
    enum class EngineCommandType : uint8_t { Sync, SyncLock, PlayInSync };
    struct EngineCommand { EngineCommandType type; int slave = 0, master = 0; bool flag = false; };

    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& message) override;

    // AudioDeviceHost
    juce::AudioIODeviceCallback& getAudioCallback() override { return *this; }
    uint32_t getCallbackCount() const override               { return callbackCounter.load (std::memory_order_relaxed); }
    bool hasDeviceError() const override                     { return deviceErrorFlag.load(); }
    double getCpuLoad() const override                       { return loadMeasurer.getLoadAsProportion(); }
    bool isRunningWithoutOutput() const override             { return silentClockEnabled.load() && ! deviceActive.load(); }

    void renderBlock (float* const* outputs, int numOutputChannels, int numSamples,
                      const int* physicalToPackedMap, int numPhysical) noexcept;
    bool pushEngineCommand (const EngineCommand&);
    void applyEngineCommand (const EngineCommand&) noexcept;
    void syncAudio (int slave, int master, bool alignPhase) noexcept;
    void updateSyncLocks() noexcept;
    static double matchTempoRatio (double ratio) noexcept;

    class Housekeeper;
    class SilentClock;

    //--- components (declaration order = construction order) ------------------------
    OutputRouter router;
    MixerEngine mixerEngine;
    CueEngine cueEngine;
    std::array<std::unique_ptr<DeckEngine>, config::numDecks> decks;
    TrackLoader trackLoader;

    //--- audio-thread state ---------------------------------------------------------
    juce::SpinLock engineProducerLock;
    SpscQueue<EngineCommand, 64> engineCommands;
    int syncMaster[config::numDecks];
    std::atomic<int> publishedSyncMaster[config::numDecks];
    int64_t scheduledPlay[config::numDecks];
    juce::AudioBuffer<float> deckBuffers;
    float* chunkOutputs[maxOutputChannels] {};
    int physicalToPacked[maxOutputChannels] {};
    int numPhysicalOutputs = 0;
    juce::AudioProcessLoadMeasurer loadMeasurer;

    std::atomic<int64_t> sampleClock { 0 };
    std::atomic<double>  currentSampleRate { 48000.0 };
    std::atomic<uint32_t> callbackCounter { 0 };
    std::atomic<bool> deviceErrorFlag { false };
    std::atomic<bool> silentClockEnabled { false }, deviceActive { false }, silentRendering { false };

    std::unique_ptr<Housekeeper> housekeeper;
    std::unique_ptr<SilentClock> silentClock;

    // Last: destroyed first, so the device is closed before anything it calls back into goes away.
    std::unique_ptr<AudioDeviceManager> deviceManager;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
} // namespace hype
