#pragma once

#include "AudioFileDecoder.h"
#include "Deck.h"
#include "Mixer.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <functional>
#include <memory>

namespace hype
{
/**
    HYPE DJ PRO core engine: two Decks + Mixer driven by one audio device callback.

    * Both decks are rendered in the same callback from the same sample clock, so they are
      inherently sample-synchronous; transport commands can be scheduled at an exact engine
      sample (getSampleClock()) for tight, repeatable timing.
    * The callback never allocates, locks or frees memory. Device buffers of any size are
      processed in chunks of at most config::maxInternalBlock frames.
    * Track decoding + beat analysis happen on a background thread pool; decoded tracks are
      handed to the deck through a lock-free queue and released on a housekeeping thread.

    The engine can also be driven without an audio device (prepareToPlay + processBlock), which
    is how the tests and offline rendering work.
*/
class AudioEngine : private juce::AudioIODeviceCallback
{
public:
    struct DeviceSettings
    {
        juce::String deviceType;        // "ALSA", "JACK", "CoreAudio", "ASIO", "Windows Audio"...; empty = default
        juce::String outputDevice;      // empty = default device for the type
        double       sampleRate = 0.0;  // 0 = device default
        int          bufferSize = 0;    // 0 = device default (prefer 128..512 for DJ use)
        int          numOutputChannels = 4; // 2 = master only, 4 = master + headphone cue
    };

    struct DeviceStatus
    {
        bool         open = false;
        juce::String typeName, deviceName;
        double       sampleRate = 0.0;
        int          bufferSize = 0;
        int          numOutputChannels = 0;
        double       outputLatencyMs = 0.0;
        int          xruns = -1;          // -1 if the driver can't report it
        double       cpuLoad = 0.0;       // 0..1 of the callback's time budget
    };

    /** Called on a background thread when a load finishes (successfully or not). */
    using LoadCallback = std::function<void (int deck, const AudioFileDecoder::Result&)>;

    AudioEngine();
    ~AudioEngine() override;

    static constexpr int maxOutputChannels = 64;

    //--- device ---------------------------------------------------------------
    /** Registers the engine as the device manager's audio callback (idempotent). Used by
        AudioDeviceController, which manages the device itself. */
    void attachToDeviceManager();

    /** Returns an empty string on success, otherwise an error message. */
    juce::String openDevice (const DeviceSettings& settings);
    juce::String openDevice()   { return openDevice (DeviceSettings {}); }
    void closeDevice();
    DeviceStatus getDeviceStatus() const;
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    //--- decks / mixer --------------------------------------------------------
    Deck&  getDeck (int index) noexcept       { return *decks[(size_t) index]; }
    Mixer& getMixer() noexcept                { return mixer; }
    const Mixer& getMixer() const noexcept    { return mixer; }

    //--- loading --------------------------------------------------------------
    /** Decodes + analyses on a background thread, then swaps the track into the deck.
        A newer request for the same deck cancels an older one still in progress. */
    void loadTrackAsync (int deck, const juce::File& file, LoadCallback onFinished = {});

    /** Blocking variant (decodes on the calling thread). The swap still happens on the audio thread. */
    AudioFileDecoder::Result loadTrackSync (int deck, const juce::File& file);

    /** Decode + beat analysis without loading into a deck. */
    static AudioFileDecoder::Result decodeAndAnalyse (AudioFileDecoder& decoder, const juce::File& file,
                                                      const std::atomic<bool>* cancelFlag = nullptr);

    juce::String getSupportedFormatsWildcard() const;

    //--- synchronisation (executed sample-exactly on the audio thread) ---------
    /** One-shot: match `slave` tempo to `master` (with half/double-time matching) and optionally snap its beat phase. */
    bool syncDeck (int slave, int master, bool alignPhase = true);
    /** Continuous tempo lock of `slave` to `master`; pass master = -1 to release. */
    bool setSyncLock (int slave, int master);
    /** Tempo-syncs `slave`, moves it to its nearest beat and starts it exactly on `master`'s next beat. */
    bool playInSync (int slave, int master);
    /** The deck `slave` is tempo-locked to, or -1. Any thread. */
    int getSyncLockMaster (int slave) const noexcept { return publishedSyncMaster[slave].load (std::memory_order_relaxed); }

    int64_t getSampleClock() const noexcept   { return sampleClock.load (std::memory_order_acquire); }
    double  getSampleRate() const noexcept    { return currentSampleRate.load(); }

    //--- manual / offline driving ---------------------------------------------
    void prepareToPlay (double sampleRate, int expectedBlockSize = config::maxInternalBlock);
    /** Renders `numSamples` frames into `outputs` (any block size). Real-time safe. */
    void processBlock (float* const* outputs, int numOutputChannels, int numSamples) noexcept;

    /** Releases retired tracks. Called periodically by the housekeeping thread; exposed for offline use. */
    void collectGarbage();

    /** When enabled, a background thread drives the engine in real time (output discarded)
        whenever no audio device is running, so loading, transport and the UI keep working
        without an output device. It never renders at the same time as a device callback. */
    void setSilentClockEnabled (bool shouldRun);
    bool isRunningOnSilentClock() const noexcept   { return silentClockEnabled.load() && ! deviceActive.load(); }

    /** Incremented on every device callback; lets a watchdog detect a stalled device. */
    uint32_t getCallbackCount() const noexcept     { return callbackCounter.load (std::memory_order_relaxed); }
    /** Set when the driver reported an error (e.g. device unplugged); cleared on restart. */
    bool hasDeviceError() const noexcept           { return deviceErrorFlag.load(); }

private:
    enum class EngineCommandType : uint8_t { Sync, SyncLock, PlayInSync };
    struct EngineCommand { EngineCommandType type; int slave = 0, master = 0; bool flag = false; };

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& message) override;

    void renderBlock (float* const* outputs, int numOutputChannels, int numSamples,
                      const int* physicalToPackedMap, int numPhysical) noexcept;
    bool pushEngineCommand (const EngineCommand&);
    void applyEngineCommand (const EngineCommand&) noexcept;
    void syncAudio (int slave, int master, bool alignPhase) noexcept;
    void updateSyncLocks() noexcept;
    static double matchTempoRatio (double ratio) noexcept;

    class Housekeeper;
    class SilentClock;
    class LoadJob;

    juce::AudioDeviceManager deviceManager;
    std::array<std::unique_ptr<Deck>, config::numDecks> decks;
    Mixer mixer;

    // engine command FIFO
    juce::SpinLock engineProducerLock;
    SpscQueue<EngineCommand, 64> engineCommands;

    // audio-thread state
    int syncMaster[config::numDecks];
    std::atomic<int> publishedSyncMaster[config::numDecks];
    int64_t scheduledPlay[config::numDecks];
    juce::AudioBuffer<float> deckBuffers;
    float* chunkOutputs[maxOutputChannels] {};
    int physicalToPacked[maxOutputChannels] {};
    int numPhysicalOutputs = 0;
    std::atomic<uint32_t> callbackCounter { 0 };
    std::atomic<bool> deviceErrorFlag { false };
    juce::AudioProcessLoadMeasurer loadMeasurer;

    std::atomic<int64_t> sampleClock { 0 };
    std::atomic<double>  currentSampleRate { 48000.0 };

    // loading
    juce::ThreadPool loaderPool { juce::ThreadPoolOptions{}.withThreadName ("HYPE loader").withNumberOfThreads (2)
                                                         .withDesiredThreadPriority (juce::Thread::Priority::low) };
    juce::SpinLock loadLock;
    std::shared_ptr<std::atomic<bool>> activeLoadCancel[config::numDecks];

    std::unique_ptr<Housekeeper> housekeeper;
    std::unique_ptr<SilentClock> silentClock;
    std::atomic<bool> silentClockEnabled { false }, deviceActive { false }, silentRendering { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
} // namespace hype
