#pragma once
#include "app/AppController.h"
#include "ui/TimelineState.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace aligna
{
class TransportBar : public juce::Component, private juce::Timer
{
public:
    TransportBar(AppController&, TimelineState&);
    ~TransportBar() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void togglePlay();
    void refresh();

    juce::TextButton importButton { "Import..." };

private:
    void timerCallback() override { refresh(); }
    void commitBpm();
    void showAudioSettings();

    AppController& app;
    TimelineState& tlState;
    juce::TextButton snapButton { "Snap" };
    juce::ComboBox gridBox;
    juce::TextButton playButton { "Play" }, stopButton { "Stop" }, audioButton { "Audio settings..." };
    juce::Label position, bpmLabel { {}, "BPM" };
    juce::TextEditor bpmField;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportBar)
};
} // namespace aligna
