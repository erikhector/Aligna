#include "ui/AlignDialog.h"
#include "edit/ClipOps.h"
#include "engine/Mixer.h"
#include <atomic>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>

namespace aligna
{
// Everything one alignment run shares between the dialog and its worker thread. Each holds it through a shared_ptr,
// so the worker can outlive the dialog; every run gets its own RunState and never meets an older worker.
struct AlignDialog::RunState
{
    std::atomic<bool> cancel { false };   // set by the dialog; the worker stops at its next check
    std::atomic<float> progress { 0.0f }; // written by the worker
    std::atomic<bool> finished { false }; // set by the worker once its last row is in `done`
    std::mutex lock;
    std::vector<AlignRow> done; // finished rows the dialog has not collected yet (guarded by lock)

    // Body of the worker thread: processes the targets one after another against an immutable snapshot.
    // It owns all its arguments and touches nothing else (no dialog, no message queue, no application object), so it
    // can be left running after the dialog has gone; once cancelled it returns within one stage of computeAlignment.
    static void work(std::shared_ptr<RunState> state, std::shared_ptr<const MixSnapshot> snap, juce::String referenceId,
                     std::vector<AlignRow> rows, std::optional<juce::Range<SamplePos>> selection,
                     double maxShiftSeconds)
    {
        const auto count = rows.size();
        for (size_t i = 0; i < count && ! state->cancel; ++i)
        {
            auto row = rows[i];
            juce::String error;
            try
            {
                row.result = computeAlignment(
                    *snap, referenceId, row.clipId, selection, maxShiftSeconds,
                    [&](float p) {
                        state->progress.store((float) ((double) i + juce::jlimit(0.0f, 1.0f, p)) / (float) count);
                        return ! state->cancel;
                    },
                    error);
            }
            catch (const std::bad_alloc&) // the widest search range needs several hundred MB at once
            {
                row.result.reset();
                error = "Not enough memory for this search range";
            }
            if (state->cancel)
                break;
            row.error = row.result ? juce::String() : (error.isNotEmpty() ? error : juce::String("Alignment failed"));
            const std::lock_guard<std::mutex> guard(state->lock);
            state->done.push_back(std::move(row));
        }
        state->finished = true;
    }
};

// One result line: tick box + text.
class AlignDialog::RowComponent : public juce::Component
{
public:
    RowComponent()
    {
        addAndMakeVisible(tick);
        addAndMakeVisible(text);
        text.setInterceptsMouseClicks(false, false);
        text.setMinimumHorizontalScale(0.8f);
    }
    void resized() override
    {
        auto r = getLocalBounds();
        tick.setBounds(r.removeFromLeft(28));
        text.setBounds(r);
    }
    juce::ToggleButton tick;
    juce::Label text;
};

AlignDialog::AlignDialog(AppController& a, juce::String ref, std::vector<juce::String> t,
                         std::optional<juce::Range<SamplePos>> sel, std::function<void()> fin)
    : app(a), referenceId(std::move(ref)), targets(std::move(t)), selection(sel), onFinished(std::move(fin)),
      list("results", this)
{
    rate = app.history().current().sampleRate;

    rangeLabel.setText("Search range (+/- seconds):", juce::dontSendNotification);
    addAndMakeVisible(rangeLabel);
    rangeSlider.setRange(1.0, 300.0, 1.0);
    rangeSlider.setValue(30.0, juce::dontSendNotification);
    rangeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    rangeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 22);
    addAndMakeVisible(rangeSlider);

    if (selection && selectionIsCapped(*selection, rate))
        noteLabel.setText("Using the first 60 s of the selection.", juce::dontSendNotification);
    else if (selection)
        noteLabel.setText("Aligning the time selection.", juce::dontSendNotification);
    else
        noteLabel.setText("No time selection: using up to the first 60 s of overlap.", juce::dontSendNotification);
    noteLabel.setColour(juce::Label::textColourId, juce::Colour(0xffffb74d));
    addAndMakeVisible(noteLabel);

    statusLabel.setText(juce::String((int) targets.size()) + " clip(s) to align to the reference.",
                        juce::dontSendNotification);
    addAndMakeVisible(statusLabel);
    addAndMakeVisible(progressBar);
    list.setRowHeight(26);
    addAndMakeVisible(list);
    for (auto* b : { &alignButton, &cancelButton, &applyButton })
        addAndMakeVisible(*b);
    alignButton.onClick = [this] { startAlign(); };
    cancelButton.onClick = [this] { cancelPressed(); };
    applyButton.onClick = [this] { applyPressed(); };
    updateButtons();
    setSize(640, 420);
}

AlignDialog::~AlignDialog() { stopWork(); }

void AlignDialog::stopWork()
{
    stopTimer();
    if (run != nullptr)
    {
        // Flag and let go, never wait: the worker holds its own reference, stops at its next check and frees
        // everything itself. Whatever it still produces stays in a RunState nobody reads any more.
        run->cancel = true;
        run.reset();
    }
}

void AlignDialog::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff26282c)); }

void AlignDialog::resized()
{
    auto r = getLocalBounds().reduced(12);
    auto top = r.removeFromTop(26);
    rangeLabel.setBounds(top.removeFromLeft(190));
    rangeSlider.setBounds(top);
    r.removeFromTop(4);
    noteLabel.setBounds(r.removeFromTop(22));
    auto bottom = r.removeFromBottom(30);
    cancelButton.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(8);
    applyButton.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(8);
    alignButton.setBounds(bottom.removeFromRight(90));
    r.removeFromBottom(8);
    statusLabel.setBounds(r.removeFromTop(22));
    progressBar.setBounds(r.removeFromTop(20));
    r.removeFromTop(8);
    list.setBounds(r);
}

void AlignDialog::updateButtons()
{
    const bool running = phase == Phase::Running;
    alignButton.setEnabled(phase == Phase::Idle || phase == Phase::Done);
    rangeSlider.setEnabled(! running);
    cancelButton.setButtonText(running ? "Cancel" : "Close");
    bool any = false;
    for (size_t i = 0; i < rows.size(); ++i)
        any = any || (ticked[i] && rows[i].error.isEmpty() && ! rowStale(i));
    applyButton.setEnabled(phase == Phase::Done && any);
}

bool AlignDialog::rowStale(size_t i) const
{
    return i < rows.size() && rows[i].result && isStale(app.history().current(), refAnchor, rows[i]);
}

void AlignDialog::projectChanged() { refreshStale(); }

void AlignDialog::refreshStale()
{
    bool any = false;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rowStale(i))
        {
            any = true;
            ticked[i] = false;
        }
    list.updateContent();
    list.repaint();
    if (any && phase != Phase::Running)
    {
        statusLabel.setText("Project changed " + juce::String(juce::CharPointer_UTF8("\xe2\x80\x94")) + " press Align again.",
                            juce::dontSendNotification);
        showedStale = true;
    }
    else if (! any && showedStale && phase == Phase::Done)
    {
        statusLabel.setText("Done. Tick the clips to move, then Apply.", juce::dontSendNotification);
        showedStale = false;
    }
    updateButtons();
}

void AlignDialog::startAlign()
{
    if (phase == Phase::Running)
        return;
    stopWork();
    const auto& proj = app.history().current();
    rate = proj.sampleRate;
    rows.clear();
    ticked.clear();
    showedStale = false;
    refAnchor = captureAnchor(proj, referenceId);
    for (auto& id : targets) // labels / old starts from the project the snapshot is taken from
        for (auto& t : proj.tracks)
            for (size_t ci = 0; ci < t.clips.size(); ++ci)
                if (t.clips[ci].id == id)
                {
                    AlignRow r;
                    r.clipId = id;
                    r.label = t.name + " #" + juce::String((int) ci + 1);
                    r.oldStart = t.clips[ci].start;
                    r.anchor = captureAnchor(proj, id);
                    rows.push_back(r);
                }
    std::vector<AlignRow> todo = rows;
    rows.clear();
    list.updateContent();

    progressShown = 0.0;
    phase = Phase::Running;
    statusLabel.setText("Aligning...", juce::dontSendNotification);
    updateButtons();
    auto snap = makeSnapshot(proj, app.pool(), app.stretchCache());
    run = std::make_shared<RunState>();
    try
    {
        // Detached: nothing ever joins it. The thread owns the run state and the snapshot through its arguments.
        std::thread(&RunState::work, run, std::move(snap), referenceId, std::move(todo), selection,
                    rangeSlider.getValue())
            .detach();
    }
    catch (const std::system_error&) // no thread could be started
    {
        run.reset();
        phase = Phase::Done;
        statusLabel.setText("Could not start the alignment.", juce::dontSendNotification);
        updateButtons();
        return;
    }
    startTimerHz(30);
}

void AlignDialog::timerCallback()
{
    if (run == nullptr)
        return;
    progressShown = run->progress.load();
    const bool finished = run->finished; // read before collecting: once set, every row is already in `done`
    collectRows();
    if (finished)
        workFinished(false);
}

void AlignDialog::collectRows()
{
    if (run == nullptr)
        return;
    std::vector<AlignRow> arrived;
    {
        const std::lock_guard<std::mutex> guard(run->lock);
        arrived.swap(run->done);
    }
    for (auto& r : arrived)
        rowArrived(std::move(r));
}

void AlignDialog::cancelPressed()
{
    if (phase == Phase::Running)
    {
        // No waiting for the worker: keep the rows that are complete, then flag it and let go.
        collectRows();
        workFinished(true);
    }
    else
    {
        stopWork();
        if (onFinished)
            onFinished();
    }
}

void AlignDialog::rowArrived(AlignRow r)
{
    ticked.push_back(shouldPreTick(r));
    rows.push_back(std::move(r));
    if (rowStale(rows.size() - 1)) // project changed while aligning
        ticked.back() = false;
    list.updateContent();
    list.repaint();
}

void AlignDialog::workFinished(bool cancelled)
{
    stopWork();
    progressShown = cancelled ? progressShown : 1.0;
    phase = Phase::Done;
    cancelButton.setEnabled(true);
    statusLabel.setText(cancelled ? "Cancelled." : "Done. Tick the clips to move, then Apply.",
                        juce::dontSendNotification);
    showedStale = false;
    refreshStale();
}

juce::Component* AlignDialog::refreshComponentForRow(int row, bool, juce::Component* existing)
{
    std::unique_ptr<juce::Component> holder(existing);
    if (row < 0 || row >= (int) rows.size())
        return nullptr;
    auto* rc = dynamic_cast<RowComponent*>(existing);
    if (rc == nullptr)
    {
        holder.reset();
        rc = new RowComponent();
    }
    else
        holder.release();
    const auto& r = rows[(size_t) row];
    const bool stale = rowStale((size_t) row);
    rc->text.setText(describeRow(r, rate) + (stale ? " (stale)" : ""), juce::dontSendNotification);
    const bool ok = r.error.isEmpty() && ! stale;
    rc->text.setColour(juce::Label::textColourId,
                       ! ok ? juce::Colour(0xffef5350)
                            : (r.result && r.result->confidence < kConfidentThreshold ? juce::Colour(0xffffb74d)
                                                                                      : juce::Colours::white));
    rc->tick.setEnabled(ok);
    rc->tick.setToggleState(ok && ticked[(size_t) row], juce::dontSendNotification);
    if (stale)
        rc->text.setColour(juce::Label::textColourId, juce::Colour(0xff9e9e9e));
    rc->tick.onClick = [this, row, rc] {
        if (row < (int) ticked.size())
        {
            ticked[(size_t) row] = rc->tick.getToggleState();
            updateButtons();
        }
    };
    return rc;
}

void AlignDialog::applyPressed()
{
    std::vector<std::pair<juce::String, SamplePos>> moves;
    for (size_t i = 0; i < rows.size(); ++i)
        if (ticked[i] && rows[i].error.isEmpty() && rows[i].result && ! rowStale(i))
            moves.emplace_back(rows[i].clipId, rows[i].result->newStart);
    if (! moves.empty())
        app.history().perform("Auto-align", [&](Project& p) { return applyAlignments(p, moves); });
    stopWork();
    if (onFinished)
        onFinished();
}
} // namespace aligna
