#include "ui/TransportBar.h"
#include "model/Grid.h"
#include "edit/ClipOps.h"
#include "ui/AudioSettingsPanel.h"
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

// The tempo as the field shows it: two decimals, trailing zeros dropped.
juce::String bpmText(double bpm) { return juce::String(bpm, 2).trimCharactersAtEnd("0").trimCharactersAtEnd("."); }

// Digits with at most one decimal point and at least one digit ("" and "." are not numbers).
bool isNumber(const juce::String& s)
{
    return s.containsAnyOf("0123456789") && s.containsOnly("0123456789.") && s.indexOfChar('.') == s.lastIndexOfChar('.');
}
} // namespace

TransportBar::TransportBar(AppController& a, TimelineState& st) : app(a), tlState(st)
{
    for (auto* c : std::initializer_list<juce::Component*> { &playButton, &stopButton, &recButton, &audioButton, &recLabel, &meter,
                                                              &position, &bpmLabel, &bpmField, &snapButton, &gridBox,
                                                              &sigLabel, &beatsBox, &slashLabel, &unitBox })
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
    snapButton.setClickingTogglesState(true);
    snapButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2f7fd0));
    snapButton.setToggleState(tlState.snapEnabled, juce::dontSendNotification);
    snapButton.setTooltip("Snap to grid (G). Hold Alt while dragging to bypass.");
    snapButton.onClick = [this] { tlState.snapEnabled = snapButton.getToggleState(); };
    // Item ids: 1 = Bar (0 subdivisions), then 1/4, 1/8, 1/16 = 1, 2, 4 subdivisions per beat.
    gridBox.addItem("Bar", 1);
    gridBox.addItem("1/4", 2);
    gridBox.addItem("1/8", 3);
    gridBox.addItem("1/16", 4);
    gridBox.setSelectedId(tlState.subdivisionsPerBeat == 0 ? 1 : tlState.subdivisionsPerBeat == 2 ? 3 : tlState.subdivisionsPerBeat == 4 ? 4 : 2,
                          juce::dontSendNotification);
    gridBox.onChange = [this] {
        static const int subs[] = { 0, 1, 2, 4 };
        tlState.subdivisionsPerBeat = subs[juce::jlimit(1, 4, gridBox.getSelectedId()) - 1];
    };
    slashLabel.setJustificationType(juce::Justification::centred);
    for (int n = 1; n <= 32; ++n)
        beatsBox.addItem(juce::String(n), n);
    for (int u : { 1, 2, 4, 8, 16 })
        unitBox.addItem(juce::String(u), u);
    beatsBox.setTooltip("Beats per bar");
    unitBox.setTooltip("Beat unit (note value that gets one beat)");
    beatsBox.onChange = [this] { commitTimeSignature(); };
    unitBox.onChange = [this] { commitTimeSignature(); };
    recButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff7a2a2a));
    recButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffd9302f));
    recButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffff8a8a));
    recButton.setTooltip("Record a new take (R)");
    recButton.onClick = [this] { toggleRecord(); };
    recLabel.setColour(juce::Label::textColourId, juce::Colour(0xffff5050));
    recLabel.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    recLabel.setVisible(false);
    meter.setVisible(false);
    app.onRecordingStopped = [safe = juce::Component::SafePointer<TransportBar>(this)](juce::String msg) {
        if (safe != nullptr)
        {
            safe->refresh();
            safe->showRecordingMessage(msg);
        }
    };
    playButton.onClick = [this] { togglePlay(); };
    stopButton.onClick = [this] {
        if (app.isRecording())
        {
            toggleRecord();
            return;
        }
        app.engine().stop();
        refresh();
    };
    audioButton.onClick = [this] { showAudioSettings(); };
    refresh();
    startTimerHz(30);
}

TransportBar::~TransportBar()
{
    stopTimer();
    app.onRecordingStopped = nullptr;
}

void TransportBar::showRecordingMessage(const juce::String& msg)
{
    if (msg.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Recording", msg);
}

void TransportBar::toggleRecord()
{
    if (app.isRecording())
    {
        showRecordingMessage(app.stopRecording());
        refresh();
        return;
    }
    juce::Component::SafePointer<TransportBar> safe(this);
    app.startRecording([safe](juce::String error) {
        if (safe == nullptr)
            return;
        // An empty error does not always mean started (a permission prompt may be pending): isRecording() decides.
        if (error.isNotEmpty())
            safe->showRecordingMessage(error);
        safe->refresh();
    });
    refresh();
}

void TransportBar::togglePlay()
{
    if (app.isRecording())
    {
        toggleRecord();
        return;
    }
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
    snapButton.setToggleState(tlState.snapEnabled, juce::dontSendNotification); // G key toggles it too
    playButton.setButtonText(app.engine().isPlaying() ? "Pause" : "Play");

    const bool rec = app.isRecording();
    if (rec && audioWindow != nullptr)
        closeDialogs(); // device changes are not allowed while recording
    if (rec != wasRecording)
    {
        wasRecording = rec;
        if (onRecordingChanged)
            onRecordingChanged(rec);
    }
    recButton.setToggleState(rec, juce::dontSendNotification);
    bpmField.setEnabled(! rec);
    beatsBox.setEnabled(! rec);
    unitBox.setEnabled(! rec);
    audioButton.setEnabled(! rec);
    recLabel.setVisible(rec);
    if (rec)
    {
        const auto elapsed = std::max<SamplePos>(0, app.engine().getPosition() - app.recordingStart());
        const auto secs = (long long) (elapsed / p.sampleRate);
        recLabel.setText(juce::String::formatted("Recording %lld:%02lld", secs / 60, secs % 60), juce::dontSendNotification);
    }
    const bool showMeter = rec || audioWindow != nullptr;
    meter.setVisible(showMeter);
    const auto nowMs = juce::Time::getMillisecondCounterHiRes();
    if (showMeter)
        meter.push(app.inputPeak(), std::min(200.0, nowMs - lastMeterMs));
    lastMeterMs = nowMs;
    // Follows the project (undo / open). Skipped while a menu is open so a popup is not disturbed.
    if (! beatsBox.isPopupActive() && beatsBox.getSelectedId() != p.beatsPerBar)
        beatsBox.setSelectedId(p.beatsPerBar, juce::dontSendNotification);
    if (! unitBox.isPopupActive() && unitBox.getSelectedId() != p.beatUnit)
        unitBox.setSelectedId(p.beatUnit, juce::dontSendNotification);
    if (!bpmField.hasKeyboardFocus(false))
        bpmField.setText(bpmText(p.bpm), juce::dontSendNotification);
}

void TransportBar::commitBpm()
{
    // Also runs when the field merely loses focus, so it must not edit unless the user typed a different tempo:
    // an empty field is not 20 BPM, and the field shows only two decimals of a tempo that may have more.
    const auto text = bpmField.getText().trim();
    if (isNumber(text))
    {
        const auto v = juce::jlimit(20.0, 400.0, text.getDoubleValue());
        if (bpmText(v) != bpmText(app.history().current().bpm))
            app.history().perform("Tempo", [v](Project& p) { return setProjectBpm(p, v); });
    }
    bpmField.setText(bpmText(app.history().current().bpm), juce::dontSendNotification); // the project's, also if refused
}

void TransportBar::commitTimeSignature()
{
    const int bpb = beatsBox.getSelectedId(), unit = unitBox.getSelectedId();
    app.history().perform("Set Time Signature", [bpb, unit](Project& p) { return setTimeSignature(p, bpb, unit); });
    refresh(); // snaps the boxes back to the project if the edit was rejected
}

namespace
{
class AudioWindow : public juce::DialogWindow
{
public:
    explicit AudioWindow(std::function<void()> closeFn)
        : juce::DialogWindow("Audio settings", juce::Colour(0xff26282c), true), onClose(std::move(closeFn))
    {
    }
    void closeButtonPressed() override { onClose(); }

private:
    std::function<void()> onClose;
};
} // namespace

void TransportBar::closeDialogs() { audioWindow.reset(); }

void TransportBar::showAudioSettings()
{
    if (audioWindow != nullptr)
    {
        audioWindow->toFront(true);
        return;
    }
    juce::Component::SafePointer<TransportBar> safe(this);
    auto win = std::make_unique<AudioWindow>([safe] {
        // Deferred: the window must not be deleted from inside its own callback.
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr)
                safe->closeDialogs();
        });
    });
    win->setContentOwned(new AudioSettingsPanel(app), true);
    win->setUsingNativeTitleBar(true);
    win->centreAroundComponent(this, win->getWidth(), win->getHeight());
    win->setVisible(true);
    audioWindow = std::move(win);
}

void TransportBar::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff2a2c30)); }

void TransportBar::resized()
{
    auto r = getLocalBounds().reduced(6);
    playButton.setBounds(r.removeFromLeft(70));
    r.removeFromLeft(4);
    stopButton.setBounds(r.removeFromLeft(70));
    r.removeFromLeft(4);
    recButton.setBounds(r.removeFromLeft(60));
    r.removeFromLeft(12);
    snapButton.setBounds(r.removeFromLeft(60));
    r.removeFromLeft(4);
    gridBox.setBounds(r.removeFromLeft(80));
    r.removeFromLeft(12);
    audioButton.setBounds(r.removeFromRight(150));
    r.removeFromRight(8);
    meter.setBounds(r.removeFromRight(90).reduced(0, 12));
    r.removeFromRight(12);
    unitBox.setBounds(r.removeFromRight(56));
    slashLabel.setBounds(r.removeFromRight(14));
    beatsBox.setBounds(r.removeFromRight(56));
    sigLabel.setBounds(r.removeFromRight(40));
    r.removeFromRight(12);
    bpmField.setBounds(r.removeFromRight(60));
    bpmLabel.setBounds(r.removeFromRight(40));
    recLabel.setBounds(r.removeFromLeft(140));
    position.setBounds(r.reduced(8, 0));
}
} // namespace aligna
