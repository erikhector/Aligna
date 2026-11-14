#pragma once
#include "align/AlignResults.h"
#include "app/AppController.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

namespace aligna
{
// Content of the auto-align window: search-range slider, Align/Cancel, progress, per-clip results with tick boxes, Apply.
// Each run has a detached worker thread that owns all it touches: the MixSnapshot taken when Align is pressed and
// a RunState shared with the dialog. It never touches history, pool, UI or the message queue; the dialog collects
// the results from the RunState on its timer. Cancelling or closing sets a flag and lets go of the RunState, so the
// message thread never waits for the worker, which stops on its own shortly afterwards.
class AlignDialog : public juce::Component, private juce::ListBoxModel, private juce::Timer
{
public:
    // onFinished is called (message thread) when the dialog wants to close (Apply, or Cancel while idle/done).
    AlignDialog(AppController&, juce::String referenceClipId, std::vector<juce::String> targetClipIds,
                std::optional<juce::Range<SamplePos>> selection, std::function<void()> onFinished);
    ~AlignDialog() override;

    void stopWork(); // cancel the worker and forget it, without waiting (also done by the destructor)
    void projectChanged(); // call after any project change: marks results that no longer fit as stale
    void resized() override;
    void paint(juce::Graphics&) override;

private:
    struct RunState;
    class RowComponent;
    enum class Phase { Idle, Running, Done };

    void startAlign();
    void cancelPressed();
    void applyPressed();
    void collectRows();
    void rowArrived(AlignRow);
    void workFinished(bool cancelled);
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
    double rate = 48000.0;
    std::vector<AlignRow> rows;
    std::vector<bool> ticked;
    ClipAnchor refAnchor;       // reference placement when Align was pressed
    bool showedStale = false;
    double progressShown = 0.0;

    juce::Label rangeLabel, noteLabel, statusLabel;
    juce::Slider rangeSlider;
    juce::TextButton alignButton { "Align" }, cancelButton { "Cancel" }, applyButton { "Apply" };
    juce::ProgressBar progressBar { progressShown };
    juce::ListBox list;
    std::shared_ptr<RunState> run; // the run in progress, shared with its worker thread; null when none
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AlignDialog)
};
} // namespace aligna
