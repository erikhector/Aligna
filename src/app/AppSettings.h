#pragma once
#include <juce_data_structures/juce_data_structures.h>
#include <memory>

namespace aligna
{
// Per-machine settings, never stored in the project. File "Aligna.settings" in the user settings folder.
// Message thread only. Every change is written to disk at once.
class AppSettings
{
public:
    AppSettings();

    double latencyOffsetMs() const;
    void setLatencyOffsetMs(double ms); // clamped to [-500, 500]; not finite gives 0
    bool prerollOn() const;             // default true
    void setPrerollOn(bool on);
    std::unique_ptr<juce::XmlElement> audioDeviceState() const; // nullptr if none saved
    void setAudioDeviceState(const juce::XmlElement* xml);      // nullptr clears it

private:
    juce::PropertiesFile& file() const;

    juce::InterProcessLock processLock { "AlignaSettings" }; // held while the file is read or written
    mutable juce::ApplicationProperties props;
};
} // namespace aligna
