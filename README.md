# HYPE DJ PRO

Two-deck, low-latency DJ application in C++17 / JUCE 8: a real-time audio engine (playback, transport,
tempo/pitch, key lock, sync), a two-channel DJ mixer with EQ and headphone cue, audio device management with
disconnect recovery, and a dark professional two-deck interface. Every on-screen control drives the real engine.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/hype_engine_tests                                   # engine: 1297 checks, ~10 s
"./build/HypeDJPro_artefacts/Release/HYPE DJ PRO"           # the app
```

Targets: `HypeDJPro` (desktop app; `.app` bundle on macOS), `hype_engine` (static library), `hype_cli`
(command-line host / offline renderer), `hype_engine_tests`.

**Drivers:** WASAPI and DirectSound on Windows, CoreAudio on macOS, ALSA (+ JACK if `libjack` headers are
installed) on Linux. **ASIO** needs the Steinberg ASIO SDK, which can't be redistributed: download it and
configure with `-DHYPE_ASIO_SDK_DIR=C:/path/to/asiosdk`.

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
   UI thread(s)                                   audio thread (device callback or silent clock)
 ┌────────────────────────────────────────┐      ┌────────────────────────────────────────────────┐
 │ EngineInterface (Api/HypeEngineApi.h)  │      │ AudioEngine render loop                        │
 │  DeckControl  ─► FIFO / atomics ───────┼─────►│  DeckEngine A ─┐                               │
 │  MixerControl ─► atomics ──────────────┼─────►│  DeckEngine B ─┴► MixerEngine ─► CueEngine     │
 │  CueControl   ─► atomics ──────────────┼─────►│                       │              │         │
 │  DeviceControl (message thread)        │      │                       └► OutputRouter ◄┘        │
 │  getState()/meters ◄── atomics ────────┼◄─────┤                              ► device channels  │
 └────────────────────────────────────────┘      └────────────────────────────────────────────────┘
   TrackLoader (loader threads): decode + BPM + waveform ─► DeckEngine (pointer swap on the audio thread)
   AudioDeviceManager (message thread): hardware, output assignment ─► OutputRouter, recovery watchdog
   housekeeping thread ◄── retired tracks (memory is never freed on the audio thread)
```

| Component | Role |
|---|---|
| `Api/HypeEngineApi.h` | **The UI's only view of the engine**: `EngineInterface`, `DeckControl`, `MixerControl`, `CueControl`, `DeviceControl` and value types, with a threading contract. The build enforces the boundary. |
| `Engine/AudioEngine` | Processing callback, render loop, scheduling, sync, silent clock; composes the components |
| `Engine/DeckEngine` | One deck: transport, cue, tempo/pitch, key lock, de-click |
| `Engine/MixerEngine` | Channel gain, 3-band EQ, channel volume, crossfader, master, limiter, meters |
| `Engine/CueEngine` | Headphone monitoring (cue bus, mix, level, split) |
| `Engine/OutputRouter` | Master / headphone / direct outputs → physical channels |
| `Engine/AudioDeviceManager` | Hardware selection, output configuration, disconnection recovery, persistence |
| `Engine/TrackLoader` | Background decoding (JUCE/FFmpeg), BPM/beat grid and waveform analysis |
| `Engine/SincInterpolator`, `TimeStretcher`, `ThreeBandEq`, `SpscQueue` | DSP and real-time building blocks |

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the signal flow, threading contract and ownership.

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
| Load / eject | `EngineInterface::loadTrack(deck, file, cb)`, `DeckControl::eject` |
| Play / pause / toggle | `play(atSample=-1)`, `pause()`, `togglePlayPause()` |
| Stop | `stop()` pauses and returns to the cue point |
| CDJ cue | `cueDown()` / `cueUp()`: while playing → back to cue and pause; paused on cue → preview while held; paused elsewhere → set cue |
| Set / return to cue | `setCuePoint()`, `setCuePointSeconds(s)`, `returnToCue()` (keeps play state) |
| Seek | `seekToSeconds`, `seekToNormalised`, `seekRelativeSeconds` |
| Pitch fader | `setPitchRange(0.08/0.16/0.5/1.0)`, `setPitchFader(-1..1)`, `setTempoRatio(r)` |
| Pitch bend | `setPitchBend(±ratio)`, added on top of the fader (momentary nudge) |
| Key lock | `setKeyLock(bool)`, `setKeyShiftSemitones(s)` |
| Channel gain | `MixerControl::setChannelGainDb(deck, dB)` |
| BPM / beat grid | auto-detected on load; `setBeatGrid(bpm, firstBeat)` to override |
| Display data | `getState()`: duration, elapsed, remaining, position 0..1, original and current BPM, pitch %, beat phase, cue, peaks |

Engine level: `syncDeck(slave, master, alignPhase)` (with half/double-time matching), `setSyncLock(slave, master)`,
`playInSync(slave, master)` (starts exactly on the master's next beat), `getSampleClock()` for scheduling.

**Key-lock architecture.** In vinyl mode (key lock off, no key shift) playback goes through the sinc resampler, so
pitch follows tempo. With key lock on, the deck switches to a `TimeStretcher`. The deck keeps the playhead aligned
with what is actually heard by accounting for the stretcher's latency. The shipped implementation wraps Signalsmith
Stretch (MIT). A commercial engine (zplane élastique, Rubber Band R3) can be dropped in by implementing the
6-method interface in `TimeStretcher.h`.

## Mixer
```
deck (post channel GAIN) → 3-band EQ ─┬→ channel fader → crossfader A/B → MASTER bus → master volume → limiter → master outs
                                       ├→ headphone CUE (pre-fader) → CUE bus → blend with master → phones volume → headphone outs
                                       └→ direct out (optional, external-mixer mode)
```
- **Channel gain** −24…+12 dB, **channel fader** with audio taper, per-channel pre-fader **meters**.
- **3-band EQ** (Linkwitz-Riley isolator, 250 Hz / 2.5 kHz): flat to ±0.05 dB at unity, each band from full
  **kill** to +6 dB, plus kill buttons. Changes are smoothed, so they never click.
- **Crossfader** blends the real deck signals. Its **curve is continuously adjustable**, from a constant-power
  smooth blend to a scratch cut with ~3% cut-in; there's also a reverse ("hamster") switch.
- **Master volume** (up to +6 dB) followed by a −0.1 dBFS safety **limiter** (LIMIT indicator). The master
  **meter** shows peak with hold and a clip LED; RMS is also measured.

## Headphone monitoring
- CUE button per channel, so either deck **or both** can be cued at once.
- The cue tap is **pre-fader and independent of the crossfader**. Pre-listening never changes the master; a test
  checks that the master output is bit-identical with cue on and off.
- Cue/master mix and headphone volume. **Split cue** puts cue in the left ear and master in the right, for laptops
  with a single stereo output.
- Separate **headphone output pair**, assignable to any physical channels of the interface.

## Audio device management (`AudioDeviceManager` + Audio Settings panel)
- Driver type, output device, sample rate, buffer size (shown with its latency in ms), active output channels.
- **Master output** and **headphone output** assignment as stereo pairs of physical channels (or off). The needed
  channels are enabled on the device automatically, and the engine maps physical channels to JUCE's packed
  callback buffers.
- **Refresh** rescans every driver.
- **Disconnection recovery.** A watchdog detects a driver error, the device disappearing, or the callback stalling
  for 2 s. It then optionally falls back to the system default device, and **reconnects automatically** with the
  same routing when the interface returns. JUCE's own silent switch to the default device is detected and reported
  as "fallback". If fallback is disabled, nothing plays until the chosen interface is back.
- The current device, sample rate, buffer, latency and CPU load are shown in the top bar, with a status LED.
- Settings persist between sessions (`~/.config/HYPE DJ PRO`, `%APPDATA%`, or `~/Library/Application Support`).
- **Silent clock:** with no output device the engine keeps running in real time on a background thread, so tracks
  can still be loaded and prepared. It hands over to the device through a lock-free handshake and never renders
  at the same time as a device callback.

## Interface
Dark, high-contrast layout: status bar → two full-width scrolling waveforms → Deck A | Mixer | Deck B.
- **Decks:** title/artist, large BPM (current and original), tempo %, elapsed/remaining/duration, overview
  waveform with progress, cue marker and click-to-seek, a **jog wheel** that turns with the playhead (drag to nudge
  while playing, scrub while paused), pitch fader (down = faster) with ±8/16/50/100% range, pitch bend −/+,
  KEY LOCK, CUE (CDJ behaviour, hold to preview), PLAY/PAUSE, SYNC (tempo and phase match, then tempo lock), LOAD
  and EJECT, and drag-and-drop loading. Loading onto a playing deck is refused, to protect the live mix.
- **Waveforms:** real, computed when the track loads and coloured by frequency band (bass red, mids green, highs
  blue), with beat grid, bar lines, cue marker and a centre playhead. Mouse wheel zooms, drag nudges or scrubs.
- **Mixer:** everything listed above. All controls re-sync from the engine 10× a second.
- `--selftest` drives the real widgets by component ID and checks the engine state behind each one (35 checks).

## Performance & real-time design
- No locks, allocation, file I/O or blocking in the audio callback. A test intercepts `malloc` on the audio thread
  while every feature runs, including the EQ and the routing.
- UI → engine only through `HypeEngineApi.h`: atomics for continuous parameters, wait-free FIFOs for actions.
  Engine → UI: atomics and immutable snapshots only.
- Tracks are decoded and analysed (BPM, beat grid, waveform) on low-priority loader threads, then swapped in with a
  single pointer exchange. Retired tracks are freed on a housekeeping thread, never on the audio thread.
- Fades and smoothing everywhere (4 ms de-click crossfades; 20–30 ms parameter ramps), limiter on the master.
- Denormals are flushed; any device buffer size works (internally ≤512-frame chunks).

## Command-line host

```bash
./build/hype_cli --list-devices
./build/hype_cli --buffer 256 --outs 4 trackA.mp3 trackB.flac    # live; type 'help'
./build/hype_cli --render mix.wav --seconds 90 trackA.mp3 trackB.m4a   # offline synced transition
```

## Verified (tests)
**Engine, 1297 checks:** pitch and level across sample rates; vinyl and key-lock tempo; key shift; pitch range and
bend; play/pause/stop; CDJ cue; seek; end of track; load while playing; sample-accurate scheduled start; identical
output for any block size; click-free transport in both playback paths; EQ flat at unity, kills >40 dB, boosts
accurate, and click-free EQ moves; crossfader curve endpoints, monotonicity and reverse; crossfader blending real
signals; headphone cue leaving the master bit-identical, independent of fader and crossfader, both decks at once,
split cue; master volume and meters; limiter; BPM/beat grid; sync, sync lock and quantised start; zero allocations
on the audio thread; CPU headroom; WAV/AIFF/FLAC/MP3/AAC decoding; waveform analysis.

**Device management, with an in-process fake driver:** enumeration; rate/buffer/channel selection; master and
headphones landing on the assigned physical outputs; live re-assignment; unplug → fallback → automatic reconnect
with routing restored; unplug without fallback; stalled-callback detection; driver-error recovery; silent-clock
handover; XML persistence.

**UI:** `--selftest` (35 checks on the real widgets), screenshots under Xvfb, and a virtual ALSA device opened
through JUCE's real ALSA backend.

## Known limitations / next steps
- Developed and tested on Linux without audio hardware (offline rendering only). Live latency and xrun behaviour
  on real interfaces, and the macOS/Windows decoder paths, still need hardware testing.
- The beat analyser assumes a constant tempo. Variable-tempo tracks need a full beat tracker.
- Sync lock matches tempo continuously; phase is aligned on engage/quantised start only (no ongoing phase correction).
- No loops, hot cues, filter FX, scratching (the jog nudges/scrubs), library browser or MIDI controller support yet.
- The GUI was verified under a virtual X server (screenshots and the `--selftest` run) and with a virtual ALSA
  device. It has not been tried on a physical interface, or on macOS/Windows.
- **Licensing:** JUCE 8 is AGPLv3 or commercial. A closed-source product needs a JUCE licence. FFmpeg is
  LGPL and must stay dynamically linked. Signalsmith Stretch is MIT.
