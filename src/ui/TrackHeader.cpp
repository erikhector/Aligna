#include "ui/TrackHeader.h"
#include <cmath>

namespace aligna
{
TrackHeader::TrackHeader(AppController& a, juce::String id) : app(a), trackId(std::move(id))
{
    nameLabel.setEditable(false, true, false);
    nameLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    nameLabel.setColour(juce::Label::backgroundWhenEditingColourId, juce::Colour(0xff111213));
    nameLabel.onTextChange = [this] {
        const auto text = nameLabel.getText().trim();
        edit("Rename Track", [text](Track& t) {
            if (t.name == text || text.isEmpty())
                return false;
            t.name = text;
            return true;
        });
    };

    volume.setSliderStyle(juce::Slider::LinearHorizontal);
    volume.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    volume.setRange(-60.0, 12.0, 0.1);
    volume.setDoubleClickReturnValue(true, 0.0);
    volume.setTooltip("Volume (dB)");
    pan.setSliderStyle(juce::Slider::LinearHorizontal);
    pan.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    pan.setRange(-1.0, 1.0, 0.01);
    pan.setDoubleClickReturnValue(true, 0.0);
    pan.setTooltip("Pan");

    auto commitVolume = [this] {
        const auto v = (float) volume.getValue();
        edit("Track Volume", [v](Track& t) {
            if (std::abs(t.volumeDb - v) < 1.0e-6f)
                return false;
            t.volumeDb = v;
            return true;
        });
    };
    auto commitPan = [this] {
        const auto v = (float) pan.getValue();
        edit("Track Pan", [v](Track& t) {
            if (std::abs(t.pan - v) < 1.0e-6f)
                return false;
            t.pan = v;
            return true;
        });
    };
    // One edit per drag (on release); changes made without the mouse held (wheel, double-click reset) commit at once.
    volume.onDragEnd = commitVolume;
    volume.onValueChange = [this, commitVolume] { if (!volume.isMouseButtonDown()) commitVolume(); };
    pan.onDragEnd = commitPan;
    pan.onValueChange = [this, commitPan] { if (!pan.isMouseButtonDown()) commitPan(); };

    muteButton.setClickingTogglesState(true);
    soloButton.setClickingTogglesState(true);
    muteButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffd9534f));
    soloButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffe0b030));
    muteButton.onClick = [this] {
        const bool on = muteButton.getToggleState();
        edit("Mute", [on](Track& t) { if (t.mute == on) return false; t.mute = on; return true; });
    };
    soloButton.onClick = [this] {
        const bool on = soloButton.getToggleState();
        edit("Solo", [on](Track& t) { if (t.solo == on) return false; t.solo = on; return true; });
    };

    for (auto* c : std::initializer_list<juce::Component*> { &nameLabel, &volume, &pan, &muteButton, &soloButton })
        addAndMakeVisible(*c);
}

template <typename F>
void TrackHeader::edit(const juce::String& name, F&& f)
{
    const auto id = trackId;
    app.history().perform(name, [&](Project& p) {
        for (auto& t : p.tracks)
            if (t.id == id)
                return f(t);
        return false;
    });
}

void TrackHeader::update(const Track& t)
{
    if (!nameLabel.isBeingEdited())
        nameLabel.setText(t.name, juce::dontSendNotification);
    if (!volume.isMouseButtonDown())
        volume.setValue(t.volumeDb, juce::dontSendNotification);
    if (!pan.isMouseButtonDown())
        pan.setValue(t.pan, juce::dontSendNotification);
    muteButton.setToggleState(t.mute, juce::dontSendNotification);
    soloButton.setToggleState(t.solo, juce::dontSendNotification);
}

void TrackHeader::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff26282c));
    g.setColour(juce::Colour(0xff3a3d42));
    g.drawHorizontalLine(getHeight() - 1, 0.0f, (float) getWidth());
    g.drawVerticalLine(getWidth() - 1, 0.0f, (float) getHeight());
    g.setColour(juce::Colours::white.withAlpha(0.5f));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("Vol", 8, 38, 28, 16, juce::Justification::centredLeft);
    g.drawText("Pan", 8, 58, 28, 16, juce::Justification::centredLeft);
}

void TrackHeader::resized()
{
    auto r = getLocalBounds().reduced(6, 4);
    nameLabel.setBounds(r.removeFromTop(26));
    r.removeFromTop(6);
    auto vol = r.removeFromTop(20);
    vol.removeFromLeft(30);
    volume.setBounds(vol);
    auto pn = r.removeFromTop(20);
    pn.removeFromLeft(30);
    pan.setBounds(pn);
    r.removeFromTop(4);
    auto buttons = r.removeFromTop(22);
    muteButton.setBounds(buttons.removeFromLeft(34));
    buttons.removeFromLeft(6);
    soloButton.setBounds(buttons.removeFromLeft(34));
}
} // namespace aligna
