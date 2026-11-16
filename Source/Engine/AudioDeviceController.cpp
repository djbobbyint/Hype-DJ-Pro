#include "AudioDeviceController.h"

namespace hype
{
namespace
{
    constexpr int watchdogIntervalMs = 500;
    constexpr int stalledTicksBeforeLost = 4;   // ~2 s without a single audio callback
    constexpr int rescanEveryTicks = 4;         // rescan drivers every ~2 s while recovering
}

AudioDeviceController::AudioDeviceController (AudioEngine& e)
    : engine (e), deviceManager (e.getDeviceManager())
{
    deviceManager.addChangeListener (this);
    engine.attachToDeviceManager();
}

AudioDeviceController::~AudioDeviceController()
{
    stopTimer();
    deviceManager.removeChangeListener (this);
}

//==============================================================================
juce::StringArray AudioDeviceController::getDeviceTypes() const
{
    juce::StringArray names;
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        names.add (type->getTypeName());
    return names;
}

juce::StringArray AudioDeviceController::getDeviceNames (const juce::String& typeName) const
{
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        if (type->getTypeName() == typeName)
        {
            type->scanForDevices();
            return type->getDeviceNames (false);
        }
    return {};
}

void AudioDeviceController::refreshDevices()
{
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        type->scanForDevices();
    poll();
    notify();
}

juce::StringArray AudioDeviceController::getOutputChannelNames() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getOutputChannelNames();
    return {};
}

juce::Array<double> AudioDeviceController::getAvailableSampleRates() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getAvailableSampleRates();
    return {};
}

juce::Array<int> AudioDeviceController::getAvailableBufferSizes() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getAvailableBufferSizes();
    return {};
}

//==============================================================================
juce::String AudioDeviceController::apply (const Settings& newSettings)
{
    settings = newSettings;
    wantDevice = true;

    const auto error = openDevice (settings.typeName, settings.deviceName, false);
    if (error.isEmpty())
        return {};

    if (settings.fallbackToDefaultDevice && settings.deviceName.isNotEmpty())
    {
        if (openDevice (settings.typeName, {}, true).isEmpty())
            setState (State::fallback, "\"" + settings.deviceName + "\" unavailable (" + error + ") - using the default device");
        else
            setState (State::recovering, error);
    }
    else
    {
        setState (State::recovering, error);
    }

    startTimer (watchdogIntervalMs);
    return error;
}

juce::String AudioDeviceController::setOutputAssignment (int masterFirst, int cueFirst)
{
    settings.masterFirstChannel = masterFirst;
    settings.cueFirstChannel = cueFirst;

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        applyRoutingToMixer();
        return {};
    }

    // Enable any channel the new assignment needs that the device isn't running yet.
    const auto active = device->getActiveOutputChannels();
    const int available = device->getOutputChannelNames().size();
    bool needsRestart = false;
    for (int first : { masterFirst, cueFirst })
        for (int ch = first; first >= 0 && ch < juce::jmin (first + 2, available); ++ch)
            if (! active[ch])
                needsRestart = true;

    if (! needsRestart)
    {
        applyRoutingToMixer();
        notify();
        return {};
    }

    return openDevice (device->getTypeName(), device->getName(), state == State::fallback);
}

void AudioDeviceController::closeDevice()
{
    wantDevice = false;
    stopTimer();
    deviceManager.closeAudioDevice();
    setState (State::noDevice, "Audio output disabled");
}

void AudioDeviceController::setWatchdogEnabled (bool shouldRun)
{
    if (shouldRun && wantDevice)
        startTimer (watchdogIntervalMs);
    else
        stopTimer();
}

//==============================================================================
juce::String AudioDeviceController::openDevice (const juce::String& typeName, const juce::String& deviceName, bool isFallback)
{
    if (typeName.isNotEmpty() && deviceManager.getCurrentAudioDeviceType() != typeName)
        deviceManager.setCurrentAudioDeviceType (typeName, true);

    auto* type = deviceManager.getCurrentDeviceTypeObject();
    if (type == nullptr)
        return "Audio driver type \"" + typeName + "\" is not available";

    type->scanForDevices();
    const auto names = type->getDeviceNames (false);

    juce::String name = deviceName;
    if (name.isEmpty())
        name = names[juce::jmax (0, type->getDefaultDeviceIndex (false))];

    if (name.isEmpty() || ! names.contains (name))
        return deviceName.isEmpty() ? "No output devices found for " + type->getTypeName()
                                    : "Device \"" + deviceName + "\" not found";

    // How many outputs does this device have? (Use the open device if it's the same one.)
    int numOutputs = 0;
    if (auto* current = deviceManager.getCurrentAudioDevice(); current != nullptr && current->getName() == name)
        numOutputs = current->getOutputChannelNames().size();
    else if (std::unique_ptr<juce::AudioIODevice> probe { type->createDevice (name, {}) })
        numOutputs = probe->getOutputChannelNames().size();

    if (numOutputs <= 0)
        return "Device \"" + name + "\" has no outputs";

    juce::BigInteger outputs = settings.outputChannels;
    for (int first : { settings.masterFirstChannel, settings.cueFirstChannel })
        if (first >= 0)
            outputs.setRange (first, 2, true);
    outputs.setRange (numOutputs, juce::jmax (0, outputs.getHighestBit() + 1 - numOutputs), false);
    if (outputs.isZero())
        outputs.setRange (0, juce::jmin (2, numOutputs), true);

    juce::AudioDeviceManager::AudioDeviceSetup setup = deviceManager.getAudioDeviceSetup();
    setup.outputDeviceName = name;
    setup.inputDeviceName = {};
    setup.sampleRate = settings.sampleRate;
    setup.bufferSize = settings.bufferSize;
    setup.useDefaultOutputChannels = false;
    setup.outputChannels = outputs;
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();

    auto error = deviceManager.setAudioDeviceSetup (setup, true);
    if (error.isEmpty() && deviceManager.getCurrentAudioDevice() == nullptr)
        error = "Device failed to open";

    if (error.isNotEmpty())
        return error;

    // Remember what was actually chosen so it can be restored after a disconnect.
    if (! isFallback)
    {
        settings.typeName = type->getTypeName();
        settings.deviceName = deviceName.isEmpty() ? juce::String() : name;
    }

    engine.attachToDeviceManager();
    applyRoutingToMixer();
    lastCallbackCount = engine.getCallbackCount();
    stalledTicks = 0;

    setState (isFallback ? State::fallback : State::running,
              isFallback ? "Using \"" + name + "\" until \"" + settings.deviceName + "\" is available again"
                         : juce::String());
    startTimer (watchdogIntervalMs);
    return {};
}

void AudioDeviceController::applyRoutingToMixer()
{
    auto& mixer = engine.getMixer();
    int available = AudioEngine::maxOutputChannels;
    if (auto* device = deviceManager.getCurrentAudioDevice())
        available = device->getOutputChannelNames().size();

    auto clampPair = [available] (int first, int& l, int& r)
    {
        l = (first >= 0 && first < available) ? first : -1;
        r = (first >= 0 && first + 1 < available) ? first + 1 : l;   // mono device: duplicate
    };

    int l, r;
    clampPair (settings.masterFirstChannel, l, r);
    mixer.setMasterOutput (l, r);
    clampPair (settings.cueFirstChannel, l, r);
    mixer.setCueOutput (l, r);
}

//==============================================================================
bool AudioDeviceController::isPreferredDeviceAvailable()
{
    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        if (settings.typeName.isNotEmpty() && type->getTypeName() != settings.typeName)
            continue;
        const auto names = type->getDeviceNames (false);
        return settings.deviceName.isEmpty() ? ! names.isEmpty() : names.contains (settings.deviceName);
    }
    return false;
}

bool AudioDeviceController::currentDeviceLooksDead()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr || ! device->isOpen() || engine.hasDeviceError())
        return true;

    if (device->isPlaying())
    {
        const auto count = engine.getCallbackCount();
        stalledTicks = (count == lastCallbackCount) ? stalledTicks + 1 : 0;
        lastCallbackCount = count;
        return stalledTicks >= stalledTicksBeforeLost;
    }
    return true;
}

void AudioDeviceController::poll()
{
    if (! wantDevice)
        return;

    // JUCE reopens the system default device by itself when the current one disappears.
    // Never let that happen silently: report it as a fallback, or close it if not allowed.
    if (state == State::running && settings.deviceName.isNotEmpty())
        if (auto* device = deviceManager.getCurrentAudioDevice(); device != nullptr && device->getName() != settings.deviceName)
        {
            if (settings.fallbackToDefaultDevice)
            {
                applyRoutingToMixer();
                lastCallbackCount = engine.getCallbackCount();
                stalledTicks = 0;
                setState (State::fallback, "\"" + settings.deviceName + "\" was lost - using \"" + device->getName()
                                               + "\" until it is available again");
            }
            else
            {
                deviceManager.closeAudioDevice();
                ticksSinceRescan = rescanEveryTicks;
                setState (State::recovering, "Audio device \"" + settings.deviceName + "\" was lost - reconnecting...");
            }
        }

    if ((state == State::running || state == State::fallback) && currentDeviceLooksDead())
    {
        auto lostName = deviceManager.getCurrentAudioDevice() != nullptr ? deviceManager.getCurrentAudioDevice()->getName()
                                                                          : settings.deviceName;
        deviceManager.closeAudioDevice();
        ticksSinceRescan = rescanEveryTicks;   // rescan immediately
        setState (State::recovering, "Audio device \"" + lostName + "\" was lost - reconnecting...");
    }

    if (state != State::recovering && state != State::fallback)
        return;

    if (++ticksSinceRescan >= rescanEveryTicks)
    {
        ticksSinceRescan = 0;
        for (auto* type : deviceManager.getAvailableDeviceTypes())
            if (settings.typeName.isEmpty() || type->getTypeName() == settings.typeName)
                type->scanForDevices();
    }
    else if (state == State::fallback)
    {
        return;   // between rescans, nothing new to try
    }

    if (isPreferredDeviceAvailable() && openDevice (settings.typeName, settings.deviceName, false).isEmpty())
    {
        setState (State::running, "Reconnected to \"" + deviceManager.getCurrentAudioDevice()->getName() + "\"");
        return;
    }

    if (state == State::recovering && settings.fallbackToDefaultDevice && settings.deviceName.isNotEmpty())
        openDevice (settings.typeName, {}, true);
}

void AudioDeviceController::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Device opened/closed/removed: check immediately rather than waiting for the timer.
    if (wantDevice && (state == State::running || state == State::fallback))
    {
        auto* device = deviceManager.getCurrentAudioDevice();
        if (device == nullptr || (state == State::running && settings.deviceName.isNotEmpty()
                                  && device->getName() != settings.deviceName))
            poll();
    }
    notify();
}

void AudioDeviceController::setState (State s, const juce::String& message)
{
    state = s;
    lastMessage = message;
    notify();
}

void AudioDeviceController::notify()
{
    if (onStatusChanged)
        onStatusChanged();
}

AudioDeviceController::Status AudioDeviceController::getStatus() const
{
    Status s;
    s.state = state;
    s.message = lastMessage;
    s.masterFirstChannel = settings.masterFirstChannel;
    s.cueFirstChannel = settings.cueFirstChannel;

    const auto engineStatus = engine.getDeviceStatus();
    s.cpuLoad = engineStatus.cpuLoad;

    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        s.typeName = device->getTypeName();
        s.deviceName = device->getName();
        s.sampleRate = device->getCurrentSampleRate();
        s.bufferSize = device->getCurrentBufferSizeSamples();
        s.numOutputChannels = device->getOutputChannelNames().size();
        s.numActiveOutputs = device->getActiveOutputChannels().countNumberOfSetBits();
        s.latencyMs = engineStatus.outputLatencyMs;
        s.xruns = device->getXRunCount();
    }
    else
    {
        s.typeName = settings.typeName;
        s.deviceName = settings.deviceName;
    }
    return s;
}

//==============================================================================
std::unique_ptr<juce::XmlElement> AudioDeviceController::createStateXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("HYPE_AUDIO");
    xml->setAttribute ("type", settings.typeName);
    xml->setAttribute ("device", settings.deviceName);
    xml->setAttribute ("sampleRate", settings.sampleRate);
    xml->setAttribute ("bufferSize", settings.bufferSize);
    xml->setAttribute ("outputs", settings.outputChannels.toString (16));
    xml->setAttribute ("master", settings.masterFirstChannel);
    xml->setAttribute ("cue", settings.cueFirstChannel);
    xml->setAttribute ("fallback", settings.fallbackToDefaultDevice);
    return xml;
}

juce::String AudioDeviceController::restoreFromXml (const juce::XmlElement* xml)
{
    Settings s;
    if (xml != nullptr && xml->hasTagName ("HYPE_AUDIO"))
    {
        s.typeName = xml->getStringAttribute ("type");
        s.deviceName = xml->getStringAttribute ("device");
        s.sampleRate = xml->getDoubleAttribute ("sampleRate");
        s.bufferSize = xml->getIntAttribute ("bufferSize");
        s.outputChannels.parseString (xml->getStringAttribute ("outputs"), 16);
        s.masterFirstChannel = xml->getIntAttribute ("master", 0);
        s.cueFirstChannel = xml->getIntAttribute ("cue", 2);
        s.fallbackToDefaultDevice = xml->getBoolAttribute ("fallback", true);
    }
    return apply (s);
}
} // namespace hype
