#pragma once

#include "app/AppController.h"
#include "ui/TimelineState.h"
#include "ui/TimelineView.h"
#include "ui/TrackHeader.h"
#include "ui/TransportBar.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace aligna
{
class MainComponent : public juce::Component
{
public:
    explicit MainComponent(AppController&);
    ~MainComponent() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    TimelineState& timelineState() { return tlState; }

private:
    void chooseImport();
    void projectChanged();
    void layoutHeaders();

    AppController& app;
    TimelineState tlState;
    TransportBar transport;
    TimelineView timeline;
    juce::Component headerColumn; // clips its children; headers scroll vertically with the lanes
    std::vector<std::unique_ptr<TrackHeader>> headers;
    std::unique_ptr<juce::FileChooser> chooser;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
}
