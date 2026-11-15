#include "Deck.h"

#include <cmath>
#include <limits>

namespace hype
{
namespace
{
    constexpr double keyShiftEpsilon = 1.0e-4;

    /** How close (in seconds) the playhead must be to the cue point for CUE to start a preview. */
    constexpr double cueTolerance = 0.002;

    /** Keep rendering this long past the last sample so stretcher latency drains naturally. */
    constexpr double endMarginSeconds = 0.25;

    void zero (float* l, float* r, int n) noexcept
    {
        juce::FloatVectorOperations::clear (l, n);
        juce::FloatVectorOperations::clear (r, n);
    }
}

Deck::Deck (int deckIndex)
    : index (deckIndex), stretcher (createDefaultTimeStretcher())
{
    prepare (48000.0);
}

Deck::~Deck()
{
    // Drain anything still in flight; no audio callback can be running by now.
    Command c;
    while (commandQueue.pop (c))
        if (c.track != nullptr)
            c.track->decReferenceCount();

    for (int i = 0; i < numScheduled; ++i)
        if (scheduled[i].cmd.track != nullptr)
            scheduled[i].cmd.track->decReferenceCount();

    if (track != nullptr)
        track->decReferenceCount();

    collectGarbage();
}

//==============================================================================
// Control side
//==============================================================================

bool Deck::push (const Command& c)
{
    const juce::SpinLock::ScopedLockType sl (producerLock);
    return commandQueue.push (c);
}

bool Deck::loadTrack (LoadedTrack::Ptr newTrack)
{
    if (newTrack == nullptr)
        return eject();

    const int gen = nextGeneration.fetch_add (1);

    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        pendingInfo = newTrack->getInfo();
        pendingInfoGeneration = gen;
    }

    Command c { CommandType::Load };
    c.track = newTrack.get();
    c.track->incReferenceCount();   // reference travels with the command
    c.generation = gen;

    if (! push (c))
    {
        c.track->decReferenceCount();
        return false;
    }
    return true;
}

bool Deck::eject()
{
    Command c { CommandType::Load };
    c.generation = nextGeneration.fetch_add (1);
    return push (c);
}

bool Deck::play (int64_t at)            { Command c { CommandType::Play };  c.atSample = at; return push (c); }
bool Deck::pause (int64_t at)           { Command c { CommandType::Pause }; c.atSample = at; return push (c); }
bool Deck::togglePlayPause()            { return push ({ CommandType::TogglePlay }); }
bool Deck::stop()                       { return push ({ CommandType::Stop }); }
bool Deck::cueDown()                    { return push ({ CommandType::CueDown }); }
bool Deck::cueUp()                      { return push ({ CommandType::CueUp }); }
bool Deck::setCuePoint()                { return push ({ CommandType::SetCue }); }
bool Deck::returnToCue()                { return push ({ CommandType::ReturnToCue }); }

bool Deck::setCuePointSeconds (double s)
{
    Command c { CommandType::SetCueAt };
    c.value = s;
    return push (c);
}

bool Deck::seekToSeconds (double s, int64_t at)
{
    Command c { CommandType::SeekAbsoluteSeconds };
    c.value = s;
    c.atSample = at;
    return push (c);
}

bool Deck::seekToNormalised (double p)
{
    Command c { CommandType::SeekNormalised };
    c.value = juce::jlimit (0.0, 1.0, p);
    return push (c);
}

bool Deck::seekRelativeSeconds (double d)
{
    Command c { CommandType::SeekRelativeSeconds };
    c.value = d;
    return push (c);
}

bool Deck::setBeatGrid (double newBpm, double firstBeatSeconds)
{
    Command c { CommandType::SetBeatGrid };
    c.value = juce::jmax (0.0, newBpm);
    c.value2 = firstBeatSeconds;
    return push (c);
}

void Deck::setPitchRange (double fraction)
{
    fraction = juce::jlimit (0.01, 1.0, fraction);
    pitchRange.store (fraction);
    baseSpeed.store (juce::jlimit (config::minSpeed, config::maxSpeed, 1.0 + pitchFader.load() * fraction));
}

void Deck::setPitchFader (double position)
{
    position = juce::jlimit (-1.0, 1.0, position);
    pitchFader.store (position);
    baseSpeed.store (juce::jlimit (config::minSpeed, config::maxSpeed, 1.0 + position * pitchRange.load()));
}

void Deck::setTempoRatio (double ratio)
{
    ratio = juce::jlimit (config::minSpeed, config::maxSpeed, ratio);
    baseSpeed.store (ratio);
    pitchFader.store (juce::jlimit (-1.0, 1.0, (ratio - 1.0) / pitchRange.load()));
}

void Deck::setPitchBend (double offset)          { pitchBend.store (juce::jlimit (-1.0, 1.0, offset)); }
void Deck::setKeyLock (bool enabled)             { keyLock.store (enabled); }
void Deck::setKeyShiftSemitones (double s)       { keyShift.store (juce::jlimit (-config::maxPitchSemitones, config::maxPitchSemitones, s)); }
void Deck::setTrimDb (float db)                  { trimDb.store (juce::jlimit (config::minTrimDb, config::maxTrimDb, db)); }

Deck::State Deck::getState() const
{
    State s;
    s.loaded         = pubLoaded.load();
    s.playing        = pubPlaying.load();
    s.cuePreviewing  = pubPreviewing.load();
    s.reachedEnd     = pubEnded.load();
    s.keyLock        = keyLock.load();
    s.keyLockPathActive = pubKeyLockPath.load();
    s.trackGeneration = pubGeneration.load();
    s.keyShiftSemitones = keyShift.load();

    if (s.loaded)
    {
        const juce::SpinLock::ScopedLockType sl (infoLock);
        if (pendingInfoGeneration == s.trackGeneration)
        {
            s.title  = pendingInfo.title;
            s.artist = pendingInfo.artist;
        }
    }

    const double rate = pubRate.load();
    const auto length = pubLength.load();
    const double p = pubPos.load();

    s.sampleRate = rate;
    if (rate > 0.0 && length > 0)
    {
        s.durationSeconds  = (double) length / rate;
        s.elapsedSeconds   = juce::jlimit (0.0, s.durationSeconds, p / rate);
        s.remainingSeconds = s.durationSeconds - s.elapsedSeconds;
        s.position         = s.elapsedSeconds / s.durationSeconds;
        s.cueSeconds       = pubCue.load() / rate;
    }

    s.tempoRatio     = pubTempo.load();
    s.effectiveSpeed = pubSpeed.load();
    s.pitchPercent   = (s.effectiveSpeed - 1.0) * 100.0;
    s.originalBpm    = pubBpm.load();
    s.currentBpm     = s.originalBpm * s.effectiveSpeed;

    if (s.originalBpm > 0.0 && rate > 0.0)
    {
        const double beats = (p / rate - pubFirstBeat.load()) * s.originalBpm / 60.0;
        s.beatPhase = beats - std::floor (beats);
    }

    s.peakLeft  = pubPeakL.exchange (0.0f);
    s.peakRight = pubPeakR.exchange (0.0f);
    return s;
}

void Deck::collectGarbage()
{
    LoadedTrack* t = nullptr;
    while (retiredTracks.pop (t))
        t->decReferenceCount();
}

//==============================================================================
// Audio side
//==============================================================================

void Deck::prepare (double sampleRate)
{
    deviceRate = sampleRate;
    fadeLength = juce::jlimit (32, config::maxInternalBlock, (int) std::round (sampleRate * config::declickSeconds));

    increments.allocate ((size_t) config::maxInternalBlock, true);
    tail.setSize (2, fadeLength);
    tailScratch.setSize (2, fadeLength);
    tail.clear();
    tailRemaining = tailReadIndex = 0;

    fadeInLinear.allocate ((size_t) fadeLength, false);
    fadeOutLinear.allocate ((size_t) fadeLength, false);
    fadeInPower.allocate ((size_t) fadeLength, false);
    fadeOutPower.allocate ((size_t) fadeLength, false);
    for (int i = 0; i < fadeLength; ++i)
    {
        const double x = (i + 0.5) / fadeLength;
        fadeInLinear[i]  = (float) x;
        fadeOutLinear[i] = (float) (1.0 - x);
        fadeInPower[i]   = (float) std::sin (x * juce::MathConstants<double>::halfPi);
        fadeOutPower[i]  = (float) std::cos (x * juce::MathConstants<double>::halfPi);
    }

    stretcher->prepare (sampleRate, 2);
    stretcherPrimed = false;

    speedSmoothed.reset (sampleRate, config::speedSmoothingSeconds);
    speedSmoothed.setCurrentAndTargetValue (computeTargetSpeed (true));
    trimSmoothed.reset (sampleRate, config::gainSmoothingSeconds);
    trimSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (trimDb.load()));
}

void Deck::pullCommands (int64_t now)
{
    Command c;
    while (commandQueue.pop (c))
    {
        const int64_t t = c.atSample < 0 ? now : c.atSample;

        if (t <= now || numScheduled >= config::maxScheduledCommands)
        {
            // Flush earlier scheduled commands first so ordering is preserved.
            applyDueCommands (now);
            apply (c);
            continue;
        }

        // Insert sorted by time (stable for equal times).
        int i = numScheduled;
        while (i > 0 && scheduled[i - 1].time > t)
        {
            scheduled[i] = scheduled[i - 1];
            --i;
        }
        scheduled[i] = { t, c };
        ++numScheduled;
    }
}

void Deck::applyDueCommands (int64_t now)
{
    int consumed = 0;
    while (consumed < numScheduled && scheduled[consumed].time <= now)
        apply (scheduled[consumed++].cmd);

    if (consumed > 0)
    {
        for (int i = consumed; i < numScheduled; ++i)
            scheduled[i - consumed] = scheduled[i];
        numScheduled -= consumed;
    }
}

int64_t Deck::getNextCommandTime() const noexcept
{
    return numScheduled > 0 ? scheduled[0].time : std::numeric_limits<int64_t>::max();
}

double Deck::clampPosition (double p) const noexcept
{
    if (track == nullptr)
        return 0.0;
    return juce::jlimit (0.0, (double) track->getLength(), p);
}

void Deck::retire (LoadedTrack* t)
{
    if (t == nullptr)
        return;

    // Never drop the last reference on the audio thread. The queue is drained regularly by the
    // engine's housekeeping thread; if it were ever full we leak rather than free here.
    if (! retiredTracks.push (t))
        jassertfalse;
}

void Deck::apply (const Command& c)
{
    const double trackRate = track != nullptr ? track->getSampleRate() : 1.0;

    switch (c.type)
    {
        case CommandType::Load:
        {
            if (playing || tailRemaining > 0)
                captureTail (FadeShape::EqualPower);

            retire (track);
            track = c.track;  // takes over the in-flight reference
            playing = previewing = reachedEnd = false;
            stretcherPrimed = false;
            loadedGeneration = c.generation;
            bpm = 0.0;
            firstBeat = 0.0;
            pos = cuePos = 0.0;

            if (track != nullptr)
            {
                bpm = track->getInfo().bpm;
                firstBeat = track->getInfo().firstBeatSeconds;
                // Auto-cue to the first beat, like most DJ software does.
                if (bpm > 0.0)
                    pos = cuePos = clampPosition (firstBeat * track->getSampleRate());
            }
            break;
        }

        case CommandType::Play:
            playAudio();
            break;

        case CommandType::Pause:
            if (playing)
            {
                const double heard = pos;
                captureTail (FadeShape::EqualPower);
                pos = heard;
                playing = previewing = false;
                stretcherPrimed = false;
            }
            break;

        case CommandType::TogglePlay:
        {
            Command p { playing && ! previewing ? CommandType::Pause : CommandType::Play };
            apply (p);
            break;
        }

        case CommandType::Stop:
            if (playing)
                captureTail (FadeShape::EqualPower);
            playing = previewing = false;
            stretcherPrimed = false;
            pos = cuePos;
            break;

        case CommandType::CueDown:
            if (track == nullptr)
                break;

            if (playing && ! previewing)
            {
                captureTail (FadeShape::EqualPower);
                playing = false;
                stretcherPrimed = false;
                pos = cuePos;
            }
            else if (! playing)
            {
                if (std::abs (pos - cuePos) <= cueTolerance * trackRate)
                {
                    captureTail (FadeShape::EqualPower);
                    pos = cuePos;
                    playing = previewing = true;
                    reachedEnd = false;
                    stretcherPrimed = false;
                }
                else
                {
                    cuePos = pos;
                }
            }
            break;

        case CommandType::CueUp:
            if (previewing)
            {
                captureTail (FadeShape::EqualPower);
                playing = previewing = false;
                stretcherPrimed = false;
                pos = cuePos;
            }
            break;

        case CommandType::SetCue:
            if (track != nullptr)
                cuePos = clampPosition (pos);
            break;

        case CommandType::SetCueAt:
            if (track != nullptr)
                cuePos = clampPosition (c.value * trackRate);
            break;

        case CommandType::ReturnToCue:
            if (track != nullptr)
                seekAudio (cuePos);
            break;

        case CommandType::SeekAbsoluteSeconds:
            if (track != nullptr)
                seekAudio (c.value * trackRate);
            break;

        case CommandType::SeekNormalised:
            if (track != nullptr)
                seekAudio (c.value * (double) track->getLength());
            break;

        case CommandType::SeekRelativeSeconds:
            if (track != nullptr)
                seekAudio (pos + c.value * trackRate);
            break;

        case CommandType::SetBeatGrid:
            bpm = c.value;
            firstBeat = c.value2;
            break;
    }
}

void Deck::playAudio() noexcept
{
    if (track == nullptr)
        return;

    if (playing)
    {
        previewing = false;  // PLAY during a cue preview latches playback
        return;
    }

    if (pos >= (double) track->getLength())
        pos = cuePos;

    captureTail (FadeShape::EqualPower);
    playing = true;
    previewing = false;
    reachedEnd = false;
    stretcherPrimed = false;
}

void Deck::seekAudio (double target) noexcept
{
    if (track == nullptr)
        return;

    target = clampPosition (target);

    if (playing)
        captureTail (FadeShape::EqualPower);

    pos = target;
    reachedEnd = false;
    stretcherPrimed = false;
}

double Deck::computeTargetSpeed (bool includeBend) const noexcept
{
    double s = syncSpeed > 0.0 ? syncSpeed : baseSpeed.load (std::memory_order_relaxed);
    if (includeBend)
        s += pitchBend.load (std::memory_order_relaxed);
    return juce::jlimit (config::minSpeed, config::maxSpeed, s);
}

double Deck::getTargetSpeedAudio (bool includeBend) const noexcept { return computeTargetSpeed (includeBend); }
bool   Deck::hasBeatGridAudio() const noexcept                     { return track != nullptr && bpm > 0.0; }

double Deck::getTargetBpmAudio (bool includeBend) const noexcept
{
    return bpm * computeTargetSpeed (includeBend);
}

double Deck::getBeatLengthSamplesAudio() const noexcept
{
    return (track != nullptr && bpm > 0.0) ? track->getSampleRate() * 60.0 / bpm : 0.0;
}

double Deck::getBeatPhaseAudio() const noexcept
{
    if (! hasBeatGridAudio())
        return 0.0;
    const double beats = (pos / track->getSampleRate() - firstBeat) * bpm / 60.0;
    return beats - std::floor (beats);
}

void Deck::setSyncSpeedAudio (double s) noexcept
{
    syncSpeed = s > 0.0 ? juce::jlimit (config::minSpeed, config::maxSpeed, s) : 0.0;
}

void Deck::applyTempoFromSyncAudio (double s) noexcept
{
    s = juce::jlimit (config::minSpeed, config::maxSpeed, s);
    baseSpeed.store (s);
    pitchFader.store (juce::jlimit (-1.0, 1.0, (s - 1.0) / pitchRange.load()));
    snapSpeed = true;   // jump straight to the new tempo so the phase alignment holds exactly
}

StereoSource Deck::sourceView() const noexcept
{
    StereoSource src;
    if (track != nullptr)
    {
        src.channels[0] = track->getChannel (0);
        src.channels[1] = track->getChannel (1);
        src.length = track->getLength();
    }
    return src;
}

//==============================================================================

void Deck::render (float* left, float* right, int numSamples)
{
    jassert (numSamples <= config::maxInternalBlock);

    // Path selection (vinyl <-> key lock). Both paths start from the same playhead, so a
    // short correlated (linear) crossfade makes the switch inaudible.
    const bool wantKeyLock = keyLock.load (std::memory_order_relaxed)
                          || std::abs (keyShift.load (std::memory_order_relaxed)) > keyShiftEpsilon;
    const Path wanted = wantKeyLock ? Path::KeyLock : Path::Vinyl;

    if (wanted != path)
    {
        if (playing)
        {
            const double heard = pos;
            captureTail (FadeShape::Linear);
            pos = heard;
        }
        path = wanted;
        stretcherPrimed = false;
    }

    renderInternal (left, right, numSamples);

    // End of track: stop once the playhead (and any stretcher latency) has run out.
    if (playing && track != nullptr
        && pos >= (double) track->getLength() + endMarginSeconds * track->getSampleRate())
    {
        playing = previewing = false;
        stretcherPrimed = false;
        reachedEnd = true;
        pos = (double) track->getLength();
    }

    const float pl = juce::FloatVectorOperations::findMaximum (left, numSamples);
    const float nl = -juce::FloatVectorOperations::findMinimum (left, numSamples);
    const float pr = juce::FloatVectorOperations::findMaximum (right, numSamples);
    const float nr = -juce::FloatVectorOperations::findMinimum (right, numSamples);
    const float peakL = juce::jmax (pl, nl), peakR = juce::jmax (pr, nr);
    if (peakL > pubPeakL.load (std::memory_order_relaxed)) pubPeakL.store (peakL, std::memory_order_relaxed);
    if (peakR > pubPeakR.load (std::memory_order_relaxed)) pubPeakR.store (peakR, std::memory_order_relaxed);

    publishState();
}

void Deck::publishState() noexcept
{
    pubPos.store (pos, std::memory_order_relaxed);
    pubCue.store (cuePos, std::memory_order_relaxed);
    pubSpeed.store (speedSmoothed.getCurrentValue(), std::memory_order_relaxed);
    pubTempo.store (computeTargetSpeed (false), std::memory_order_relaxed);
    pubBpm.store (bpm, std::memory_order_relaxed);
    pubFirstBeat.store (firstBeat, std::memory_order_relaxed);
    pubRate.store (track != nullptr ? track->getSampleRate() : 0.0, std::memory_order_relaxed);
    pubLength.store (track != nullptr ? track->getLength() : 0, std::memory_order_relaxed);
    pubPlaying.store (playing, std::memory_order_relaxed);
    pubPreviewing.store (previewing, std::memory_order_relaxed);
    pubEnded.store (reachedEnd, std::memory_order_relaxed);
    pubLoaded.store (track != nullptr, std::memory_order_relaxed);
    pubKeyLockPath.store (path == Path::KeyLock, std::memory_order_relaxed);
    pubGeneration.store (loadedGeneration, std::memory_order_release);
}

void Deck::renderInternal (float* left, float* right, int numSamples)
{
    // Speed smoothing runs even while paused so a tempo change made while stopped is
    // already in place on the next play.
    speedSmoothed.setTargetValue (computeTargetSpeed (true));
    if (snapSpeed)
    {
        speedSmoothed.setCurrentAndTargetValue (computeTargetSpeed (true));
        snapSpeed = false;
    }

    if (playing && track != nullptr)
        renderPlaying (left, right, numSamples);
    else
    {
        speedSmoothed.skip (numSamples);
        zero (left, right, numSamples);
    }

    // Trim gain.
    trimSmoothed.setTargetValue (juce::Decibels::decibelsToGain (trimDb.load (std::memory_order_relaxed), -100.0f));
    if (trimSmoothed.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = trimSmoothed.getNextValue();
            left[i] *= g;
            right[i] *= g;
        }
    }
    else
    {
        const float g = trimSmoothed.getTargetValue();
        if (std::abs (g - 1.0f) > 1.0e-7f)
        {
            juce::FloatVectorOperations::multiply (left, g, numSamples);
            juce::FloatVectorOperations::multiply (right, g, numSamples);
        }
    }

    // De-click crossfade: fade the new signal in under the (already faded-out) tail.
    if (tailRemaining > 0)
    {
        const float* fadeIn = tailShape == FadeShape::Linear ? fadeInLinear.get() : fadeInPower.get();
        const float* tl = tail.getReadPointer (0);
        const float* tr = tail.getReadPointer (1);
        const int n = juce::jmin (numSamples, tailRemaining);
        for (int i = 0; i < n; ++i)
        {
            const int k = tailReadIndex + i;
            left[i]  = left[i]  * fadeIn[k] + tl[k];
            right[i] = right[i] * fadeIn[k] + tr[k];
        }
        tailReadIndex += n;
        tailRemaining -= n;
    }
}

void Deck::captureTail (FadeShape shape)
{
    // Render what *would* have been heard next (including any fade already in progress),
    // fade it out, and keep it to overlap the new signal. This advances the playback state,
    // so callers restore the playhead afterwards where appropriate.
    float* sl = tailScratch.getWritePointer (0);
    float* sr = tailScratch.getWritePointer (1);
    renderInternal (sl, sr, fadeLength);

    const float* fadeOut = shape == FadeShape::Linear ? fadeOutLinear.get() : fadeOutPower.get();
    juce::FloatVectorOperations::multiply (sl, fadeOut, fadeLength);
    juce::FloatVectorOperations::multiply (sr, fadeOut, fadeLength);

    std::swap (tail, tailScratch);   // swaps internal storage only - no allocation
    tailRemaining = fadeLength;
    tailReadIndex = 0;
    tailShape = shape;
}

void Deck::renderPlaying (float* left, float* right, int numSamples)
{
    if (path == Path::Vinyl)
        renderVinyl (left, right, numSamples);
    else
        renderKeyLock (left, right, numSamples);
}

void Deck::renderVinyl (float* left, float* right, int numSamples)
{
    const double srRatio = track->getSampleRate() / deviceRate;
    float maxInc = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float inc = (float) (speedSmoothed.getNextValue() * srRatio);
        increments[i] = inc;
        maxInc = juce::jmax (maxInc, std::abs (inc));
    }

    interpolator.process (track->getChannel (0), track->getChannel (1), track->getLength(),
                          pos, increments.get(), left, right, numSamples,
                          interpolator.chooseKernel (maxInc));
}

void Deck::primeStretcher()
{
    const double srRatio = track->getSampleRate() / deviceRate;
    const double rate = juce::jmax (config::minStretchRate, speedSmoothed.getCurrentValue()) * srRatio;
    const int length = stretcher->getPrimeLength (rate);
    const auto start = (int64_t) std::llround (pos);

    const double keyRatio = std::pow (2.0, keyShift.load() / 12.0);
    stretcher->setTransposeFactor (keyLock.load() ? keyRatio * srRatio : rate * keyRatio);
    stretcher->prime (sourceView(), start, length);

    pos = (double) start;
    feedPos = start + length;
    stretcherPrimed = true;
}

void Deck::renderKeyLock (float* left, float* right, int numSamples)
{
    if (! stretcherPrimed)
        primeStretcher();

    const double srRatio = track->getSampleRate() / deviceRate;
    const double keyRatio = std::pow (2.0, keyShift.load (std::memory_order_relaxed) / 12.0);
    const bool lockKey = keyLock.load (std::memory_order_relaxed);
    const auto source = sourceView();
    const int inLatency = stretcher->getInputLatency();
    const int outLatency = stretcher->getOutputLatency();

    for (int offset = 0; offset < numSamples; offset += config::stretchChunk)
    {
        const int n = juce::jmin (config::stretchChunk, numSamples - offset);

        double speedSum = 0.0;
        for (int i = 0; i < n; ++i)
            speedSum += speedSmoothed.getNextValue();

        const double speed = juce::jmax (config::minStretchRate, speedSum / n);
        const double rate = speed * srRatio;          // source samples per output sample

        // Output frequency = input frequency * transpose, measured at the device rate.
        // Key lock: original key (only the source/device rate difference is compensated).
        // Otherwise: pitch follows tempo, plus the key shift.
        stretcher->setTransposeFactor (lockKey ? keyRatio * srRatio : rate * keyRatio);

        pos += rate * n;

        // Keep the input "lead" equal to the stretcher's latency at this rate, so the
        // playhead we report is the one being heard.
        const auto targetFeed = (int64_t) std::llround (pos + inLatency + rate * outLatency);
        const int inputCount = (int) juce::jmax<int64_t> (0, targetFeed - feedPos);

        stretcher->process (source, feedPos, inputCount, left + offset, right + offset, n);
        feedPos += inputCount;
    }
}
} // namespace hype
