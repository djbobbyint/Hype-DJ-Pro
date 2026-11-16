# HYPE DJ PRO — Phase 1 Test Plan

Two layers:

1. **Automated tests.** These run anywhere with no audio hardware, through a simulated 8-output audio interface
   (`Tests/FakeAudioDevice.h`) driven by the engine's real device-callback path.
2. **Manual hardware procedure.** The same 13 items checked by ear and eye on a real Windows PC with a real audio
   interface. Hardware behaviour (drivers, latency, xruns, hot-plugging) can only be confirmed this way.

---

## 1. Automated tests

| Executable | What it covers |
|---|---|
| `hype_phase1_tests` | **The 13 Phase 1 items below, end to end.** Audio files on disk → TrackLoader → decks → mixer → headphone cue → output router → the audio device callback. Controls go through the public `EngineInterface` exactly as the UI does, and output is captured per *physical* device channel (master = outputs 1/2, headphones = outputs 3/4). |
| `hype_engine_tests` | Component-level engine tests (1297 checks): DSP accuracy, transport and cue semantics, key lock, EQ, routing, device recovery, zero audio-thread allocations, decoders. |
| `HYPE DJ PRO --selftest` | Drives every real UI widget (35 checks) and verifies the engine responded. |

```
hype_phase1_tests                          # all 13 (30-minute soak), ~1 minute
hype_phase1_tests 4 5                      # only items 4 and 5
HYPE_SOAK_MINUTES=240 hype_phase1_tests 13 # 4-hour soak (~6 minutes)
```
On Windows: `set HYPE_SOAK_MINUTES=240` then `tests\hype_phase1_tests.exe 13`.

### Test fixtures
The suite generates two different "music" tracks: kick on every beat, bassline, a sustained chord, a lead tone and
off-beat hi-hats.
- **Track A:** 126 BPM, C major, 44.1 kHz, MP3 (WAV where the `ffmpeg` tool isn't available).
- **Track B:** 122 BPM, D major, 48 kHz, FLAC.

Each track's **lead tone is its fingerprint** (A = 784 Hz, B = 932 Hz). The tests measure exactly how much of each
track reaches every physical output, using a windowed DFT at those frequencies. Clean tonal tracks are used for
click detection in the soak.

### What each item measures

| # | Item | Automated verification |
|---|---|---|
| 1 | Load a track into Deck A | Async load through the API → decoder, duration (60 s), detected BPM (126.0 ±0.1), waveform present; deck B untouched |
| 2 | Load another track into Deck B | Same for B (122.0 BPM); deck A still has track A; different waveform |
| 3 | Play both simultaneously | Both playing; both fingerprints on the master at the expected level (±10%); each deck advances exactly 2.0 s in 2.0 s of audio; L = R |
| 4 | Pause A without stopping B | A's position frozen to the sample; A's fingerprint gone (< −40 dB); B advances 1.0 s and is unchanged (±5%) |
| 5 | Pause B without stopping A | Mirror of 4 |
| 6 | Independent volume | A fader at 0.5 → A exactly −12.0 dB (audio taper), B unchanged (±0.3 dB); B fader 0 → B silent, A unchanged; B at 0.75 → −5.0 dB |
| 7 | Crossfader left → right | 9 positions: each deck's level matches the constant-power law within ±0.02, A monotonically falls and B rises; continuous 100-step sweep with no NaN or overs |
| 8 | Actual audio blending | **Sample-exact superposition**: at 5 crossfader positions the master equals (A alone) + (B alone) with error < 10⁻⁵, and each deck's share equals the crossfader gain |
| 9 | EQ | Same track position and same 2 beats for every measurement: low kill > 25 dB cut at 30–80 Hz while mids/highs stay within 1 dB; high kill > 25 dB cut at 7–18 kHz; mid kill > 20 dB cut on the lead; +6 dB / −12 dB boost and cut accurate; knob fully left = kill; reset restores within 0.5 dB; deck A's EQ doesn't affect deck B and vice versa |
| 10 | Independent headphone monitoring | Audience hears A only while B is cued: B in the headphones at unity and absent from the master (< −40 dB); cue is pre-fader (B fader closed, still in headphones); both decks cued; headphone level and master volume independent; cue/master blend; split cue (cue left, master right); **master bit-identical with and without cueing** |
| 11 | Different sample rates | Files at 44.1/48/96 kHz × device at 44.1/48/96 kHz (9 combinations): pitch 440.000 Hz ±0.05, timing exact, level exact; key lock at +10% tempo keeps 440 Hz at full level |
| 12 | Audio device changes (during playback) | Buffer 256→64→512; rate 48→96→44.1 kHz; master/headphones moved to outputs 5/6 and 7/8; switch to a 2-channel device and back; unplug (→ fallback device) and replug (→ automatic reconnect with routing restored); driver error recovery. After **every** change both decks are still playing, positions continued (no reset) and audio is on the right physical outputs |
| 13 | Long-duration stability | Both decks play for 30 audio-minutes (configurable) while a seeded "virtual DJ" acts twice a second: crossfader, EQ, kills, volumes, tempo, key lock, seeks, headphone cue, pitch bend, sync lock, and track reloads through the background loader. Checked on **every** sample: no NaN/Inf, no overs, no clicks; every block: no audio-thread allocation, playing decks always advance; at the end the sample clock matches the audio rendered and replaced tracks have been freed |

### Results (this build)
| Platform | Phase 1 suite | Engine suite |
|---|---|---|
| Linux x64 (native) | **13/13 PASS**, 483 checks; 30-minute soak: 0 clicks / NaN / overs / stalls / allocations | 1297/1297 |
| Linux, 4-hour soak | **PASS**: 240 audio-minutes, 28,724 actions, 441 reloads, 0 clicks, 0 allocations, 2 tracks in memory at the end | — |
| Linux under ThreadSanitizer | **13/13 PASS**, 0 data races | 1297/1297, 0 races |
| Linux under AddressSanitizer + UBSan | — | 1297/1297, 0 errors |
| **Windows x64 build** (run under Wine 9) | **13/13 PASS**, 483 checks, 30-minute soak at 36× real time | 1287/1287 |

The Windows numbers come from Wine on Linux, not a Windows PC. Wine also can't show this app's windows on a headless
display (JUCE's per-window DXGI vertical-blank thread never wakes there), so the **Windows GUI has to be checked by
hand** (section 2). On Windows the "zero allocations" check is not active (the allocation interceptor is glibc-only);
the Linux run covers it.

---

## 2. Manual hardware test procedure (Windows)

**Equipment:** a Windows 10/11 PC, an audio interface with at least 4 outputs (e.g. a 2-channel DJ controller
interface or any 4-out USB interface), speakers on outputs 1/2, headphones on outputs 3/4. Also two real music
tracks with clearly different content, ideally an MP3 and a FLAC or M4A.

**Setup:** start `HYPE DJ PRO.exe` → AUDIO SETTINGS → driver *Windows Audio (Low Latency Mode)* (or *Exclusive
Mode*), your interface, 48 kHz, 256 samples, **Master output = Out 1/2**, **Headphone output = Out 3/4**. Close the
panel. The top bar should show your device, *48.0 kHz*, *256 smp*, a latency figure and a green LED.

Record **PASS/FAIL** and notes for each step.

| # | Steps | Expected result |
|---|---|---|
| 1 | Deck A → LOAD → choose track 1 (or drag it onto deck A) | Title and artist appear, large BPM shown, duration shown, overview and scrolling waveforms drawn, jog wheel ring at 0. Deck B still empty |
| 2 | Deck B → LOAD → track 2 | Same on deck B; deck A unchanged |
| 3 | Crossfader to the centre. Press PLAY on A, then PLAY on B | Both tracks audible on the speakers at the same time; both elapsed times count up and remaining times count down; both jog wheels turn; channel meters and master meter move |
| 4 | Press PAUSE on A | A stops instantly with no click; B carries on without a glitch; A's time display freezes |
| 5 | PLAY A, then PAUSE B | Mirror of 4 |
| 6 | PLAY both. Pull A's channel fader down slowly, then up; repeat with B | Only that deck gets quieter or louder, smoothly with no zipper noise; the other deck is unaffected; the channel meter follows |
| 7 | Crossfader fully left, then slowly fully right | Left: only A. Right: only B. In between: a smooth blend with no volume dip or jump; no clicks while moving. Turn CURVE to the right and repeat: the blend becomes a sharp cut near the ends. REV swaps the sides |
| 8 | Crossfader centre, both playing. Close A's fader, then reopen it while closing B's | You hear the sum of both tracks; removing one leaves exactly the other, with no artefacts |
| 9 | On deck A, turn LOW fully left, then back to centre; repeat for MID and HI; press each K (kill) button | LOW: kick and bass vanish. MID: vocals and leads vanish. HI: hi-hats and cymbals vanish. Centre restores the original sound exactly; no clicks while turning; deck B unaffected. Repeat on deck B |
| 10 | Crossfader fully left (audience hears A). Headphones on. Press CUE on channel B | **Headphones:** track B. **Speakers:** still only track A, no change at all. Pull B's channel fader down: still in the headphones. Press CUE on A too: both in the headphones. Turn CUE/MST: blends towards the master. PHONES: headphone volume only. SPLIT: cue in the left ear, master in the right |
| 11 | AUDIO SETTINGS → 44.1 kHz, play 30 s; 96 kHz, play 30 s (if supported); back to 48 kHz | Playback carries on in tune (no pitch change) at every rate; the top bar shows the new rate; the decks keep their positions. Load a 44.1 kHz and a 48 kHz file at the same time: both in tune |
| 12a | While both decks play: change buffer size 256 → 128 → 512 | A short gap at most while the device restarts, then playback continues from the same place; the latency figure updates |
| 12b | Change Headphone output to another pair (or Off and back) | Headphones move to that output; the speakers are unaffected |
| 12c | **Unplug the USB interface** while playing | The top bar turns amber ("lost… using default device", or "reconnecting" if fallback is unticked). With fallback on, the master continues on the laptop's built-in output and the app keeps running |
| 12d | **Plug the interface back in** | Within about 2 s the app reconnects to the interface by itself; the top bar turns green; master and headphones are back on their outputs |
| 12e | Switch driver (e.g. Windows Audio ↔ DirectSound) | Device reopens and playback continues |
| 13 | Close the app. From a command prompt: `"HYPE DJ PRO.exe" --load-a track1.mp3 --load-b track2.flac --soak 60` | The app plays both decks for 60 minutes while operating the controls automatically. Exit code 0 and a report in Documents saying **Result: PASS** (0 stalls, 0 device problems, 0 clips, driver xruns 0). Listen at intervals for dropouts or distortion |

Also run `tests\hype_phase1_tests.exe` on the same PC; it should report 13/13 PASS.

**Also worth checking:** CPU in the top bar stays low (typically < 10%); no xruns at 256 samples (some interfaces need
512 with WASAPI shared mode); closing and reopening the app restores the device and routing.

### Reporting a failure
Include the driver/device/rate/buffer from the top bar, the step, what you heard, and (for 13) the soak report file.

---

## 3. Phase 1 success criteria

| Criterion | Automated evidence | Needs on hardware |
|---|---|---|
| Two different music tracks can be loaded | Items 1–2 (MP3 + FLAC; AAC via FFmpeg verified separately) | Steps 1–2 |
| Both tracks play simultaneously | Item 3 | Step 3 |
| Independent playback controls per deck | Items 4–5, engine transport/cue tests, UI self-test | Steps 4–5 |
| Crossfader blends the two actual audio signals | Items 7–8 (sample-exact superposition) | Steps 7–8 |
| EQ and volume controls work | Items 6, 9 | Steps 6, 9 |
| Headphone monitoring independent of the master | Item 10 (bit-identical master) | Step 10 |
| Audio device selection works | Items 11–12 with the simulated interface; Linux ALSA device opened through JUCE | Steps 11–12 (**hardware only**) |
| Playback stable during extended testing | Item 13 (30 min; 4 h run), TSan/ASan clean | Step 13 `--soak` (**hardware only**) |
