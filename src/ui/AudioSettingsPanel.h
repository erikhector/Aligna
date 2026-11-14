#pragma once
#include "app/AppController.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace aligna
{
// Device selector (inputs and outputs, individual input channels) plus the per-machine recording settings.
class AudioSettingsPanel : public juce::Component
{
public:
    explicit AudioSettingsPanel(AppController&);
    void resized() override;

private:
    AppController& app;
    juce::AudioDeviceSelectorComponent selector;
    juce::Label recordingTitle { {}, "Recording" }, latencyLabel { {}, "Latency offset (ms)" }, prerollLabel { {}, "Pre-roll" };
    juce::Slider latency;
    juce::ComboBox preroll;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioSettingsPanel)
};
} // namespace aligna
