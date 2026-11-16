#pragma once

#include "Api/HypeEngineApi.h"
#include "OutputRouter.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>
#include <functional>

namespace hype
{
/** What the AudioDeviceManager needs from the audio engine (implemented by AudioEngine). */
class AudioDeviceHost
{
public:
    virtual ~AudioDeviceHost() = default;
    virtual juce::AudioIODeviceCallback& getAudioCallback() = 0;
    virtual uint32_t getCallbackCount() const = 0;    // increments on every device callback
    virtual bool hasDeviceError() const = 0;          // driver reported an error since the last start
    virtual double getCpuLoad() const = 0;            // 0..1 of the callback budget
    virtual bool isRunningWithoutOutput() const = 0;  // silent clock active
};

/**
    AudioDeviceManager: audio hardware and output configuration (message thread only).

    * Owns the juce::AudioDeviceManager and registers the engine's audio callback with it.
    * Enumerates driver types (ASIO / WASAPI / DirectSound on Windows, CoreAudio on macOS,
      ALSA / JACK on Linux), devices, output channels, sample rates and buffer sizes.
    * Applies a configuration, enabling the channels the master and headphone outputs need.
    * Output assignment: master and headphone outputs are stereo pairs of physical channels,
      written to the OutputRouter.
    * Watchdog: detects a lost device (driver error, device removed, callback stalled) and
      recovers - reopening the preferred device when available again, optionally falling back
      to the system default device meanwhile (never silently).
    * Serialises its configuration to XML.
*/
class AudioDeviceManager : public DeviceControl,
                           private juce::Timer,
                           private juce::ChangeListener
{
public:
    AudioDeviceManager (AudioDeviceHost& host, OutputRouter& router);
    ~AudioDeviceManager() override;

    //--- DeviceControl -----------------------------------------------------------
    juce::StringArray getDeviceTypes() const override;
    juce::StringArray getDeviceNames (const juce::String& typeName) const override;   // last scan
    void refreshDevices() override;
    juce::StringArray getOutputChannelNames() const override;
    juce::BigInteger getActiveOutputChannels() const override;
    juce::Array<double> getAvailableSampleRates() const override;
    juce::Array<int> getAvailableBufferSizes() const override;
    juce::String apply (const DeviceSettings&) override;
    juce::String setOutputAssignment (int masterFirstChannel, int cueFirstChannel) override;
    void setFallbackToDefaultDevice (bool b) override { settings.fallbackToDefaultDevice = b; }
    void closeDevice() override;
    DeviceSettings getSettings() const override       { return settings; }
    DeviceStatus getStatus() const override;
    std::unique_ptr<juce::XmlElement> createStateXml() const override;
    juce::String restoreFromXml (const juce::XmlElement*) override;
    void setStatusListener (std::function<void()> l) override { onStatusChanged = std::move (l); }

    //--- engine / tests ------------------------------------------------------------
    /** The underlying JUCE device manager (e.g. to add a custom device type). */
    juce::AudioDeviceManager& getJuceDeviceManager() noexcept { return deviceManager; }
    /** Watchdog tick (an internal timer calls it every 500 ms; public for tests). */
    void poll();
    void setWatchdogEnabled (bool shouldRun);

private:
    void timerCallback() override { poll(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::String openDevice (const juce::String& typeName, const juce::String& deviceName, bool isFallback);
    bool isPreferredDeviceAvailable();
    bool currentDeviceLooksDead();
    void attachCallback();
    void applyRouting();
    void setState (DeviceState, const juce::String& message = {});
    void notify();

    AudioDeviceHost& host;
    OutputRouter& router;
    mutable juce::AudioDeviceManager deviceManager;   // JUCE query methods are non-const
    std::function<void()> onStatusChanged;

    DeviceSettings settings;
    DeviceState state = DeviceState::noDevice;
    juce::String lastMessage;
    bool wantDevice = false;

    uint32_t lastCallbackCount = 0;
    int stalledTicks = 0;
    int ticksSinceRescan = 0;
};
} // namespace hype
