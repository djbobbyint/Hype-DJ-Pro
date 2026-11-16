HYPE DJ PRO - Phase 1 (two-deck engine) - Windows x64
======================================================

Requirements: Windows 10 or 11, 64-bit. No installer and no runtime downloads needed:
everything is in this folder. Keep the four FFmpeg DLLs next to "HYPE DJ PRO.exe".

Run
---
1. Double-click "HYPE DJ PRO.exe".
2. Click AUDIO SETTINGS (top right):
     - Audio driver: "Windows Audio" (WASAPI shared), "Windows Audio (Exclusive Mode)",
       "Windows Audio (Low Latency Mode)" or "DirectSound".
     - Pick your interface, sample rate and buffer size (128-256 samples is typical).
     - Master output = speakers / PA outputs, Headphone output = your headphone outputs
       (e.g. Out 3/4 on a 4-output DJ interface).
   With a plain stereo laptop output, set both to "Out 1/2" and enable SPLIT in the
   headphone section (cue in the left ear, master in the right).
3. Load a track on each deck: LOAD button, or drag a file onto a deck.
   Formats: WAV, AIFF, FLAC, Ogg Vorbis, MP3 (built in); AAC/M4A, ALAC, Opus via the bundled FFmpeg.

ASIO is not included in this build: the Steinberg ASIO SDK cannot be redistributed. Build from
source with -DHYPE_ASIO_SDK_DIR=<path to the SDK> to enable it (docs/BUILDING.md).

Command-line options
--------------------
  "HYPE DJ PRO.exe" --load-a a.mp3 --load-b b.flac       load two tracks at start-up
  "HYPE DJ PRO.exe" --load-a a.mp3 --load-b b.flac --soak 60
        real-time stability run on your audio device for 60 minutes; writes a report to
        Documents\HYPE DJ PRO soak <date>.txt and exits with code 0 (PASS) or 1 (FAIL)
  "HYPE DJ PRO.exe" --load-a a.mp3 --load-b b.flac --selftest
        drives every UI control and checks the audio engine responds (console output)

  hype_cli.exe --list-devices                            list audio drivers and devices
  hype_cli.exe --render mix.wav --seconds 60 a.mp3 b.m4a offline synced transition to a WAV file

Automated tests (tests\ folder, run from a command prompt)
---------------------------------------------------------
  tests\hype_phase1_tests.exe        Phase 1 acceptance tests 1-13 (about 1 minute)
  set HYPE_SOAK_MINUTES=240 && tests\hype_phase1_tests.exe 13      4-hour soak test
  tests\hype_engine_tests.exe        full engine test suite

See TESTING.md for the manual hardware test procedure.

Settings are stored in %APPDATA%\HYPE DJ PRO\HYPE DJ PRO.settings
