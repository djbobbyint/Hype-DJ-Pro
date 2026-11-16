#pragma once

#include "Api/HypeEngineApi.h"
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
    DeckEngine: one DJ deck - a player for a single in-memory track. Implements the public
    DeckControl interface; the audio-thread side is private and driven only by AudioEngine.

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
class DeckEngine : public DeckControl
{
public:
    explicit DeckEngine (int deckIndex);
    ~DeckEngine() override;

    int getIndex() const noexcept { return index; }

    //==========================================================================
    // Transport (any non-audio thread). Return false only if the command FIFO is full.
    // `atEngineSample` < 0 means "as soon as possible"; otherwise the action happens
    // sample-accurately at that AudioEngine::getSampleClock() time.

    /** Swaps in a decoded track. The deck stops, playhead and cue go to 0 (or the first beat). */
    bool loadTrack (LoadedTrack::Ptr track);
    bool eject() override;

    bool play  (int64_t atEngineSample = -1) override;
    bool pause (int64_t atEngineSample = -1) override;
    bool togglePlayPause() override;

    /** Stops playback and returns the playhead to the cue point. */
    bool stop() override;

    /** CDJ-style CUE button.
        - playing:                    jump back to the cue point and pause
        - paused on the cue point:    preview from the cue point while held
        - paused elsewhere:           set the cue point here
        Pressing PLAY during a preview keeps playing after CUE is released. */
    bool cueDown() override;
    bool cueUp() override;

    /** Sets the cue point at the exact current playhead. */
    bool setCuePoint() override;
    bool setCuePointSeconds (double seconds) override;

    /** Jumps to the cue point, keeping the current play/pause state. */
    bool returnToCue() override;

    bool seekToSeconds (double seconds, int64_t atEngineSample = -1) override;
    bool seekToNormalised (double proportion0to1) override;
    bool seekRelativeSeconds (double deltaSeconds) override;

    //==========================================================================
    // Tempo & pitch (any thread, lock-free)

    /** Pitch fader range as a fraction, e.g. 0.08 = +/-8 %, 0.16, 0.50, 1.0 (= +/-100 %). */
    void setPitchRange (double fraction) override;
    /** Pitch fader position in [-1, 1]; tempo = 1 + position * range. */
    void setPitchFader (double position) override;
    /** Sets tempo directly as a ratio (1.0 = original). Updates the fader position to match. */
    void setTempoRatio (double ratio) override;
    /** Momentary pitch bend, added to the tempo ratio (e.g. +/-0.04 while a bend button / jog is held). */
    void setPitchBend (double ratioOffset) override;

    /** Key lock: keep the original key regardless of tempo. */
    void setKeyLock (bool enabled) override;
    /** Key shift in semitones, independent of tempo (forces the key-lock path while non-zero). */
    void setKeyShiftSemitones (double semitones) override;

    /** Overrides the beat grid (e.g. from the user tapping, or an external analyser). */
    bool setBeatGrid (double bpm, double firstBeatSeconds) override;

    double getPitchRange() const override  { return pitchRange.load(); }
    double getPitchFader() const override  { return pitchFader.load(); }
    double getTempoRatio() const override  { return baseSpeed.load(); }
    bool   isKeyLockOn() const override    { return keyLock.load(); }


    DeckState getState() const override;
    std::shared_ptr<const WaveformData> getWaveform() const override;

    /** The track currently loaded on the audio thread (engine/tests; the UI uses getWaveform()).
        Returns nullptr while empty, or while a newer load hasn't reached the audio thread yet. */
    LoadedTrack::Ptr getDisplayTrack() const;

    /** Releases tracks retired by the audio thread (housekeeping thread). */
    void collectGarbage();

private:
    friend class AudioEngine;

    //==========================================================================
    // Audio thread only (AudioEngine)

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
    double feedSrcPos = 0.0;      // next source sample to feed the stretcher (source-rate domain)

    SincInterpolator interpolator;
    std::unique_ptr<TimeStretcher> stretcher;

    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear> speedSmoothed;

    juce::HeapBlock<float> increments;

    // Key-lock input resampled to the device rate (when track and device rates differ), so the
    // stretcher only handles the musical tempo/key change, never a sample-rate conversion.
    juce::AudioBuffer<float> resampledInput;
    juce::HeapBlock<float> resampleIncrements;
    int resampleCapacity = 0;
    StereoSource makeResampledInput (double& sourcePosition, int count, double srRatio) noexcept;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeckEngine)
};
} // namespace hype
