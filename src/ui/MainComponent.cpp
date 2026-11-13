#include "ui/MainComponent.h"

namespace aligna
{
MainComponent::MainComponent(AppController& a) : app(a), transport(a)
{
    addAndMakeVisible(transport);
    transport.importButton.onClick = [this] { chooseImport(); };
    setWantsKeyboardFocus(true);
    setSize(1200, 800);
}

void MainComponent::chooseImport()
{
    chooser = std::make_unique<juce::FileChooser>("Import audio", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems
                             | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& fc) {
                             juce::String errors;
                             app.importFiles(fc.getResults(), errors);
                             if (errors.isNotEmpty())
                                 juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                        "Import problems", errors);
                             repaint();
                         });
}

bool MainComponent::keyPressed(const juce::KeyPress& k)
{
    if (k == juce::KeyPress::spaceKey)
    {
        transport.togglePlay();
        return true;
    }
    return false;
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1f22));
    g.setColour(juce::Colours::white.withAlpha(0.7f));
    g.setFont(juce::FontOptions(16.0f));
    g.drawText(juce::String((int) app.history().current().tracks.size()) + " track(s)",
               getLocalBounds().withTrimmedTop(48), juce::Justification::centred);
}

void MainComponent::resized() { transport.setBounds(getLocalBounds().removeFromTop(48)); }
}
