#include "ui/AudioSettingsPanel.h"

namespace aligna
{
AudioSettingsPanel::AudioSettingsPanel(AppController& a)
    : app(a), selector(a.devices(), 0, 2, 2, 2, false, false, false, false)
{
    addAndMakeVisible(selector);
    for (auto* c : std::initializer_list<juce::Component*> { &recordingTitle, &latencyLabel, &prerollLabel, &latency, &preroll })
        addAndMakeVisible(*c);
    recordingTitle.setFont(juce::FontOptions(15.0f, juce::Font::bold));

    latency.setSliderStyle(juce::Slider::LinearHorizontal);
    latency.setRange(-500.0, 500.0, 1.0);
    latency.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    latency.setValue(app.settings().latencyOffsetMs(), juce::dontSendNotification);
    latency.setTooltip("Shifts recorded takes earlier (positive) or later (negative) to cancel latency the driver does not report");
    // The settings file is written on every change, so commit on drag end / text entry, not on every drag tick.
    latency.onDragEnd = [this] { app.settings().setLatencyOffsetMs(latency.getValue()); };
    latency.onValueChange = [this] {
        if (! latency.isMouseButtonDown())
            app.settings().setLatencyOffsetMs(latency.getValue());
    };

    preroll.addItem("1 bar", 1);
    preroll.addItem("Off", 2);
    preroll.setSelectedId(app.settings().prerollOn() ? 1 : 2, juce::dontSendNotification);
    preroll.onChange = [this] { app.settings().setPrerollOn(preroll.getSelectedId() == 1); };

    setSize(480, 360 + 100);
}

void AudioSettingsPanel::resized()
{
    auto r = getLocalBounds();
    auto rec = r.removeFromBottom(100).reduced(8, 4);
    selector.setBounds(r);
    recordingTitle.setBounds(rec.removeFromTop(24));
    auto row = rec.removeFromTop(28);
    latencyLabel.setBounds(row.removeFromLeft(140));
    latency.setBounds(row);
    rec.removeFromTop(4);
    row = rec.removeFromTop(28);
    prerollLabel.setBounds(row.removeFromLeft(140));
    preroll.setBounds(row.removeFromLeft(120));
}
} // namespace aligna
