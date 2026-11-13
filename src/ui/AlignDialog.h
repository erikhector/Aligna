#pragma once
#include "align/AlignResults.h"
#include "app/AppController.h"
#include <atomic>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

namespace aligna
{
// Content of the auto-align window: search-range slider, Align/Cancel, progress, per-clip results with tick boxes, Apply.
// The worker thread only reads a MixSnapshot taken when Align is pressed; it never touches history, pool or UI.
class AlignDialog : public juce::Component, private juce::ListBoxModel, private juce::Timer
{
public:
    // onFinished is called (message thread) when the dialog wants to close (Apply, or Cancel while idle/done).
    AlignDialog(AppController&, juce::String referenceClipId, std::vector<juce::String> targetClipIds,
                std::optional<juce::Range<SamplePos>> selection, std::function<void()> onFinished);
    ~AlignDialog() override;

    void stopWork(); // cancel and join the worker (also done by the destructor)
    void projectChanged(); // call after any project change: marks results that no longer fit as stale
    void resized() override;
    void paint(juce::Graphics&) override;

private:
    class Worker;
    class RowComponent;
    enum class Phase { Idle, Running, Done };

    void startAlign();
    void cancelPressed();
    void applyPressed();
    void rowArrived(int token, AlignRow);
    void workFinished(int token, bool cancelled);
    void updateButtons();
    bool rowStale(size_t row) const;
    void refreshStale();
    void timerCallback() override;

    // ListBoxModel
    int getNumRows() override { return (int) rows.size(); }
    void paintListBoxItem(int, juce::Graphics&, int, int, bool) override {}
    juce::Component* refreshComponentForRow(int row, bool, juce::Component*) override;

    AppController& app;
    juce::String referenceId;
    std::vector<juce::String> targets;
    std::optional<juce::Range<SamplePos>> selection;
    std::function<void()> onFinished;

    Phase phase = Phase::Idle;
    int runToken = 0;
    double rate = 48000.0;
    std::vector<AlignRow> rows;
    std::vector<bool> ticked;
    ClipAnchor refAnchor;       // reference placement when Align was pressed
    bool showedStale = false;
    std::atomic<float> progressValue { 0.0f };
    double progressShown = 0.0;

    juce::Label rangeLabel, noteLabel, statusLabel;
    juce::Slider rangeSlider;
    juce::TextButton alignButton { "Align" }, cancelButton { "Cancel" }, applyButton { "Apply" };
    juce::ProgressBar progressBar { progressShown };
    juce::ListBox list;
    std::unique_ptr<Worker> worker;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AlignDialog)
};
} // namespace aligna
