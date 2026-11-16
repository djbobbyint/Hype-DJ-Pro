# HYPE DJ PRO — Engine Architecture

## Components

| Component | Files | Responsibility | Threads |
|---|---|---|---|
| **AudioEngine** | `Engine/AudioEngine.*` | Owns the processing callback and composes everything else. Runs the render loop: commands → decks → mixer → cue → router. Handles sample-accurate scheduling, sync (one-shot, lock, quantised start) and the silent clock. Implements `EngineInterface`. | audio callback; silent clock; housekeeping |
| **DeckEngine** ×2 | `Engine/DeckEngine.*` | Playback of one track: transport, CDJ cue, seek, tempo/pitch, pitch bend, key lock (vinyl resampler ↔ time-stretcher), de-click crossfades, beat grid, display state. Implements `DeckControl`. | control: any; render: audio |
| **MixerEngine** | `Engine/MixerEngine.*`, `ThreeBandEq.*` | Channel gain → 3-band isolator EQ → channel fader → crossfader (adjustable curve, reverse) → master volume → limiter; meters. Exposes pre-fader channel taps and the master bus. Implements `MixerControl`. | control: any; process: audio |
| **CueEngine** | `Engine/CueEngine.*` | Headphone monitoring: cue bus from the mixer's pre-fader taps, cue/master blend, level, split cue; meters. Only *reads* mixer buffers, so it can't affect the master. Implements `CueControl`. | control: any; process: audio |
| **OutputRouter** | `Engine/OutputRouter.*` | Places master, headphone and direct outputs on physical device channels. | config: any; route: audio |
| **AudioDeviceManager** | `Engine/AudioDeviceManager.*` | Hardware and output configuration: drivers, devices, rate, buffer, channels, master/headphone assignment (→ OutputRouter), watchdog recovery and fallback, persistence. Owns `juce::AudioDeviceManager`. Implements `DeviceControl`. | message thread |
| **TrackLoader** | `Engine/TrackLoader.*`, `AudioFileDecoder.*`, `FFmpegDecoder.*`, `BeatAnalyzer.*`, `WaveformData.*` | Decoding (JUCE / FFmpeg), BPM and beat grid, waveform; cancels superseded loads; hands finished tracks to the AudioEngine. | low-priority loader pool |

Supporting DSP: `SincInterpolator` (vinyl mode), `TimeStretcher` (key-lock interface + Signalsmith implementation),
`SpscQueue` (wait-free FIFO), `LoadedTrack` (immutable, ref-counted decoded audio).

## Signal flow (audio thread, per ≤512-frame chunk)

```
DeckEngine A ─┐            ┌─ channel taps (pre-fader) ─┬─► CueEngine ─► headphone bus ─┐
DeckEngine B ─┴► MixerEngine ┤                            └──────────── direct outs ─────┤
                             └─ master bus (volume, limiter) ─────────────────────────────┴► OutputRouter ─► device channels
```

## The UI ↔ engine boundary

The UI includes exactly one engine header, `Source/Api/HypeEngineApi.h`. The build enforces this:
`cmake/CheckApiBoundary.cmake` runs before every app build and fails if a UI file includes anything else from the
engine. The API contains:

- `EngineInterface` (created by `createEngine()`), which hands out `DeckControl&` ×2, `MixerControl&`,
  `CueControl&` and `DeviceControl&`, plus `loadTrack`, sync and the sample clock.
- Value types only: `DeckState`, `MixerMeters`, `CueMeters`, `LoadResult`, `DeviceSettings`, `DeviceStatus`, and
  `WaveformData` (immutable, shared read-only). No audio buffers, no engine internals, no audio-thread entry points.

### Threading contract
| Interface | Callable from | Mechanism |
|---|---|---|
| `DeckControl`, `MixerControl`, `CueControl` | any thread except the audio thread | parameters: atomics, smoothed per sample on the audio thread; actions: wait-free SPSC FIFOs (producers serialised by a spin lock the audio thread never takes), applied at the next block or an exact scheduled sample |
| getters / `getState()` / meters | any thread | atomics published by the audio thread |
| `DeviceControl` | message thread | opens/closes hardware |
| `LoadCallback` | invoked on a loader thread | the UI marshals to the message thread |

### Real-time guarantees (tested)
- No allocation on the audio thread: `malloc` is intercepted during processing while every feature is exercised.
- No locks or waits in the callback; tracks are swapped by pointer and freed on the housekeeping thread.
- Clean under ThreadSanitizer, AddressSanitizer and UBSan.

## Ownership and lifetime
`AudioEngine` constructs, in order: OutputRouter → MixerEngine → CueEngine → DeckEngines → TrackLoader →
housekeeping/silent-clock threads → AudioDeviceManager. Destruction runs in reverse: the device is closed first, so
no callback can reach a component that's being destroyed; loader jobs finish before the decks they deliver to go
away.
