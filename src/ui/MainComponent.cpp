#include "ui/MainComponent.h"

namespace aligna
{
MainComponent::MainComponent()
{
    setSize(1200, 800);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1f22));
    g.setColour(juce::Colours::white.withAlpha(0.7f));
    g.setFont(juce::FontOptions(24.0f));
    g.drawText("Aligna", getLocalBounds(), juce::Justification::centred);
}

void MainComponent::resized() {}
}
