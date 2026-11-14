#include "app/AppSettings.h"
#include "edit/RecordTiming.h"

namespace aligna
{
namespace
{
const juce::String latencyKey = "latencyOffsetMs", prerollKey = "preroll", deviceKey = "audioDeviceState";
}

AppSettings::AppSettings()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "Aligna";
    o.filenameSuffix = "settings";
#if JUCE_LINUX || JUCE_BSD
    o.folderName = ".config/Aligna"; // JUCE puts folderName directly under ~ on Linux
#else
    o.folderName = "Aligna";
#endif
    o.osxLibrarySubFolder = "Application Support";
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    o.millisecondsBeforeSaving = 0; // save synchronously on every change
    props.setStorageParameters(o);
}

juce::PropertiesFile& AppSettings::file() const
{
    auto* f = props.getUserSettings();
    jassert(f != nullptr); // getUserSettings() always returns a file once storage parameters are set
    return *f;
}

double AppSettings::latencyOffsetMs() const
{
    return clampLatencyOffsetMs(file().getDoubleValue(latencyKey, 0.0));
}

void AppSettings::setLatencyOffsetMs(double ms) { file().setValue(latencyKey, clampLatencyOffsetMs(ms)); }

bool AppSettings::prerollOn() const { return file().getBoolValue(prerollKey, true); }

void AppSettings::setPrerollOn(bool on) { file().setValue(prerollKey, on); }

std::unique_ptr<juce::XmlElement> AppSettings::audioDeviceState() const { return file().getXmlValue(deviceKey); }

void AppSettings::setAudioDeviceState(const juce::XmlElement* xml)
{
    if (xml != nullptr)
        file().setValue(deviceKey, xml);
    else
        file().removeValue(deviceKey);
}
} // namespace aligna
