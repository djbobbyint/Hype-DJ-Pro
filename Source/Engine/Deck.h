#pragma once

#include "EngineConfig.h"
#include "LoadedTrack.h"
#include "SincInterpolator.h"
#include "SpscQueue.h"
#include "TimeStretcher.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>

namespace hype
{
/**
    One DJ deck: a player for a single in-memory track.

    Threading model
    ---------------
    * Control methods (play, cueDown, setPitchFader, ...) may be called from any non-audio
      thread (UI, MIDI, loader). They never block the audio thread:
        - continuous parameters are plain atomics, smoothed per sample on the audio thread;
        - discrete transport actions are queued through a wait-free FIFO and applied at the
          start of the next audio chunk (or at an exact engine sample if scheduled).
    * getState() may be called from any thread; it reads atomics published by the audio thread.
    * Everything in the "audio thread" section is called only by AudioEngine.

    Playback paths
    --------------
    * Vinyl:   key lock off and no key shift. Band-limited sinc resampling; pitch follows tempo.
    * KeyLock: key lock on, or a key shift is applied. TimeStretcher keeps pitch independent
               of tempo. Switching between paths is a short correlated crossfade.

    Every discontinuity (play, pause, stop, cue, seek, load, path switch) is de-clicked by
    crossfading the old output (rendered a few ms ahead into a tail buffer) with the new one.
*/
class Deck
{
public:
    explicit Deck (int deckIndex);
    ~Deck();

    int getIndex() const noexcept { return index; }

    //==========================================================================
    // Transport (any non-audio thread). Return false only if the command FIFO is full.
    // `atEngineSample` < 0 means "as soon as possible"; otherwise the action happens
    // sample-accurately at that AudioEngine::getSampleClock() time.

    /** Swaps in a decoded track. The deck stops, playhead and cue go to 0 (or the first beat). */
    bool loadTrack (LoadedTrack::Ptr track);
    bool eject();

    bool play  (int64_t atEngineSample = -1);
    bool pause (int64_t atEngineSample = -1);
    bool togglePlayPause();

    /** Stops playback and returns the playhead to the cue point. */
    bool stop();

    /** CDJ-style CUE button.
        - playing:                    jump back to the cue point and pause
        - paused on the cue point:    preview from the cue point while held
        - paused elsewhere:           set the cue point here
        Pressing PLAY during a preview keeps playing after CUE is released. */
    bool cueDown();
    bool cueUp();

    /** Sets the cue point at the exact current playhead. */
    bool setCuePoint();
    bool setCuePointSeconds (double seconds);

    /** Jumps to the cue point, keeping the current play/pause state. */
    bool returnToCue();

    bool seekToSeconds (double seconds, int64_t atEngineSample = -1);
    bool seekToNormalised (double proportion0to1);
    bool seekRelativeSeconds (double deltaSeconds);

    //==========================================================================
    // Tempo & pitch (any thread, lock-free)

    /** Pitch fader range as a fraction, e.g. 0.08 = +/-8 %, 0.16, 0.50, 1.0 (= +/-100 %). */
    void setPitchRange (double fraction);
    /** Pitch fader position in [-1, 1]; tempo = 1 + position * range. */
    void setPitchFader (double position);
    /** Sets tempo directly as a ratio (1.0 = original). Updates the fader position to match. */
    void setTempoRatio (double ratio);
    /** Momentary pitch bend, added to the tempo ratio (e.g. +/-0.04 while a bend button / jog is held). */
    void setPitchBend (double ratioOffset);

    /** Key lock: keep the original key regardless of tempo. */
    void setKeyLock (bool enabled);
    /** Key shift in semitones, independent of tempo (forces the key-lock path while non-zero). */
    void setKeyShiftSemitones (double semitones);

    /** Channel trim gain in dB (pre-fader). */
    void setTrimDb (float decibels);

    /** Overrides the beat grid (e.g. from the user tapping, or an external analyser). */
    bool setBeatGrid (double bpm, double firstBeatSeconds);

    double getPitchRange() const noexcept  { return pitchRange.load(); }
    double getPitchFader() const noexcept  { return pitchFader.load(); }
    double getTempoRatio() const noexcept  { return baseSpeed.load(); }
    bool   isKeyLockOn() const noexcept    { return keyLock.load(); }
    float  getTrimDb() const noexcept      { return trimDb.load(); }

    //==========================================================================
    struct State
    {
        bool   loaded = false;
        bool   playing = false;
        bool   cuePreviewing = false;
        bool   reachedEnd = false;
        bool   keyLock = false;
        bool   keyLockPathActive = false;   // true when the stretcher is in use

        juce::String title, artist;
        double sampleRate = 0.0;

        double durationSeconds  = 0.0;
        double elapsedSeconds   = 0.0;
        double remainingSeconds = 0.0;
        double position         = 0.0;      // 0..1 through the track
        double cueSeconds       = 0.0;

        double tempoRatio       = 1.0;      // pitch fader (or sync) tempo, without bend
        double effectiveSpeed   = 1.0;      // what is actually playing, including bend
        double pitchPercent     = 0.0;      // (effectiveSpeed - 1) * 100
        double keyShiftSemitones = 0.0;

        double originalBpm      = 0.0;      // 0 = unknown
        double currentBpm       = 0.0;      // originalBpm * effectiveSpeed
        double beatPhase        = 0.0;      // 0..1 position within the current beat
        double firstBeatSeconds = 0.0;      // beat grid anchor (assumed downbeat)

        float  peakLeft = 0.0f, peakRight = 0.0f;  // since last getState() call
        int    trackGeneration = 0;
    };

    State getState() const;

    /** The track currently loaded on the audio thread, for display (waveform, metadata).
        Returns nullptr while empty, or while a newer load hasn't reached the audio thread yet.
        Any non-audio thread. */
    LoadedTrack::Ptr getDisplayTrack() const;

    //==========================================================================
    // Audio thread only (called by AudioEngine)

    void prepare (double deviceSampleRate);
    void pullCommands (int64_t now);
    void applyDueCommands (int64_t now);
    int64_t getNextCommandTime() const noexcept;    // INT64_MAX if none pending
    void render (float* left, float* right, int numSamples);

    // Sync support (audio thread)
    bool   hasBeatGridAudio() const noexcept;
    double getBeatPhaseAudio() const noexcept;
    double getTargetBpmAudio (bool includeBend) const noexcept;
    double getOriginalBpmAudio() const noexcept    { return bpm; }
    double getBeatLengthSamplesAudio() const noexcept;
    bool   isPlayingAudio() const noexcept         { return playing; }
    double getTargetSpeedAudio (bool includeBend) const noexcept;
    void   setSyncSpeedAudio (double speedOrZero) noexcept;      // continuous sync lock
    void   applyTempoFromSyncAudio (double speed) noexcept;      // one-shot sync
    void   seekAudio (double sourcePosition) noexcept;           // sample-exact, de-clicked
    void   playAudio() noexcept;
    double getPositionAudio() const noexcept       { return pos; }

    /** Non-audio thread: releases tracks retired by the audio thread. */
    void collectGarbage();

private:
    enum class CommandType : uint8_t
    {
        Load, Play, Pause, TogglePlay, Stop, CueDown, CueUp, SetCue, SetCueAt,
        ReturnToCue, SeekAbsoluteSeconds, SeekNormalised, SeekRelativeSeconds, SetBeatGrid
    };

    struct Command
    {
        CommandType type;
        int64_t     atSample = -1;
        double      value = 0.0, value2 = 0.0;
        LoadedTrack* track = nullptr;   // owns one reference while in flight
        int         generation = 0;
    };

    enum class FadeShape { Linear, EqualPower };
    enum class Path { Vinyl, KeyLock };

    bool push (const Command&);
    void apply (const Command&);
    void renderInternal (float* left, float* right, int numSamples);
    void renderPlaying (float* left, float* right, int numSamples);
    void renderVinyl (float* left, float* right, int numSamples);
    void renderKeyLock (float* left, float* right, int numSamples);
    void captureTail (FadeShape shape);
    void primeStretcher();
    void retire (LoadedTrack*);
    double computeTargetSpeed (bool includeBend) const noexcept;
    double clampPosition (double p) const noexcept;
    StereoSource sourceView() const noexcept;
    void publishState() noexcept;

    const int index;

    //--- control-side --------------------------------------------------------
    juce::SpinLock producerLock;
    SpscQueue<Command, config::commandQueueSize> commandQueue;
    std::atomic<int> nextGeneration { 1 };

    std::atomic<double> pitchRange { 0.08 }, pitchFader { 0.0 }, baseSpeed { 1.0 },
                        pitchBend { 0.0 }, keyShift { 0.0 };
    std::atomic<bool>   keyLock { false };
    std::atomic<float>  trimDb { 0.0f };

    mutable juce::SpinLock infoLock;
    LoadedTrack::Ptr pendingTrack;  // most recently requested load (for display on non-audio threads)
    int pendingTrackGeneration = 0;

    //--- audio-side ----------------------------------------------------------
    double deviceRate = 48000.0;
    int fadeLength = 192;

    LoadedTrack* track = nullptr;
    double pos = 0.0, cuePos = 0.0;
    bool playing = false, previewing = false, reachedEnd = false;
    double bpm = 0.0, firstBeat = 0.0;
    double syncSpeed = 0.0;
    bool snapSpeed = false;
    int loadedGeneration = 0;

    Path path = Path::Vinyl;
    bool stretcherPrimed = false;
    int64_t feedPos = 0;

    SincInterpolator interpolator;
    std::unique_ptr<TimeStretcher> stretcher;

    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear> speedSmoothed;
    juce::SmoothedValue<float,  juce::ValueSmoothingTypes::Linear> trimSmoothed;

    juce::HeapBlock<float> increments;
    juce::AudioBuffer<float> tail, tailScratch;
    int tailRemaining = 0, tailReadIndex = 0;
    FadeShape tailShape = FadeShape::EqualPower;
    juce::HeapBlock<float> fadeInLinear, fadeOutLinear, fadeInPower, fadeOutPower;

    struct ScheduledCommand { int64_t time; Command cmd; };
    ScheduledCommand scheduled[config::maxScheduledCommands];
    int numScheduled = 0;

    SpscQueue<LoadedTrack*, 64> retiredTracks;

    //--- published (audio -> any thread) -------------------------------------
    std::atomic<double> pubPos { 0.0 }, pubCue { 0.0 }, pubSpeed { 1.0 }, pubTempo { 1.0 },
                        pubBpm { 0.0 }, pubFirstBeat { 0.0 }, pubRate { 0.0 };
    std::atomic<juce::int64> pubLength { 0 };
    std::atomic<bool>  pubPlaying { false }, pubPreviewing { false }, pubEnded { false },
                       pubLoaded { false }, pubKeyLockPath { false };
    std::atomic<int>   pubGeneration { 0 };
    mutable std::atomic<float> pubPeakL { 0.0f }, pubPeakR { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Deck)
};
} // namespace hype
