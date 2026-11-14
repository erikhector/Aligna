#pragma once
#include "app/AppController.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace aligna
{
class TrackHeader : public juce::Component
{
public:
    static constexpr int width = 180;

    TrackHeader(AppController&, juce::String trackId);
    const juce::String& getTrackId() const { return trackId; }
    void update(const Track&); // refresh controls from the model without triggering edits
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override; // right-click (also on the name): the track menu

private:
    template <typename F> void edit(const juce::String& name, F&& f);
    void requestDelete(); // removes this track as one "Delete Track" step, after the current callback returns

    AppController& app;
    juce::String trackId;
    juce::Label nameLabel;
    juce::Slider volume, pan;
    juce::TextButton muteButton { "M" }, soloButton { "S" }, deleteButton;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackHeader)
};
} // namespace aligna
