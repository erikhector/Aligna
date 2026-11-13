#pragma once

#include "app/AppController.h"
#include "ui/TransportBar.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace aligna
{
class MainComponent : public juce::Component
{
public:
    explicit MainComponent(AppController&);
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    void chooseImport();

    AppController& app;
    TransportBar transport;
    std::unique_ptr<juce::FileChooser> chooser;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
}
