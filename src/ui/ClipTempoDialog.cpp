#include "ui/ClipTempoDialog.h"
#include "edit/ClipOps.h"
#include <cmath>

namespace aligna
{
namespace
{
juce::String bpmText(double v) { return juce::String(v, 2).trimCharactersAtEnd("0").trimCharactersAtEnd("."); }
} // namespace

ClipTempoDialog::ClipTempoDialog(AppController& a, const juce::String& id, std::function<void()> requestClose)
    : app(a), clipId(id), close(std::move(requestClose))
{
    Project copy = app.history().current();
    if (const auto* c = findClip(copy, clipId); c != nullptr && c->sourceBpm > 0.0)
        bpmField.setText(bpmText(c->sourceBpm), false);
    bpmField.setInputRestrictions(7, "0123456789.");
    bpmField.setJustification(juce::Justification::centred);
    bpmField.setFont(juce::FontOptions(22.0f));
    bpmField.onTextChange = [this] { updateInfo(); };
    bpmField.onReturnKey = [this] { commit(); };

    tapButton.onClick = [this] {
        const double bpm = tapper.tap(juce::Time::getMillisecondCounterHiRes() / 1000.0);
        if (bpm > 0.0)
            bpmField.setText(bpmText(std::clamp(bpm, 20.0, 400.0)), true);
    };
    clearButton.onClick = [this] { bpmField.setText({}, true); };
    okButton.onClick = [this] { commit(); };
    cancelButton.onClick = [this] { close(); };
    info.setColour(juce::Label::textColourId, juce::Colour(0xffffb74d));
    info.setJustificationType(juce::Justification::centred);

    for (auto* c : std::initializer_list<juce::Component*> { &prompt, &bpmField, &tapButton, &clearButton, &okButton, &cancelButton, &info })
        addAndMakeVisible(c);
    okButton.addShortcut(juce::KeyPress(juce::KeyPress::returnKey));
    setSize(320, 230);
    updateInfo();
}

void ClipTempoDialog::resized()
{
    auto r = getLocalBounds().reduced(12);
    prompt.setBounds(r.removeFromTop(20));
    r.removeFromTop(4);
    bpmField.setBounds(r.removeFromTop(36));
    r.removeFromTop(8);
    auto row = r.removeFromTop(48);
    clearButton.setBounds(row.removeFromRight(70));
    row.removeFromRight(8);
    tapButton.setBounds(row);
    r.removeFromTop(6);
    info.setBounds(r.removeFromTop(20));
    r.removeFromTop(6);
    auto buttons = r.removeFromBottom(28);
    cancelButton.setBounds(buttons.removeFromRight(80));
    buttons.removeFromRight(8);
    okButton.setBounds(buttons.removeFromRight(80));
}

void ClipTempoDialog::updateInfo()
{
    info.setText({}, juce::dontSendNotification);
    const double bpm = bpmField.getText().getDoubleValue();
    if (bpm <= 0.0)
        return;
    Project copy = app.history().current();
    const auto* c = findClip(copy, clipId);
    if (c == nullptr)
        return;
    bool limited = false;
    const double r = clipTempoRatio(copy, *c, bpm, &limited);
    if (limited)
        info.setText(juce::String::fromUTF8("Limited to \xc3\x97") + juce::String(r, 2), juce::dontSendNotification);
}

void ClipTempoDialog::commit()
{
    const auto text = bpmField.getText().trim();
    const double bpm = text.isEmpty() ? 0.0 : juce::jlimit(20.0, 400.0, text.getDoubleValue());
    const auto id = clipId;
    app.history().perform("Clip Tempo", [id, bpm](Project& p) { return setClipTempo(p, id, bpm); });
    close();
}
} // namespace aligna
