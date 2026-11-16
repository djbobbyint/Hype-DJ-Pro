#include "AudioEngine.h"

#include <cmath>
#include <limits>
#include <thread>

namespace hype
{
std::unique_ptr<EngineInterface> createEngine()
{
    return std::make_unique<AudioEngine>();
}

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
/** Drives the engine in real time while no device is running (see setRunWithoutOutput). */
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
AudioEngine::AudioEngine()
    : decks { std::make_unique<DeckEngine> (0), std::make_unique<DeckEngine> (1) },
      trackLoader ([this] (int d, LoadedTrack::Ptr t) { return decks[(size_t) d]->loadTrack (std::move (t)); })
{
    for (int d = 0; d < config::numDecks; ++d)
    {
        syncMaster[d] = -1;
        publishedSyncMaster[d].store (-1);
        scheduledPlay[d] = -1;
    }

    // Default routing: deck A on the left of the crossfader, deck B on the right.
    mixerEngine.setCrossfaderAssign (0, CrossfaderAssign::A);
    mixerEngine.setCrossfaderAssign (1, CrossfaderAssign::B);

    prepareToPlay (48000.0);

    housekeeper = std::make_unique<Housekeeper> (*this);
    housekeeper->startThread (juce::Thread::Priority::low);
    silentClock = std::make_unique<SilentClock> (*this);

    deviceManager = std::make_unique<AudioDeviceManager> (static_cast<AudioDeviceHost&> (*this), router);
}

AudioEngine::~AudioEngine()
{
    deviceManager.reset();          // closes the device: no more callbacks
    silentClock->stopThread (2000);
    housekeeper->stopThread (2000);
    collectGarbage();
}

void AudioEngine::loadTrack (int deck, const juce::File& file, LoadCallback onFinished)
{
    trackLoader.loadAsync (deck, file, std::move (onFinished));
}

void AudioEngine::setRunWithoutOutput (bool shouldRun)
{
    silentClockEnabled.store (shouldRun);
    if (shouldRun && ! silentClock->isThreadRunning())
        silentClock->startThread (juce::Thread::Priority::high);
}

void AudioEngine::collectGarbage()
{
    for (auto& d : decks)
        d->collectGarbage();
}

//==============================================================================
// Device callback
//==============================================================================

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    // Take over from the silent clock: claim the engine, then wait for any render in flight.
    deviceActive.store (true);
    while (silentRendering.load())
        std::this_thread::yield();

    // JUCE passes only the *active* output channels, packed in order: map physical -> packed.
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

void AudioEngine::audioDeviceError (const juce::String&)
{
    deviceErrorFlag.store (true);   // the AudioDeviceManager watchdog recovers on the message thread
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
    mixerEngine.prepare (sampleRate);
    cueEngine.prepare (sampleRate);
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

    for (int c = 0; c < numOutputChannels; ++c)
        if (outputs[c] != nullptr)
            juce::FloatVectorOperations::clear (outputs[c], numSamples);

    int64_t now = sampleClock.load (std::memory_order_relaxed);

    // 1) Commands from control threads. Deck commands first, so engine-level actions
    //    (sync, quantised start) see the deck state the user has just set up.
    for (auto& d : decks)
        d->pullCommands (now);

    EngineCommand ec;
    while (engineCommands.pop (ec))
        applyEngineCommand (ec);

    // 2) Render in chunks, splitting exactly at scheduled command times.
    const float* deckL[config::numDecks];
    const float* deckR[config::numDecks];
    const float* tapL[config::numDecks];
    const float* tapR[config::numDecks];

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

        // Decks
        for (int d = 0; d < config::numDecks; ++d)
        {
            float* l = deckBuffers.getWritePointer (d * 2);
            float* r = deckBuffers.getWritePointer (d * 2 + 1);
            decks[(size_t) d]->render (l, r, chunk);
            deckL[d] = l;
            deckR[d] = r;
        }

        // Mixer, then headphones from the mixer's pre-fader taps and master bus
        mixerEngine.process (deckL, deckR, chunk);
        for (int d = 0; d < config::numDecks; ++d)
        {
            tapL[d] = mixerEngine.getChannelOutput (d, 0);
            tapR[d] = mixerEngine.getChannelOutput (d, 1);
        }
        cueEngine.process (tapL, tapR, mixerEngine.getMasterOutput (0), mixerEngine.getMasterOutput (1), chunk);

        // Physical outputs
        for (int phys = 0; phys < numPhysical; ++phys)
        {
            const int packed = physToPacked != nullptr ? physToPacked[phys] : phys;
            chunkOutputs[phys] = (packed >= 0 && packed < numOutputChannels && outputs[packed] != nullptr)
                                     ? outputs[packed] + offset : nullptr;
        }
        router.route (chunkOutputs, numPhysical, chunk,
                      mixerEngine.getMasterOutput (0), mixerEngine.getMasterOutput (1),
                      cueEngine.getOutput (0), cueEngine.getOutput (1), cueEngine.isSplitActive(),
                      tapL, tapR);

        offset += chunk;
        now += chunk;
        sampleClock.store (now, std::memory_order_release);
    }
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
