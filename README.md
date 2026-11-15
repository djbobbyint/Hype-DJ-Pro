# HYPE DJ PRO — Core Audio Engine

Two-deck, low-latency DJ audio engine in C++17 / JUCE 8. This is the foundation layer:
playback, transport, tempo/pitch, key lock, mixing, routing and sync. No UI yet; it's driven
through a C++ API, a command-line host, or offline rendering.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/hype_engine_tests          # 150 checks, ~4 s
```

Dependencies are fetched by CMake: **JUCE 8.0.4** (or a local checkout in `external/JUCE`) and
**Signalsmith Stretch 1.4.0**. Linux also needs `libasound2-dev`; FFmpeg dev packages
(`libavformat-dev libavcodec-dev libswresample-dev libavutil-dev`) are optional (`-DHYPE_ENABLE_FFMPEG=OFF`).

## Formats

| Format | Linux | macOS | Windows |
|---|---|---|---|
| WAV / AIFF / FLAC / Ogg | JUCE | JUCE | JUCE |
| MP3 | JUCE | JUCE | JUCE |
| AAC / M4A / ALAC | FFmpeg (optional) | CoreAudio | Media Foundation |

Tracks are decoded completely into RAM (32-bit float, native sample rate) on a background thread, then
beat-analysed. That gives instant, sample-exact seeking and cueing. The cost is ~10 MB per stereo minute
and a load time equal to the decode time; tracks are capped at 60 minutes (`config::maxTrackSeconds`).

## Architecture

```
            UI / MIDI / CLI threads                       audio thread (device callback)
 ┌──────────────────────────────────────┐        ┌───────────────────────────────────────────┐
 │ Deck::play/cue/seek… ─► wait-free FIFO ├──────►│ apply commands (sample-accurate schedule) │
 │ Deck::setPitchFader… ─► atomics        ├──────►│ Deck A ─┐                                 │
 │ AudioEngine::syncDeck ─► FIFO          ├──────►│ Deck B ─┴► Mixer ─► master / cue / direct │
 │ getState() ◄── published atomics       │◄──────┤                                           │
 └──────────────────────────────────────┘        └───────────────────────────────────────────┘
   loader pool: decode + BPM analysis ──► LoadedTrack ──► deck (swap on audio thread)
   housekeeping thread ◄── retired tracks (memory never freed on the audio thread)
```

| File | Role |
|---|---|
| `Engine/AudioEngine` | Device I/O, chunked render loop, sample clock, async loading, sync |
| `Engine/Deck` | One player: transport, cue, tempo/pitch, key-lock path, de-click, state |
| `Engine/Mixer` | Faders, trim, crossfader curves, PFL cue, master limiter, output routing |
| `Engine/SincInterpolator` | 16-tap windowed-sinc variable-rate reader (vinyl mode) with anti-alias kernels |
| `Engine/TimeStretcher` | **Key-lock interface** + Signalsmith implementation |
| `Engine/BeatAnalyzer` | Offline BPM + first-beat detection (kick-weighted, beat-grid fit) |
| `Engine/AudioFileDecoder`, `FFmpegDecoder` | Decoder chain |
| `Engine/SpscQueue` | Wait-free single-producer/single-consumer ring buffer |

### Real-time guarantees
- The audio callback never allocates, locks or frees. A test enforces this by intercepting `malloc` on
  the audio thread while it exercises every feature (play, seek, key lock, sync, cue, track swap, eject).
- Any device buffer size works. Blocks are split into ≤512-frame chunks, and split again at the exact
  sample a scheduled command is due.
- Every discontinuity (play, pause, stop, cue, seek, load, vinyl↔key-lock switch) is crossfaded over 4 ms
  against a pre-rendered tail of the old signal. Speed, gain, fader and crossfader changes are smoothed per sample.
- Denormals are flushed; there's a −0.1 dBFS safety limiter on the master.

## Per-deck features

| Feature | API |
|---|---|
| Load / eject | `AudioEngine::loadTrackAsync(deck, file, cb)`, `Deck::loadTrack`, `eject` |
| Play / pause / toggle | `play(atSample=-1)`, `pause()`, `togglePlayPause()` |
| Stop | `stop()` pauses and returns to the cue point |
| CDJ cue | `cueDown()` / `cueUp()`: while playing → back to cue and pause; paused on cue → preview while held; paused elsewhere → set cue |
| Set / return to cue | `setCuePoint()`, `setCuePointSeconds(s)`, `returnToCue()` (keeps play state) |
| Seek | `seekToSeconds`, `seekToNormalised`, `seekRelativeSeconds` |
| Pitch fader | `setPitchRange(0.08/0.16/0.5/1.0)`, `setPitchFader(-1..1)`, `setTempoRatio(r)` |
| Pitch bend | `setPitchBend(±ratio)`, added on top of the fader (momentary nudge) |
| Key lock | `setKeyLock(bool)`, `setKeyShiftSemitones(s)` |
| Trim gain | `setTrimDb(dB)` |
| BPM / beat grid | auto-detected on load; `setBeatGrid(bpm, firstBeat)` to override |
| Display data | `getState()`: duration, elapsed, remaining, position 0..1, original and current BPM, pitch %, beat phase, cue, peaks |

Engine level: `syncDeck(slave, master, alignPhase)` (with half/double-time matching), `setSyncLock(slave, master)`,
`playInSync(slave, master)` (starts exactly on the master's next beat), `getSampleClock()` for scheduling.

**Key-lock architecture.** In vinyl mode (key lock off, no key shift) playback goes through the sinc resampler, so
pitch follows tempo. With key lock on, the deck switches to a `TimeStretcher`. The deck keeps the playhead aligned
with what is actually heard by accounting for the stretcher's latency. The shipped implementation wraps Signalsmith
Stretch (MIT). A commercial engine (zplane élastique, Rubber Band R3) can be dropped in by implementing the
6-method interface in `TimeStretcher.h`.

## Mixer / routing
Channel fader (audio taper) → crossfader (assign A/B/thru; Smooth/Linear/Cut curves) → master gain → limiter.
PFL cue bus (pre-fader) with cue/master blend and cue gain. Output channels are configurable: master (default 1/2),
cue (default 3/4), and per-deck direct outs (pre-fader) for external-mixer setups. Any output can be set to −1 (off).

## Command-line host

```bash
./build/hype_cli --list-devices
./build/hype_cli --buffer 256 --outs 4 trackA.mp3 trackB.flac    # live; type 'help'
./build/hype_cli --render mix.wav --seconds 90 trackA.mp3 trackB.m4a   # offline synced transition
```

## Verified (tests)
Correct pitch and level across sample rates (44.1 kHz file on a 48 kHz device), vinyl and key-lock tempo, key
shift, pitch range and bend, play/pause/stop, full CDJ cue semantics, seek, end of track, load while playing,
sample-accurate scheduled start, bit-identical vinyl output at block sizes 1/7/256/333/2048, click-free transport
in both paths, smoothed fader/crossfader, two decks running independently on separate outputs, fader/trim/crossfader
gain laws, PFL routing, limiter, BPM detection (96–174 BPM, beat phase within 3 ms), one-shot sync, sync lock,
quantised start, zero allocations on the audio thread, ~46× real time for two key-locked decks, and decoding of
WAV/AIFF/FLAC/MP3/AAC plus corrupt and missing files. The suite also passes under AddressSanitizer + UBSan.

## Known limitations / next steps
- Developed and tested on Linux without audio hardware (offline rendering only). Live latency and xrun behaviour
  on real interfaces, and the macOS/Windows decoder paths, still need hardware testing.
- The beat analyser assumes a constant tempo. Variable-tempo tracks need a full beat tracker.
- Sync lock matches tempo continuously; phase is aligned on engage/quantised start only (no ongoing phase correction).
- No loops, hot cues, EQ/filters, scratching/jog or GUI yet. The deck design (stateless sinc reader, tail
  crossfade, sample-accurate commands) is meant to support them.
- **Licensing:** JUCE 8 is AGPLv3 or commercial. A closed-source product needs a JUCE licence. FFmpeg is
  LGPL and must stay dynamically linked. Signalsmith Stretch is MIT.
