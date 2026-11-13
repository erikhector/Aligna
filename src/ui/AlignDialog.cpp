#include "ui/AlignDialog.h"
#include "edit/ClipOps.h"
#include "engine/Mixer.h"

namespace aligna
{
// Processes the targets one after another against an immutable snapshot.
class AlignDialog::Worker : public juce::Thread
{
public:
    Worker(AlignDialog& d, int tok, std::shared_ptr<const MixSnapshot> s, juce::String ref, std::vector<AlignRow> t,
           std::optional<juce::Range<SamplePos>> sel, double maxShift)
        : juce::Thread("Aligna auto-align"), owner(&d), token(tok), snap(std::move(s)), referenceId(std::move(ref)),
          rows(std::move(t)), selection(sel), maxShiftSeconds(maxShift), progress(&d.progressValue)
    {
    }

    void cancel()
    {
        cancelled = true;
        signalThreadShouldExit();
    }

    void run() override
    {
        const auto count = rows.size();
        for (size_t i = 0; i < count && ! cancelled; ++i)
        {
            auto row = rows[i];
            juce::String error;
            auto res = computeAlignment(
                *snap, referenceId, row.clipId, selection, maxShiftSeconds,
                [&](float p) {
                    progress->store((float) ((double) i + juce::jlimit(0.0f, 1.0f, p)) / (float) count);
                    return ! cancelled && ! threadShouldExit();
                },
                error);
            if (cancelled)
                break;
            row.result = res;
            row.error = res ? juce::String() : (error.isNotEmpty() ? error : juce::String("Alignment failed"));
            juce::MessageManager::callAsync([safe = owner, tok = token, row] {
                if (safe != nullptr)
                    safe->rowArrived(tok, row);
            });
        }
        juce::MessageManager::callAsync([safe = owner, tok = token, c = cancelled.load()] {
            if (safe != nullptr)
                safe->workFinished(tok, c);
        });
    }

private:
    juce::Component::SafePointer<AlignDialog> owner;
    int token;
    std::shared_ptr<const MixSnapshot> snap;
    juce::String referenceId;
    std::vector<AlignRow> rows;
    std::optional<juce::Range<SamplePos>> selection;
    double maxShiftSeconds;
    std::atomic<float>* progress;
    std::atomic<bool> cancelled { false };
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
    if (worker != nullptr)
    {
        worker->cancel();
        worker->stopThread(5000);
        worker.reset();
    }
    ++runToken; // late callbacks are ignored
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

    progressValue = 0.0f;
    progressShown = 0.0;
    phase = Phase::Running;
    statusLabel.setText("Aligning...", juce::dontSendNotification);
    updateButtons();
    auto snap = makeSnapshot(proj, app.pool());
    worker = std::make_unique<Worker>(*this, ++runToken, snap, referenceId, std::move(todo), selection,
                                      rangeSlider.getValue());
    worker->startThread();
    startTimerHz(30);
}

void AlignDialog::timerCallback() { progressShown = progressValue.load(); }

void AlignDialog::cancelPressed()
{
    if (phase == Phase::Running)
    {
        // Flag only; the worker finishes within one progress callback and reports back.
        if (worker != nullptr)
            worker->cancel();
        statusLabel.setText("Cancelling...", juce::dontSendNotification);
        cancelButton.setEnabled(false);
    }
    else
    {
        stopWork();
        if (onFinished)
            onFinished();
    }
}

void AlignDialog::rowArrived(int token, AlignRow r)
{
    if (token != runToken)
        return;
    ticked.push_back(shouldPreTick(r));
    rows.push_back(std::move(r));
    if (rowStale(rows.size() - 1)) // project changed while aligning
        ticked.back() = false;
    list.updateContent();
    list.repaint();
}

void AlignDialog::workFinished(int token, bool cancelled)
{
    if (token != runToken)
        return;
    stopTimer();
    if (worker != nullptr)
    {
        worker->stopThread(5000);
        worker.reset();
    }
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
