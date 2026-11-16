#pragma once

/**
    HYPE DJ PRO - public engine API.

    This header is the ONLY engine header the user interface (Source/App) may include; the build
    checks it. Everything the UI can do to the audio engine is declared here, as abstract
    interfaces with an explicit threading contract:

        EngineInterface        top level: decks, mixer, cue, devices, loading, sync, clock
          DeckControl  x 2     transport, cue, tempo/pitch, key lock, beat grid, display state
          MixerControl         channel gain, 3-band EQ, channel volume, crossfader, master
          CueControl           headphone monitoring
          DeviceControl        audio hardware and output configuration

    Threading contract
    ------------------
    * DeckControl, MixerControl, CueControl: every method may be called from any thread except
      the audio thread. Calls never block the audio thread: parameters are lock-free atomics
      (smoothed on the audio thread), actions travel through wait-free FIFOs and take effect
      at the start of the next audio block (or at an exact engine sample when scheduled).
      Getters return values/snapshots, never references to engine-internal state.
    * DeviceControl: message thread only (it opens/closes hardware).
    * Load callbacks run on a background loader thread; marshal to the UI thread yourself.
    * Nothing in this API exposes audio buffers, the audio thread, or engine internals.
*/

#include "Engine/WaveformData.h"

#include <juce_core/juce_core.h>
#include <cstdint>
#include <functional>
#include <memory>

namespace hype
{
constexpr int numDecks = 2;

/** Parameter ranges the UI needs (the engine clamps to these). */
namespace ranges
{
    constexpr float channelGainMinDb = -24.0f, channelGainMaxDb = 12.0f;
    constexpr float eqMinDb = -26.0f;          // at or below: band killed
    constexpr float eqMaxDb = 6.0f;
    constexpr float masterVolumeMax = 2.0f, cueLevelMax = 2.0f;
}

//==============================================================================
// Value types
//==============================================================================

/** Snapshot of one deck, published by the audio thread. */
struct DeckState
{
    bool   loaded = false;
    bool   playing = false;
    bool   cuePreviewing = false;
    bool   reachedEnd = false;
    bool   keyLock = false;
    bool   keyLockPathActive = false;   // true when the time-stretcher is in use

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

    float  peakLeft = 0.0f, peakRight = 0.0f;  // deck output peaks since the previous getState()
    int    trackGeneration = 0;                 // changes whenever a different track is loaded
};

enum class EqBand { low = 0, mid = 1, high = 2 };
enum class CrossfaderAssign { A, Thru, B };
enum class CrossfaderCurve { Smooth, Linear, Cut };     // presets for the continuous curve

struct MixerMeters
{
    float channelPeak[numDecks] {};      // post-gain/EQ, pre-fader (gain staging)
    float masterPeakL = 0, masterPeakR = 0;
    float masterRmsL = 0, masterRmsR = 0;
    bool  masterClipped = false;         // limiter engaged (or > 0 dBFS with the limiter off)
    float limiterReductionDb = 0;
};

struct CueMeters
{
    float peakL = 0, peakR = 0;
};

struct LoadResult
{
    bool ok = false;
    juce::String error;
    juce::String title, artist, decoderName;
    double bpm = 0.0, durationSeconds = 0.0, sampleRate = 0.0;
};

/** Called on a background thread when a load finishes (successfully or not). */
using LoadCallback = std::function<void (int deck, const LoadResult&)>;

struct DeviceSettings
{
    juce::String typeName;              // "ASIO", "Windows Audio", "CoreAudio", "ALSA", ...
    juce::String deviceName;            // empty = default device of the type
    double sampleRate = 0.0;            // 0 = device default
    int bufferSize = 0;                 // 0 = device default
    juce::BigInteger outputChannels;    // empty = derived from the master/headphone pairs
    int masterFirstChannel = 0;         // stereo pair starting here (0-based), -1 = off
    int cueFirstChannel = 2;            // stereo pair starting here (0-based), -1 = off
    bool fallbackToDefaultDevice = true;
};

enum class DeviceState { noDevice, running, recovering, fallback };

struct DeviceStatus
{
    DeviceState state = DeviceState::noDevice;
    juce::String typeName, deviceName, message;
    double sampleRate = 0.0;
    int bufferSize = 0;
    int numOutputChannels = 0;
    int numActiveOutputs = 0;
    double latencyMs = 0.0;
    double cpuLoad = 0.0;
    int xruns = -1;
    int masterFirstChannel = -1, cueFirstChannel = -1;
    bool engineRunningWithoutOutput = false;
};

//==============================================================================
// Control interfaces
//==============================================================================

/** One deck. Any non-audio thread. Action methods return false only if the command FIFO is full.
    `atEngineSample` < 0 means "as soon as possible", otherwise the exact EngineInterface::getSampleClock() time. */
class DeckControl
{
public:
    virtual ~DeckControl() = default;

    // Transport
    virtual bool play (int64_t atEngineSample = -1) = 0;
    virtual bool pause (int64_t atEngineSample = -1) = 0;
    virtual bool togglePlayPause() = 0;
    virtual bool stop() = 0;                          // pause and return to the cue point
    virtual bool eject() = 0;

    // CDJ-style cue: playing -> back to cue + pause; paused on cue -> preview while held;
    // paused elsewhere -> set cue. PLAY during a preview latches playback.
    virtual bool cueDown() = 0;
    virtual bool cueUp() = 0;
    virtual bool setCuePoint() = 0;                   // at the exact current playhead
    virtual bool setCuePointSeconds (double seconds) = 0;
    virtual bool returnToCue() = 0;                   // keeps play/pause state

    // Seeking
    virtual bool seekToSeconds (double seconds, int64_t atEngineSample = -1) = 0;
    virtual bool seekToNormalised (double proportion0to1) = 0;
    virtual bool seekRelativeSeconds (double deltaSeconds) = 0;

    // Tempo & pitch
    virtual void setPitchRange (double fraction) = 0;        // 0.08, 0.16, 0.5, 1.0
    virtual void setPitchFader (double position) = 0;        // -1..1, tempo = 1 + position * range
    virtual void setTempoRatio (double ratio) = 0;           // 1.0 = original tempo
    virtual void setPitchBend (double ratioOffset) = 0;      // momentary nudge
    virtual void setKeyLock (bool enabled) = 0;
    virtual void setKeyShiftSemitones (double semitones) = 0;
    virtual bool setBeatGrid (double bpm, double firstBeatSeconds) = 0;

    virtual double getPitchRange() const = 0;
    virtual double getPitchFader() const = 0;
    virtual double getTempoRatio() const = 0;
    virtual bool   isKeyLockOn() const = 0;

    // Display
    virtual DeckState getState() const = 0;
    /** Waveform of the track currently playing (null while empty or until a new load is live). */
    virtual std::shared_ptr<const WaveformData> getWaveform() const = 0;
};

/** Channel strips, crossfader and master section. Any non-audio thread. */
class MixerControl
{
public:
    virtual ~MixerControl() = default;

    virtual void setChannelGainDb (int deck, float decibels) = 0;           // -24..+12 dB
    virtual void setEqGainDb (int deck, EqBand band, float decibels) = 0;   // <= -26 dB = kill, max +6
    virtual void setEqKill (int deck, EqBand band, bool killed) = 0;
    virtual void setChannelVolume (int deck, float fader0to1) = 0;
    virtual void setCrossfaderAssign (int deck, CrossfaderAssign) = 0;

    virtual void setCrossfader (float position0to1) = 0;                    // 0 = A, 1 = B
    virtual void setCrossfaderCurveShape (float shape0to1) = 0;             // 0 smooth .. 1 scratch cut
    virtual void setCrossfaderCurve (CrossfaderCurve preset) = 0;
    virtual void setCrossfaderReverse (bool reversed) = 0;
    virtual void setMasterVolume (float linearGain) = 0;                    // 0..2
    virtual void setLimiterEnabled (bool enabled) = 0;

    virtual float getChannelGainDb (int deck) const = 0;
    virtual float getEqGainDb (int deck, EqBand) const = 0;
    virtual bool  isEqKilled (int deck, EqBand) const = 0;
    virtual float getChannelVolume (int deck) const = 0;
    virtual CrossfaderAssign getCrossfaderAssign (int deck) const = 0;
    virtual float getCrossfader() const = 0;
    virtual float getCrossfaderCurveShape() const = 0;
    virtual bool  isCrossfaderReversed() const = 0;
    virtual float getMasterVolume() const = 0;

    /** Peak/RMS since the previous call (intended for a single UI consumer). */
    virtual MixerMeters getAndResetMeters() = 0;
};

/** Headphone monitoring. Any non-audio thread. Never affects the master output. */
class CueControl
{
public:
    virtual ~CueControl() = default;

    virtual void setCueEnabled (int deck, bool enabled) = 0;   // both decks may be cued at once
    virtual void setCueMix (float cueToMaster0to1) = 0;        // 0 = cue only, 1 = master only
    virtual void setCueLevel (float linearGain) = 0;           // headphone volume 0..2
    virtual void setSplitCue (bool enabled) = 0;               // cue left ear, master right ear

    virtual bool  isCueEnabled (int deck) const = 0;
    virtual float getCueMix() const = 0;
    virtual float getCueLevel() const = 0;
    virtual bool  isSplitCue() const = 0;

    virtual CueMeters getAndResetMeters() = 0;
};

/** Audio hardware and output configuration. Message thread only. */
class DeviceControl
{
public:
    virtual ~DeviceControl() = default;

    virtual juce::StringArray getDeviceTypes() const = 0;
    virtual juce::StringArray getDeviceNames (const juce::String& typeName) const = 0;
    virtual void refreshDevices() = 0;

    virtual juce::StringArray getOutputChannelNames() const = 0;
    virtual juce::BigInteger getActiveOutputChannels() const = 0;
    virtual juce::Array<double> getAvailableSampleRates() const = 0;
    virtual juce::Array<int> getAvailableBufferSizes() const = 0;

    /** Opens/reconfigures the device. Returns an empty string on success. */
    virtual juce::String apply (const DeviceSettings&) = 0;
    /** Master / headphone stereo pairs (first channel, -1 = off). */
    virtual juce::String setOutputAssignment (int masterFirstChannel, int cueFirstChannel) = 0;
    virtual void setFallbackToDefaultDevice (bool) = 0;
    virtual void closeDevice() = 0;

    virtual DeviceSettings getSettings() const = 0;
    virtual DeviceStatus getStatus() const = 0;

    virtual std::unique_ptr<juce::XmlElement> createStateXml() const = 0;
    virtual juce::String restoreFromXml (const juce::XmlElement*) = 0;

    /** Called on the message thread whenever the device or its state changes. */
    virtual void setStatusListener (std::function<void()> listener) = 0;
};

/** The engine as seen by the UI. */
class EngineInterface
{
public:
    virtual ~EngineInterface() = default;

    virtual DeckControl&   deck (int index) = 0;
    virtual MixerControl&  mixer() = 0;
    virtual CueControl&    cue() = 0;
    virtual DeviceControl& devices() = 0;

    /** Decodes and analyses on a background thread, then loads into the deck. A newer request
        for the same deck cancels an older one. */
    virtual void loadTrack (int deck, const juce::File& file, LoadCallback onFinished = {}) = 0;
    virtual juce::String getSupportedFormatsWildcard() const = 0;

    // Synchronisation (executed sample-exactly on the audio thread)
    virtual bool syncDeck (int slave, int master, bool alignPhase = true) = 0;
    virtual bool setSyncLock (int slave, int master) = 0;    // master -1 releases
    virtual int  getSyncLockMaster (int slave) const = 0;
    virtual bool playInSync (int slave, int master) = 0;

    virtual int64_t getSampleClock() const = 0;
    virtual double  getSampleRate() const = 0;

    /** Keep the engine running in real time when no output device is open (for the UI). */
    virtual void setRunWithoutOutput (bool shouldRun) = 0;
};

/** Creates the real engine. */
std::unique_ptr<EngineInterface> createEngine();
} // namespace hype
