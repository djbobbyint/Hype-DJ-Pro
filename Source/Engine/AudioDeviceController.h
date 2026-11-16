#pragma once

#include "AudioEngine.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>
#include <functional>

namespace hype
{
/**
    Audio device management for HYPE DJ PRO (message thread only).

    * Enumerates driver types (ASIO / WASAPI / DirectSound on Windows, CoreAudio on macOS,
      ALSA / JACK on Linux), devices, output channels, sample rates and buffer sizes.
    * Applies a configuration and makes sure the channels used by the master and headphone
      outputs are enabled on the device.
    * Owns the output assignment: master and headphone outputs are stereo pairs of physical
      device channels (or off), forwarded to the Mixer.
    * Watchdog: detects a lost device (driver error, device removed from the system, or the
      audio callback stalling) and recovers - reopening the preferred device as soon as it is
      available again, optionally falling back to the system default device meanwhile.
    * Serialises its configuration to XML for persistence between sessions.
*/
class AudioDeviceController : private juce::Timer,
                              private juce::ChangeListener
{
public:
    struct Settings
    {
        juce::String typeName;          // e.g. "ASIO", "Windows Audio", "CoreAudio", "ALSA"
        juce::String deviceName;        // empty = default device of the type
        double sampleRate = 0.0;        // 0 = device default
        int bufferSize = 0;             // 0 = device default
        juce::BigInteger outputChannels; // empty = derived from the master/headphone pairs
        int masterFirstChannel = 0;     // stereo pair starting here, -1 = off
        int cueFirstChannel = 2;        // stereo pair starting here, -1 = off
        bool fallbackToDefaultDevice = true;
    };

    enum class State { noDevice, running, recovering, fallback };

    struct Status
    {
        State state = State::noDevice;
        juce::String typeName, deviceName, message;
        double sampleRate = 0.0;
        int bufferSize = 0;
        int numOutputChannels = 0;       // channels the device has
        int numActiveOutputs = 0;
        double latencyMs = 0.0;
        double cpuLoad = 0.0;
        int xruns = -1;
        int masterFirstChannel = -1, cueFirstChannel = -1;
    };

    explicit AudioDeviceController (AudioEngine& engine);
    ~AudioDeviceController() override;

    //--- enumeration ---------------------------------------------------------
    juce::StringArray getDeviceTypes() const;
    /** Devices found by the last scan (see refreshDevices()). */
    juce::StringArray getDeviceNames (const juce::String& typeName) const;
    /** Rescans every driver type for devices (e.g. after plugging in an interface). */
    void refreshDevices();

    /** Output channel names of the open device (e.g. "Out 1", "Out 2"...). */
    juce::StringArray getOutputChannelNames() const;
    /** Output channels currently enabled on the open device. */
    juce::BigInteger getActiveOutputChannels() const;
    juce::Array<double> getAvailableSampleRates() const;
    juce::Array<int> getAvailableBufferSizes() const;

    //--- configuration --------------------------------------------------------
    /** Opens/reconfigures the device. Returns an empty string on success. */
    juce::String apply (const Settings& settings);
    /** Changes only the output assignment (no device restart unless a channel must be enabled). */
    juce::String setOutputAssignment (int masterFirstChannel, int cueFirstChannel);
    void closeDevice();
    /** Recovery policy only; doesn't touch the running device. */
    void setFallbackToDefaultDevice (bool shouldFallBack) { settings.fallbackToDefaultDevice = shouldFallBack; }

    const Settings& getSettings() const noexcept { return settings; }
    Status getStatus() const;

    std::unique_ptr<juce::XmlElement> createStateXml() const;
    /** Restores and applies a saved configuration (falls back to the default device on failure). */
    juce::String restoreFromXml (const juce::XmlElement*);

    //--- recovery -------------------------------------------------------------
    /** Watchdog tick (called by an internal timer every 500 ms; public for tests). */
    void poll();
    void setWatchdogEnabled (bool shouldRun);

    /** Called on the message thread whenever the device or its state changes. */
    std::function<void()> onStatusChanged;

private:
    void timerCallback() override { poll(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::String openDevice (const juce::String& typeName, const juce::String& deviceName, bool isFallback);
    bool isPreferredDeviceAvailable();
    bool currentDeviceLooksDead();
    void applyRoutingToMixer();
    void setState (State, const juce::String& message = {});
    void notify();

    AudioEngine& engine;
    juce::AudioDeviceManager& deviceManager;
    Settings settings;
    State state = State::noDevice;
    juce::String lastMessage;
    bool wantDevice = false;

    uint32_t lastCallbackCount = 0;
    int stalledTicks = 0;
    int ticksSinceRescan = 0;
};
} // namespace hype
