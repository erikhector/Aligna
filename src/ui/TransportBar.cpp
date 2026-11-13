#include "ui/TransportBar.h"
#include "model/Grid.h"
#include <cmath>

namespace aligna
{
namespace
{
juce::String formatPosition(SamplePos pos, const Project& p)
{
    const auto bb = toBarBeat(pos, p);
    const auto totalMs = (long long) std::llround((double) pos * 1000.0 / p.sampleRate);
    const auto m = totalMs / 60000, s = (totalMs / 1000) % 60, ms = totalMs % 1000;
    return juce::String::formatted("%d|%d    %lld:%02lld.%03lld", bb.bar, bb.beat, m, s, ms);
}
} // namespace

TransportBar::TransportBar(AppController& a) : app(a)
{
    for (auto* c : std::initializer_list<juce::Component*> { &importButton, &playButton, &stopButton, &audioButton,
                                                              &position, &bpmLabel, &bpmField })
    {
        addAndMakeVisible(*c);
        c->setWantsKeyboardFocus(false);
    }
    bpmField.setWantsKeyboardFocus(true);
    position.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    position.setJustificationType(juce::Justification::centred);
    bpmField.setInputRestrictions(6, "0123456789.");
    bpmField.setJustification(juce::Justification::centred);
    bpmField.onReturnKey = [this] { commitBpm(); };
    bpmField.onFocusLost = [this] { commitBpm(); };
    playButton.onClick = [this] { togglePlay(); };
    stopButton.onClick = [this] { app.engine().stop(); refresh(); };
    audioButton.onClick = [this] { showAudioSettings(); };
    refresh();
    startTimerHz(30);
}

TransportBar::~TransportBar() { stopTimer(); }

void TransportBar::togglePlay()
{
    if (app.engine().isPlaying())
        app.engine().pause();
    else
        app.engine().play();
    refresh();
}

void TransportBar::refresh()
{
    const auto& p = app.history().current();
    position.setText(formatPosition(app.engine().getPosition(), p), juce::dontSendNotification);
    playButton.setButtonText(app.engine().isPlaying() ? "Pause" : "Play");
    if (!bpmField.hasKeyboardFocus(false))
        bpmField.setText(juce::String(p.bpm, 2).trimCharactersAtEnd("0").trimCharactersAtEnd("."),
                         juce::dontSendNotification);
}

void TransportBar::commitBpm()
{
    const auto v = juce::jlimit(20.0, 400.0, bpmField.getText().getDoubleValue());
    if (std::abs(v - app.history().current().bpm) > 1e-9)
        app.history().perform("Set BPM", [v](Project& p) { p.bpm = v; return true; });
    bpmField.setText(juce::String(v, 2).trimCharactersAtEnd("0").trimCharactersAtEnd("."), juce::dontSendNotification);
}

void TransportBar::showAudioSettings()
{
    auto* sel = new juce::AudioDeviceSelectorComponent(app.devices(), 0, 0, 2, 2, false, false, true, false);
    sel->setSize(480, 360);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(sel);
    o.dialogTitle = "Audio settings";
    o.componentToCentreAround = this;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

void TransportBar::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff2a2c30)); }

void TransportBar::resized()
{
    auto r = getLocalBounds().reduced(6);
    importButton.setBounds(r.removeFromLeft(90));
    r.removeFromLeft(12);
    playButton.setBounds(r.removeFromLeft(70));
    r.removeFromLeft(4);
    stopButton.setBounds(r.removeFromLeft(70));
    r.removeFromLeft(12);
    audioButton.setBounds(r.removeFromRight(150));
    r.removeFromRight(12);
    bpmField.setBounds(r.removeFromRight(60));
    bpmLabel.setBounds(r.removeFromRight(40));
    position.setBounds(r.reduced(8, 0));
}
} // namespace aligna
