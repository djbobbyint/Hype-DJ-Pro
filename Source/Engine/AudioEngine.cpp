#include "AudioEngine.h"
#include "BeatAnalyzer.h"

#include <cmath>
#include <limits>
#include <thread>

namespace hype
{
//==============================================================================
class AudioEngine::Housekeeper : public juce::Thread
{
public:
    explicit Housekeeper (AudioEngine& e) : juce::Thread ("HYPE housekeeping"), engine (e) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            engine.collectGarbage();
            wait (50);
        }
    }

private:
    AudioEngine& engine;
};

//==============================================================================
/** Drives the engine in real time while no device is running (see setSilentClockEnabled). */
class AudioEngine::SilentClock : public juce::Thread
{
public:
    explicit SilentClock (AudioEngine& e) : juce::Thread ("HYPE silent clock"), engine (e) {}

    void run() override
    {
        juce::AudioBuffer<float> scratch (2, config::maxInternalBlock);
        double nextDeadline = juce::Time::getMillisecondCounterHiRes();

        while (! threadShouldExit())
        {
            if (! engine.silentClockEnabled.load() || engine.deviceActive.load())
            {
                wait (20);
                nextDeadline = juce::Time::getMillisecondCounterHiRes();
                continue;
            }

            const double sr = engine.currentSampleRate.load();
            const int n = juce::jmin (config::maxInternalBlock, (int) (sr * 0.01));   // ~10 ms blocks

            // Dekker-style handshake with audioDeviceAboutToStart(): announce, then re-check.
            engine.silentRendering.store (true);
            if (! engine.deviceActive.load())
                engine.renderBlock (scratch.getArrayOfWritePointers(), 2, n, nullptr, 2);
            engine.silentRendering.store (false);

            nextDeadline += 1000.0 * n / sr;
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (nextDeadline < now - 100.0)
                nextDeadline = now;   // fell behind (e.g. system sleep): don't try to catch up
            else if (nextDeadline > now)
                wait ((int) std::ceil (nextDeadline - now));
        }
    }

private:
    AudioEngine& engine;
};

//==============================================================================
class AudioEngine::LoadJob : public juce::ThreadPoolJob
{
public:
    LoadJob (AudioEngine& e, int d, juce::File f, std::shared_ptr<std::atomic<bool>> cancel, LoadCallback cb)
        : juce::ThreadPoolJob ("HYPE load " + f.getFileName()),
          engine (e), deck (d), file (std::move (f)), cancelFlag (std::move (cancel)), callback (std::move (cb)) {}

    JobStatus runJob() override
    {
        AudioFileDecoder decoder;
        auto result = AudioEngine::decodeAndAnalyse (decoder, file, cancelFlag.get());

        if (cancelFlag->load())
        {
            result.track = nullptr;
            result.error = "Cancelled (superseded by a newer load)";
        }
        else if (result.wasOk() && ! engine.getDeck (deck).loadTrack (result.track))
        {
            result.track = nullptr;
            result.error = "Deck command queue full";
        }

        if (callback)
            callback (deck, result);

        return jobHasFinished;
    }

private:
    AudioEngine& engine;
    const int deck;
    const juce::File file;
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    LoadCallback callback;
};

//==============================================================================
AudioEngine::AudioEngine()
{
    for (int d = 0; d < config::numDecks; ++d)
    {
        decks[(size_t) d] = std::make_unique<Deck> (d);
        syncMaster[d] = -1;
        publishedSyncMaster[d].store (-1);
        scheduledPlay[d] = -1;
    }

    // Default routing: deck A on the left of the crossfader, deck B on the right.
    mixer.setCrossfaderAssign (0, Mixer::CrossfaderAssign::A);
    mixer.setCrossfaderAssign (1, Mixer::CrossfaderAssign::B);

    prepareToPlay (48000.0);

    housekeeper = std::make_unique<Housekeeper> (*this);
    housekeeper->startThread (juce::Thread::Priority::low);

    silentClock = std::make_unique<SilentClock> (*this);
}

AudioEngine::~AudioEngine()
{
    closeDevice();

    {
        const juce::SpinLock::ScopedLockType sl (loadLock);
        for (auto& c : activeLoadCancel)
            if (c != nullptr)
                c->store (true);
    }
    loaderPool.removeAllJobs (true, 10000);

    silentClock->stopThread (2000);
    housekeeper->stopThread (2000);
    collectGarbage();
}

//==============================================================================
// Device
//==============================================================================

juce::String AudioEngine::openDevice (const DeviceSettings& settings)
{
    closeDevice();

    if (settings.deviceType.isNotEmpty())
        deviceManager.setCurrentAudioDeviceType (settings.deviceType, true);

    const int outs = juce::jlimit (2, maxOutputChannels, settings.numOutputChannels);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = settings.outputDevice;
    setup.sampleRate = settings.sampleRate;
    setup.bufferSize = settings.bufferSize;
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.setRange (0, outs, true);
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();

    auto error = deviceManager.initialise (0, outs, nullptr, settings.outputDevice.isEmpty(),
                                           settings.outputDevice, &setup);

    if (error.isEmpty() && deviceManager.getCurrentAudioDevice() == nullptr)
        error = "No audio output device available";

    if (error.isNotEmpty())
        return error;

    deviceManager.addAudioCallback (this);
    return {};
}

void AudioEngine::attachToDeviceManager()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.addAudioCallback (this);
}

void AudioEngine::closeDevice()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

AudioEngine::DeviceStatus AudioEngine::getDeviceStatus() const
{
    DeviceStatus s;
    s.cpuLoad = loadMeasurer.getLoadAsProportion();

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        s.open = device->isPlaying();
        s.typeName = device->getTypeName();
        s.deviceName = device->getName();
        s.sampleRate = device->getCurrentSampleRate();
        s.bufferSize = device->getCurrentBufferSizeSamples();
        s.numOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();
        if (s.sampleRate > 0)
            s.outputLatencyMs = 1000.0 * (device->getOutputLatencyInSamples() + s.bufferSize) / s.sampleRate;
        s.xruns = device->getXRunCount();
    }
    return s;
}

void AudioEngine::setSilentClockEnabled (bool shouldRun)
{
    silentClockEnabled.store (shouldRun);
    if (shouldRun && ! silentClock->isThreadRunning())
        silentClock->startThread (juce::Thread::Priority::high);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    // Take over from the silent clock: claim the engine, then wait for any render in flight.
    deviceActive.store (true);
    while (silentRendering.load())
        std::this_thread::yield();

    // JUCE passes only the *active* output channels, packed in order. Build the map from
    // physical channel number (what the routing uses) to packed index.
    const auto active = device->getActiveOutputChannels();
    int packed = 0;
    for (int phys = 0; phys < maxOutputChannels; ++phys)
        physicalToPacked[phys] = active[phys] ? packed++ : -1;
    numPhysicalOutputs = juce::jmin (maxOutputChannels, active.getHighestBit() + 1);

    prepareToPlay (device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
    deviceErrorFlag.store (false);
}

void AudioEngine::audioDeviceStopped()
{
    deviceActive.store (false);   // the silent clock (if enabled) takes over
}

void AudioEngine::audioDeviceError (const juce::String& message)
{
    // Called by some drivers when the device disappears; the AudioDeviceController's
    // watchdog sees this flag and recovers on the message thread.
    deviceErrorFlag.store (true);
    juce::ignoreUnused (message);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const*, int,
                                                    float* const* outputs, int numOutputs, int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    callbackCounter.fetch_add (1, std::memory_order_relaxed);
    renderBlock (outputs, numOutputs, numSamples, physicalToPacked, numPhysicalOutputs);
}

//==============================================================================
// Processing
//==============================================================================

void AudioEngine::prepareToPlay (double sampleRate, int expectedBlockSize)
{
    currentSampleRate.store (sampleRate);

    for (auto& d : decks)
        d->prepare (sampleRate);

    mixer.prepare (sampleRate);
    deckBuffers.setSize (config::numDecks * 2, config::maxInternalBlock);
    loadMeasurer.reset (sampleRate, juce::jmax (1, expectedBlockSize));
}

void AudioEngine::processBlock (float* const* outputs, int numOutputChannels, int numSamples) noexcept
{
    // Offline / manual use: outputs are indexed by physical channel directly.
    renderBlock (outputs, numOutputChannels, numSamples, nullptr, juce::jmin (numOutputChannels, maxOutputChannels));
}

void AudioEngine::renderBlock (float* const* outputs, int numOutputChannels, int numSamples,
                               const int* physToPacked, int numPhysical) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    juce::AudioProcessLoadMeasurer::ScopedTimer timer (loadMeasurer, numSamples);

    // Silence everything once up front; the mixer overwrites the channels it routes to.
    for (int c = 0; c < numOutputChannels; ++c)
        if (outputs[c] != nullptr)
            juce::FloatVectorOperations::clear (outputs[c], numSamples);

    int64_t now = sampleClock.load (std::memory_order_relaxed);

    // 1) Commands from the control threads. Deck commands first, so engine-level actions
    //    (sync, quantised start) see the deck state the user has just set up.
    for (auto& d : decks)
        d->pullCommands (now);

    EngineCommand ec;
    while (engineCommands.pop (ec))
        applyEngineCommand (ec);

    // 2) Render in chunks, splitting exactly at scheduled command times.
    const float* deckL[config::numDecks];
    const float* deckR[config::numDecks];

    int offset = 0;
    while (offset < numSamples)
    {
        for (int d = 0; d < config::numDecks; ++d)
        {
            decks[(size_t) d]->applyDueCommands (now);
            if (scheduledPlay[d] >= 0 && scheduledPlay[d] <= now)
            {
                decks[(size_t) d]->playAudio();
                scheduledPlay[d] = -1;
            }
        }

        int chunk = juce::jmin (numSamples - offset, config::maxInternalBlock);

        int64_t next = std::numeric_limits<int64_t>::max();
        for (int d = 0; d < config::numDecks; ++d)
        {
            next = juce::jmin (next, decks[(size_t) d]->getNextCommandTime());
            if (scheduledPlay[d] >= 0)
                next = juce::jmin (next, scheduledPlay[d]);
        }
        if (next > now && next < now + chunk)
            chunk = (int) (next - now);

        updateSyncLocks();

        for (int d = 0; d < config::numDecks; ++d)
        {
            float* l = deckBuffers.getWritePointer (d * 2);
            float* r = deckBuffers.getWritePointer (d * 2 + 1);
            decks[(size_t) d]->render (l, r, chunk);
            deckL[d] = l;
            deckR[d] = r;
        }

        for (int phys = 0; phys < numPhysical; ++phys)
        {
            const int packed = physToPacked != nullptr ? physToPacked[phys] : phys;
            chunkOutputs[phys] = (packed >= 0 && packed < numOutputChannels && outputs[packed] != nullptr)
                                     ? outputs[packed] + offset : nullptr;
        }

        mixer.process (deckL, deckR, chunkOutputs, numPhysical, chunk);

        offset += chunk;
        now += chunk;
        sampleClock.store (now, std::memory_order_release);
    }
}

void AudioEngine::collectGarbage()
{
    for (auto& d : decks)
        d->collectGarbage();
}

//==============================================================================
// Loading
//==============================================================================

AudioFileDecoder::Result AudioEngine::decodeAndAnalyse (AudioFileDecoder& decoder, const juce::File& file,
                                                        const std::atomic<bool>* cancelFlag)
{
    auto result = decoder.decode (file, cancelFlag);

    if (result.wasOk() && result.track->getInfo().bpm <= 0.0)
    {
        // The track isn't shared with any deck yet; wrap its channels without copying.
        float* channels[2] = { const_cast<float*> (result.track->getChannel (0)),
                               const_cast<float*> (result.track->getChannel (1)) };
        const juce::AudioBuffer<float> view (channels, 2, (int) result.track->getLength());
        const auto beats = analyseBeats (view, result.track->getSampleRate());
        if (beats.bpm > 0.0)
            result.track->setBeatGrid (beats.bpm, beats.firstBeatSeconds);
    }

    if (result.wasOk() && result.track->getWaveform() == nullptr)
        result.track->setWaveform (WaveformData::build (result.track->getChannel (0), result.track->getChannel (1),
                                                        result.track->getLength(), result.track->getSampleRate()));
    return result;
}

void AudioEngine::loadTrackAsync (int deck, const juce::File& file, LoadCallback onFinished)
{
    jassert (deck >= 0 && deck < config::numDecks);

    auto cancel = std::make_shared<std::atomic<bool>> (false);
    {
        const juce::SpinLock::ScopedLockType sl (loadLock);
        if (activeLoadCancel[deck] != nullptr)
            activeLoadCancel[deck]->store (true);
        activeLoadCancel[deck] = cancel;
    }

    loaderPool.addJob (new LoadJob (*this, deck, file, std::move (cancel), std::move (onFinished)), true);
}

AudioFileDecoder::Result AudioEngine::loadTrackSync (int deck, const juce::File& file)
{
    AudioFileDecoder decoder;
    auto result = decodeAndAnalyse (decoder, file);

    if (result.wasOk() && ! getDeck (deck).loadTrack (result.track))
        return { nullptr, "Deck command queue full" };

    return result;
}

juce::String AudioEngine::getSupportedFormatsWildcard() const
{
    return AudioFileDecoder().getWildcardForAllFormats();
}

//==============================================================================
// Sync
//==============================================================================

bool AudioEngine::pushEngineCommand (const EngineCommand& c)
{
    const juce::SpinLock::ScopedLockType sl (engineProducerLock);
    return engineCommands.push (c);
}

bool AudioEngine::syncDeck (int slave, int master, bool alignPhase)
{
    return slave != master && pushEngineCommand ({ EngineCommandType::Sync, slave, master, alignPhase });
}

bool AudioEngine::setSyncLock (int slave, int master)
{
    return slave != master && pushEngineCommand ({ EngineCommandType::SyncLock, slave, master, false });
}

bool AudioEngine::playInSync (int slave, int master)
{
    return slave != master && pushEngineCommand ({ EngineCommandType::PlayInSync, slave, master, false });
}

double AudioEngine::matchTempoRatio (double ratio) noexcept
{
    // Half/double-time matching: pick the tempo multiple closest to the slave's own tempo.
    double best = ratio;
    for (double candidate : { ratio * 0.5, ratio * 2.0 })
        if (std::abs (std::log2 (candidate)) < std::abs (std::log2 (best)))
            best = candidate;
    return best;
}

void AudioEngine::syncAudio (int slaveIndex, int masterIndex, bool alignPhase) noexcept
{
    auto& slave = *decks[(size_t) slaveIndex];
    auto& master = *decks[(size_t) masterIndex];

    if (! slave.hasBeatGridAudio() || ! master.hasBeatGridAudio())
        return;

    const double speed = matchTempoRatio (master.getTargetBpmAudio (false) / slave.getOriginalBpmAudio());
    slave.applyTempoFromSyncAudio (speed);

    if (alignPhase)
    {
        double delta = master.getBeatPhaseAudio() - slave.getBeatPhaseAudio();
        delta -= std::floor (delta + 0.5);   // wrap to [-0.5, 0.5): move by the smallest amount

        if (std::abs (delta) > 1.0e-6)
            slave.seekAudio (slave.getPositionAudio() + delta * slave.getBeatLengthSamplesAudio());
    }
}

void AudioEngine::updateSyncLocks() noexcept
{
    for (int d = 0; d < config::numDecks; ++d)
    {
        const int m = syncMaster[d];
        auto& slave = *decks[(size_t) d];

        if (m >= 0 && slave.hasBeatGridAudio() && decks[(size_t) m]->hasBeatGridAudio())
            slave.setSyncSpeedAudio (matchTempoRatio (decks[(size_t) m]->getTargetBpmAudio (true) / slave.getOriginalBpmAudio()));
        else
            slave.setSyncSpeedAudio (0.0);
    }
}

void AudioEngine::applyEngineCommand (const EngineCommand& c) noexcept
{
    if (c.slave < 0 || c.slave >= config::numDecks || c.master >= config::numDecks)
        return;

    switch (c.type)
    {
        case EngineCommandType::Sync:
            if (c.master >= 0)
                syncAudio (c.slave, c.master, c.flag);
            break;

        case EngineCommandType::SyncLock:
            // Releasing (or re-targeting) a lock keeps the tempo currently playing, so the
            // mix doesn't lurch back to whatever the pitch fader was before.
            if (const int oldMaster = syncMaster[c.slave]; oldMaster >= 0)
            {
                auto& slave = *decks[(size_t) c.slave];
                auto& master = *decks[(size_t) oldMaster];
                // Master's fader tempo, without any momentary pitch bend.
                if (slave.hasBeatGridAudio() && master.hasBeatGridAudio())
                    slave.applyTempoFromSyncAudio (matchTempoRatio (master.getTargetBpmAudio (false) / slave.getOriginalBpmAudio()));
                slave.setSyncSpeedAudio (0.0);
            }

            // Prevent mutual locks (A->B and B->A).
            if (c.master >= 0 && syncMaster[c.master] == c.slave)
                syncMaster[c.master] = -1;
            syncMaster[c.slave] = c.master;
            if (c.master >= 0)
                syncAudio (c.slave, c.master, false);
            for (int d = 0; d < config::numDecks; ++d)
                publishedSyncMaster[d].store (syncMaster[d], std::memory_order_relaxed);
            break;

        case EngineCommandType::PlayInSync:
        {
            if (c.master < 0)
                break;

            auto& slave = *decks[(size_t) c.slave];
            auto& master = *decks[(size_t) c.master];

            if (! slave.hasBeatGridAudio() || ! master.hasBeatGridAudio() || ! master.isPlayingAudio())
            {
                slave.playAudio();
                break;
            }

            syncAudio (c.slave, c.master, false);

            // Put the slave on its nearest beat (it is paused, so this is silent)...
            const double slaveBeat = slave.getBeatLengthSamplesAudio();
            double phase = slave.getBeatPhaseAudio();
            if (phase >= 0.5) phase -= 1.0;
            slave.seekAudio (slave.getPositionAudio() - phase * slaveBeat);

            // ...and start it exactly when the master crosses its next beat.
            const double masterSpeed = master.getTargetSpeedAudio (true);
            const double masterSourceRate = master.getBeatLengthSamplesAudio() * master.getOriginalBpmAudio() / 60.0;
            const double sourcePerDevice = masterSpeed * masterSourceRate / currentSampleRate.load();
            if (sourcePerDevice <= 0.0)
            {
                slave.playAudio();
                break;
            }

            const double toNextBeat = (1.0 - master.getBeatPhaseAudio()) * master.getBeatLengthSamplesAudio();
            scheduledPlay[c.slave] = sampleClock.load (std::memory_order_relaxed)
                                   + (int64_t) std::llround (toNextBeat / sourcePerDevice);
            break;
        }
    }
}
} // namespace hype
