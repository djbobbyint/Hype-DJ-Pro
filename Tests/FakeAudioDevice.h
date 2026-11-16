#pragma once

#include "AllocationTracker.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <map>

/**
    Deterministic in-process audio device type for testing device management without hardware.
    Devices can be "plugged" and "unplugged"; the test drives callbacks explicitly with pump().
*/
class FakeAudioDeviceType : public juce::AudioIODeviceType
{
public:
    static constexpr const char* typeName = "HYPE Test Driver";

    class Device : public juce::AudioIODevice
    {
    public:
        Device (FakeAudioDeviceType& t, const juce::String& deviceName, int outs)
            : juce::AudioIODevice (deviceName, FakeAudioDeviceType::typeName), owner (t), numOutputs (outs) {}

        ~Device() override { close(); owner.openDevices.removeFirstMatchingValue (this); }

        juce::StringArray getOutputChannelNames() override
        {
            juce::StringArray s;
            for (int i = 0; i < numOutputs; ++i)
                s.add ("Out " + juce::String (i + 1));
            return s;
        }
        juce::StringArray getInputChannelNames() override       { return {}; }
        juce::Array<double> getAvailableSampleRates() override  { return { 44100.0, 48000.0, 96000.0 }; }
        juce::Array<int> getAvailableBufferSizes() override     { return { 64, 128, 256, 512 }; }
        int getDefaultBufferSize() override                     { return 256; }

        juce::String open (const juce::BigInteger&, const juce::BigInteger& outputs, double sr, int buffer) override
        {
            if (! owner.isPlugged (getName()))
                return "device not present";
            activeOutputs = outputs;
            activeOutputs.setRange (numOutputs, juce::jmax (0, activeOutputs.getHighestBit() + 1 - numOutputs), false);
            sampleRate = sr > 0 ? sr : 48000.0;
            bufferSize = buffer > 0 ? buffer : 256;
            isOpenFlag = true;
            return {};
        }
        void close() override                       { stop(); isOpenFlag = false; }
        bool isOpen() override                      { return isOpenFlag; }
        void start (juce::AudioIODeviceCallback* cb) override
        {
            if (cb != nullptr && isOpenFlag)
            {
                cb->audioDeviceAboutToStart (this);
                callback = cb;
            }
        }
        void stop() override
        {
            if (auto* cb = callback)
            {
                callback = nullptr;
                cb->audioDeviceStopped();
            }
        }
        bool isPlaying() override                   { return callback != nullptr; }
        juce::String getLastError() override        { return {}; }
        int getCurrentBufferSizeSamples() override  { return bufferSize; }
        double getCurrentSampleRate() override      { return sampleRate; }
        int getCurrentBitDepth() override           { return 32; }
        juce::BigInteger getActiveOutputChannels() const override { return activeOutputs; }
        juce::BigInteger getActiveInputChannels() const override  { return {}; }
        int getOutputLatencyInSamples() override    { return 32; }
        int getInputLatencyInSamples() override     { return 0; }

        /** Runs `numBlocks` callbacks, capturing *physical* channels into `capture` (if given).
            With `trackAllocations`, heap allocations made inside the callback are counted. */
        void pump (int numBlocks, juce::AudioBuffer<float>* capture = nullptr, bool trackAllocations = false)
        {
            const int numActive = activeOutputs.countNumberOfSetBits();
            juce::AudioBuffer<float> packed (juce::jmax (1, numActive), bufferSize);
            if (capture != nullptr)
                capture->setSize (numOutputs, numBlocks * bufferSize);

            for (int b = 0; b < numBlocks && callback != nullptr; ++b)
            {
                packed.clear();
                auto* const* outs = packed.getArrayOfWritePointers();
                if (trackAllocations)
                {
                    alloctrack::Scope scope;
                    callback->audioDeviceIOCallbackWithContext (nullptr, 0, outs, numActive, bufferSize, {});
                }
                else
                {
                    callback->audioDeviceIOCallbackWithContext (nullptr, 0, outs, numActive, bufferSize, {});
                }
                if (capture != nullptr)
                {
                    int p = 0;
                    for (int ch = 0; ch < numOutputs; ++ch)
                        if (activeOutputs[ch])
                            capture->copyFrom (ch, b * bufferSize, packed, p++, 0, bufferSize);
                        else
                            capture->clear (ch, b * bufferSize, bufferSize);
                }
            }
        }

        /** Simulates the driver reporting an error (as e.g. CoreAudio/WASAPI do on unplug). */
        void raiseError() { if (callback != nullptr) callback->audioDeviceError ("device removed"); }

        FakeAudioDeviceType& owner;
        const int numOutputs;
        juce::BigInteger activeOutputs;
        double sampleRate = 48000.0;
        int bufferSize = 256;
        bool isOpenFlag = false;
        juce::AudioIODeviceCallback* callback = nullptr;
    };

    FakeAudioDeviceType() : juce::AudioIODeviceType (typeName)
    {
        devices["Fake Interface 8ch"] = { 8, true };
        devices["Fake Laptop 2ch"] = { 2, true };
    }

    void scanForDevices() override {}
    juce::StringArray getDeviceNames (bool wantInputNames) const override
    {
        juce::StringArray s;
        if (! wantInputNames)
            for (auto& [name, info] : devices)
                if (info.plugged)
                    s.add (name);
        return s;
    }
    int getDefaultDeviceIndex (bool) const override
    {
        return juce::jmax (0, getDeviceNames (false).indexOf ("Fake Laptop 2ch"));
    }
    int getIndexOfDevice (juce::AudioIODevice* d, bool asInput) const override
    {
        return d != nullptr && ! asInput ? getDeviceNames (false).indexOf (d->getName()) : -1;
    }
    bool hasSeparateInputsAndOutputs() const override { return true; }

    juce::AudioIODevice* createDevice (const juce::String& outputName, const juce::String&) override
    {
        auto it = devices.find (outputName);
        if (it == devices.end() || ! it->second.plugged)
            return nullptr;
        auto* d = new Device (*this, outputName, it->second.outputs);
        openDevices.add (d);
        return d;
    }

    bool isPlugged (const juce::String& name) const
    {
        auto it = devices.find (name);
        return it != devices.end() && it->second.plugged;
    }

    void setPlugged (const juce::String& name, bool plugged)
    {
        devices[name].plugged = plugged;
        if (! plugged)
            for (auto* d : openDevices)
                if (d->getName() == name)
                {
                    d->raiseError();
                    d->stop();
                }
        callDeviceChangeListeners();
    }

    Device* findOpenDevice (const juce::String& name)
    {
        for (auto* d : openDevices)
            if (d->getName() == name && d->isOpen())
                return d;
        return nullptr;
    }

private:
    struct Info { int outputs = 2; bool plugged = true; };
    std::map<juce::String, Info> devices;
    juce::Array<Device*> openDevices;
};
