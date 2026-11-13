#pragma once
#include "app/AppController.h"
#include "edit/StretchMath.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace aligna
{
// "Clip tempo..." content: the tempo the source was recorded at (BPM field, Tap, Clear). OK performs "Clip Tempo";
// the clip then follows the project BPM. An empty field clears the tempo (the stretch ratio stays as it is).
class ClipTempoDialog : public juce::Component
{
public:
    ClipTempoDialog(AppController&, const juce::String& clipId, std::function<void()> requestClose);

    void resized() override;
    void focusField() { bpmField.grabKeyboardFocus(); }

private:
    void updateInfo();
    void commit();

    AppController& app;
    juce::String clipId;
    std::function<void()> close;
    TapTempo tapper;
    juce::Label prompt { {}, "Tempo of the recording (BPM)" };
    juce::TextEditor bpmField;
    juce::TextButton tapButton { "Tap" }, clearButton { "Clear" }, okButton { "OK" }, cancelButton { "Cancel" };
    juce::Label info;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClipTempoDialog)
};
} // namespace aligna
