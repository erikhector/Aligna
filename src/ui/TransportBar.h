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

    void closeDialogs(); // closes the Audio Settings dialog if open

private:
    void timerCallback() override { refresh(); }
    void commitBpm();
    void commitTimeSignature();
    void showAudioSettings();

    AppController& app;
    TimelineState& tlState;
    std::unique_ptr<juce::DialogWindow> audioWindow; // owned here so it cannot outlive AppController's device manager
    juce::TextButton snapButton { "Snap" };
    juce::ComboBox gridBox;
    juce::TextButton playButton { "Play" }, stopButton { "Stop" }, audioButton { "Audio settings..." };
    juce::Label position, bpmLabel { {}, "BPM" };
    juce::TextEditor bpmField;
    juce::ComboBox beatsBox, unitBox;
    juce::Label sigLabel { {}, "Time" }, slashLabel { {}, "/" };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportBar)
};
} // namespace aligna
