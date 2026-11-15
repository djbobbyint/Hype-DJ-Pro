// HYPE DJ PRO - command-line engine host.
//
//   hype_cli --list-devices
//   hype_cli [device options] [trackA] [trackB]          interactive live session
//   hype_cli --render mix.wav [--seconds 90] trackA trackB  offline demo transition
//
// Device options: --type <ALSA|JACK|CoreAudio|ASIO|...> --device <name>
//                 --rate <Hz> --buffer <frames> --outs <2|4|...>

#include "Engine/AudioEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>
#include <string>

using namespace hype;

namespace
{
juce::String formatTime (double seconds)
{
    seconds = juce::jmax (0.0, seconds);
    const int m = (int) (seconds / 60.0);
    const double s = seconds - m * 60.0;
    return juce::String (m).paddedLeft ('0', 2) + ":" + juce::String (s, 2).paddedLeft ('0', 5);
}

void printDeck (const char* name, const Deck::State& s)
{
    std::cout << "Deck " << name << ": ";
    if (! s.loaded) { std::cout << "(empty)\n"; return; }

    std::cout << (s.playing ? (s.cuePreviewing ? "CUE-PREVIEW" : "PLAYING") : "PAUSED ")
              << "  " << s.title.substring (0, 32)
              << "\n   elapsed " << formatTime (s.elapsedSeconds)
              << "  remaining -" << formatTime (s.remainingSeconds)
              << "  duration " << formatTime (s.durationSeconds)
              << "  pos " << juce::String (s.position * 100.0, 1) << "%"
              << "\n   bpm " << (s.originalBpm > 0 ? juce::String (s.currentBpm, 2) + " (orig " + juce::String (s.originalBpm, 2) + ")" : juce::String ("--"))
              << "  pitch " << (s.pitchPercent >= 0 ? "+" : "") << juce::String (s.pitchPercent, 2) << "%"
              << "  key-lock " << (s.keyLock ? "ON" : "off")
              << "  key " << juce::String (s.keyShiftSemitones, 1) << " st"
              << "  cue " << formatTime (s.cueSeconds)
              << "\n";
}

void printHelp()
{
    std::cout << R"(
Deck commands (prefix with a or b):
  load <file>        play | pause | toggle | stop
  cue (tap) | cuedown | cueup | setcue | cuegoto | cueat <sec>
  seek <sec> | seek% <0-100> | skip <+/-sec>
  pitch <-1..1> (fader) | range <8|16|50|100> | tempo <ratio> | bend <+/-ratio>
  keylock on|off | key <semitones> | trim <dB>
  vol <0..1> | pfl on|off | assign a|b|thru | direct <L> <R> (-1 -1 = off)
  grid <bpm> <firstBeatSec>
Engine:
  xf <0..1> | curve smooth|linear|cut | master <gain> | cuemix <0..1> | cuegain <gain>
  sync <slave> <master> | lock <slave> <master|off> | qplay <slave> <master>
  status | device | help | quit
)";
}

int deckFromChar (const juce::String& s)
{
    if (s.equalsIgnoreCase ("a")) return 0;
    if (s.equalsIgnoreCase ("b")) return 1;
    return -1;
}

void handleCommand (AudioEngine& engine, const juce::String& line, bool& quit)
{
    auto tokens = juce::StringArray::fromTokens (line, " ", "\"");
    tokens.removeEmptyStrings();
    if (tokens.isEmpty())
        return;

    auto& mixer = engine.getMixer();
    const auto cmd = tokens[0].toLowerCase();
    const auto arg = [&] (int i) { return tokens[i].unquoted(); };

    if (cmd == "quit" || cmd == "exit") { quit = true; return; }
    if (cmd == "help") { printHelp(); return; }
    if (cmd == "status")
    {
        printDeck ("A", engine.getDeck (0).getState());
        printDeck ("B", engine.getDeck (1).getState());
        const auto m = mixer.getAndResetMeters();
        std::cout << "Master peak " << juce::String (juce::Decibels::gainToDecibels (juce::jmax (m.masterL, m.masterR)), 1)
                  << " dBFS" << (m.masterClipped ? "  [LIMITING]" : "")
                  << "  xfader " << juce::String (mixer.getCrossfader(), 2) << "\n";
        return;
    }
    if (cmd == "device")
    {
        const auto d = engine.getDeviceStatus();
        std::cout << (d.open ? "Open: " : "Closed: ") << d.typeName << " / " << d.deviceName << "  "
                  << d.sampleRate << " Hz, " << d.bufferSize << " frames, " << d.numOutputChannels << " outs, "
                  << juce::String (d.outputLatencyMs, 1) << " ms latency, xruns " << d.xruns
                  << ", cpu " << juce::String (d.cpuLoad * 100.0, 1) << "%\n";
        return;
    }
    if (cmd == "xf")      { mixer.setCrossfader (arg (1).getFloatValue()); return; }
    if (cmd == "master")  { mixer.setMasterGain (arg (1).getFloatValue()); return; }
    if (cmd == "cuemix")  { mixer.setCueMix (arg (1).getFloatValue()); return; }
    if (cmd == "cuegain") { mixer.setCueGain (arg (1).getFloatValue()); return; }
    if (cmd == "curve")
    {
        const auto c = arg (1).toLowerCase();
        mixer.setCrossfaderCurve (c == "cut" ? Mixer::CrossfaderCurve::Cut
                                : c == "linear" ? Mixer::CrossfaderCurve::Linear
                                                : Mixer::CrossfaderCurve::Smooth);
        return;
    }
    if (cmd == "sync" || cmd == "lock" || cmd == "qplay")
    {
        const int slave = deckFromChar (arg (1));
        const int master = arg (2).equalsIgnoreCase ("off") ? -1 : deckFromChar (arg (2));
        if (slave < 0) { std::cout << "usage: " << cmd << " <a|b> <a|b>\n"; return; }
        if (cmd == "sync")  engine.syncDeck (slave, master, true);
        if (cmd == "lock")  engine.setSyncLock (slave, master);
        if (cmd == "qplay") engine.playInSync (slave, master);
        return;
    }

    const int d = deckFromChar (cmd);
    if (d < 0 || tokens.size() < 2) { std::cout << "Unknown command (type 'help')\n"; return; }

    auto& deck = engine.getDeck (d);
    const auto action = tokens[1].toLowerCase();
    const auto value = arg (2);

    if      (action == "load")
    {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (tokens.joinIntoString (" ", 2).unquoted());
        engine.loadTrackAsync (d, file, [d] (int, const AudioFileDecoder::Result& r)
        {
            if (r.wasOk())
                std::cout << "\n[Deck " << (d == 0 ? "A" : "B") << " loaded: " << r.track->getInfo().title
                          << " | " << r.track->getInfo().decoderName << " | "
                          << juce::String (r.track->getInfo().bpm, 2) << " bpm]\n> " << std::flush;
            else
                std::cout << "\n[Deck " << (d == 0 ? "A" : "B") << " load failed: " << r.error << "]\n> " << std::flush;
        });
    }
    else if (action == "play")     deck.play();
    else if (action == "pause")    deck.pause();
    else if (action == "toggle")   deck.togglePlayPause();
    else if (action == "stop")     deck.stop();
    else if (action == "cue")      { deck.cueDown(); deck.cueUp(); }
    else if (action == "cuedown")  deck.cueDown();
    else if (action == "cueup")    deck.cueUp();
    else if (action == "setcue")   deck.setCuePoint();
    else if (action == "cuegoto")  deck.returnToCue();
    else if (action == "cueat")    deck.setCuePointSeconds (value.getDoubleValue());
    else if (action == "seek")     deck.seekToSeconds (value.getDoubleValue());
    else if (action == "seek%")    deck.seekToNormalised (value.getDoubleValue() / 100.0);
    else if (action == "skip")     deck.seekRelativeSeconds (value.getDoubleValue());
    else if (action == "pitch")    deck.setPitchFader (value.getDoubleValue());
    else if (action == "range")    deck.setPitchRange (value.getDoubleValue() / 100.0);
    else if (action == "tempo")    deck.setTempoRatio (value.getDoubleValue());
    else if (action == "bend")     deck.setPitchBend (value.getDoubleValue());
    else if (action == "keylock")  deck.setKeyLock (value.equalsIgnoreCase ("on"));
    else if (action == "key")      deck.setKeyShiftSemitones (value.getDoubleValue());
    else if (action == "trim")     deck.setTrimDb (value.getFloatValue());
    else if (action == "vol")      mixer.setChannelVolume (d, value.getFloatValue());
    else if (action == "pfl")      mixer.setCueEnabled (d, value.equalsIgnoreCase ("on"));
    else if (action == "direct")   mixer.setDirectOutput (d, value.getIntValue(), arg (3).getIntValue());
    else if (action == "grid")     deck.setBeatGrid (value.getDoubleValue(), arg (3).getDoubleValue());
    else if (action == "assign")
        mixer.setCrossfaderAssign (d, value.equalsIgnoreCase ("a") ? Mixer::CrossfaderAssign::A
                                    : value.equalsIgnoreCase ("b") ? Mixer::CrossfaderAssign::B
                                                                   : Mixer::CrossfaderAssign::Thru);
    else
        std::cout << "Unknown deck action (type 'help')\n";
}

//==============================================================================
int renderOffline (AudioEngine& engine, const juce::File& outFile, double seconds,
                   const juce::File& trackA, const juce::File& trackB)
{
    constexpr double sr = 44100.0;
    constexpr int block = 256;
    engine.prepareToPlay (sr, block);

    for (auto [deck, file] : { std::pair { 0, trackA }, std::pair { 1, trackB } })
    {
        auto r = engine.loadTrackSync (deck, file);
        if (! r.wasOk()) { std::cerr << "Failed to load " << file.getFullPathName() << ": " << r.error << "\n"; return 1; }
        std::cout << "Deck " << (deck == 0 ? "A" : "B") << ": " << r.track->getInfo().title << " ["
                  << r.track->getInfo().decoderName << ", " << juce::String (r.track->getInfo().bpm, 2) << " bpm]\n";
    }

    auto& mixer = engine.getMixer();
    auto& a = engine.getDeck (0);
    auto& b = engine.getDeck (1);

    // Script: A plays; B is key-locked, beat-synced and started on A's beat at 1/3;
    // crossfade A -> B over the middle third.
    mixer.setCrossfader (0.0f);
    b.setKeyLock (true);
    a.play();

    juce::AudioBuffer<float> out (2, (int) (seconds * sr));
    float* ptrs[2];
    const int startB = (int) (seconds / 3.0 * sr);
    const int fadeEnd = (int) (seconds * 2.0 / 3.0 * sr);
    bool bStarted = false;

    for (int pos = 0; pos < out.getNumSamples(); pos += block)
    {
        if (! bStarted && pos >= startB - (int) sr)
        {
            engine.playInSync (1, 0);
            bStarted = true;
        }
        if (pos >= startB && pos <= fadeEnd)
            mixer.setCrossfader ((float) (pos - startB) / (float) (fadeEnd - startB));

        const int n = juce::jmin (block, out.getNumSamples() - pos);
        ptrs[0] = out.getWritePointer (0) + pos;
        ptrs[1] = out.getWritePointer (1) + pos;
        engine.processBlock (ptrs, 2, n);
    }

    outFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (outFile.createOutputStream().release());
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), sr, 2, 24, {}, 0));
    if (writer == nullptr) { std::cerr << "Cannot write " << outFile.getFullPathName() << "\n"; return 1; }
    stream.release();
    writer->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());

    std::cout << "Rendered " << seconds << " s to " << outFile.getFullPathName() << "\n";
    printDeck ("A", a.getState());
    printDeck ("B", b.getState());
    return 0;
}
} // namespace

int main (int argc, char** argv)
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    auto option = [&] (const juce::String& name) -> juce::String
    {
        const int i = args.indexOf (name);
        if (i < 0 || i + 1 >= args.size()) return {};
        auto v = args[i + 1];
        args.removeRange (i, 2);
        return v;
    };

    AudioEngine engine;

    if (args.contains ("--help") || args.contains ("-h"))
    {
        std::cout << "usage: hype_cli [--list-devices] [--render out.wav --seconds N] [--type T] [--device D]"
                     " [--rate Hz] [--buffer N] [--outs N] [trackA] [trackB]\n";
        printHelp();
        return 0;
    }

    if (args.contains ("--list-devices"))
    {
        auto& dm = engine.getDeviceManager();
        for (auto* type : dm.getAvailableDeviceTypes())
        {
            type->scanForDevices();
            std::cout << type->getTypeName() << ":\n";
            for (auto& name : type->getDeviceNames (false))
                std::cout << "   " << name << "\n";
        }
        return 0;
    }

    const auto renderPath = option ("--render");
    const auto secondsStr = option ("--seconds");

    AudioEngine::DeviceSettings settings;
    settings.deviceType   = option ("--type");
    settings.outputDevice = option ("--device");
    if (auto v = option ("--rate");   v.isNotEmpty()) settings.sampleRate = v.getDoubleValue();
    if (auto v = option ("--buffer"); v.isNotEmpty()) settings.bufferSize = v.getIntValue();
    if (auto v = option ("--outs");   v.isNotEmpty()) settings.numOutputChannels = v.getIntValue();

    const auto cwd = juce::File::getCurrentWorkingDirectory();

    if (renderPath.isNotEmpty())
    {
        if (args.size() < 2) { std::cerr << "--render needs two tracks\n"; return 1; }
        return renderOffline (engine, cwd.getChildFile (renderPath),
                              secondsStr.isNotEmpty() ? secondsStr.getDoubleValue() : 90.0,
                              cwd.getChildFile (args[0]), cwd.getChildFile (args[1]));
    }

    const auto error = engine.openDevice (settings);
    if (error.isNotEmpty())
    {
        std::cerr << "Could not open audio device: " << error << "\n"
                     "(Use --list-devices, or --render to mix offline.)\n";
        return 1;
    }

    if (engine.getDeviceStatus().numOutputChannels < 4)
        engine.getMixer().setCueOutput (-1, -1);   // stereo device: master only

    bool quit = false;
    handleCommand (engine, "device", quit);
    for (int d = 0; d < juce::jmin (2, args.size()); ++d)
        handleCommand (engine, juce::String (d == 0 ? "a" : "b") + " load \"" + args[d] + "\"", quit);

    std::cout << "HYPE DJ PRO engine ready. Type 'help' for commands.\n";
    std::string line;
    while (! quit)
    {
        std::cout << "> " << std::flush;
        if (! std::getline (std::cin, line))
            break;
        handleCommand (engine, juce::String (line), quit);
    }

    engine.closeDevice();
    return 0;
}
