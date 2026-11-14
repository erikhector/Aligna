#include "ui/MainComponent.h"

#include "engine/Mixer.h"
#include "io/FileNaming.h"
#include <set>

namespace aligna
{
namespace
{
const juce::String takeLoadTitle = "Adding take...";

void showRecordingMessage(const juce::String& msg)
{
    if (msg.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Recording", msg);
}
} // namespace

MainComponent::MainComponent(AppController& a)
    : app(a), transport(a, tlState), timeline(a, tlState),
      updater(*this, [this](std::function<void()> proceed) { confirmDiscard(std::move(proceed)); })
{
    addAndMakeVisible(transport);
    addAndMakeVisible(timeline);
    addAndMakeVisible(headerColumn);
    headerColumn.setInterceptsMouseClicks(false, true);
    transport.onRecordingChanged = [this](bool rec) {
        if (rec)
        {
            timeline.recordingStarted();
            closeAlignDialog();
        }
        for (auto& h : headers)
            h->setEnabled(! rec);
        menuItemsChanged();
        repaint();
    };
    transport.onStopRecording = [this] { stopRecording(showRecordingMessage); };
    timeline.onViewChanged = [this] { layoutHeaders(); };
    app.onProjectChanged = [this] { projectChanged(); };
    // A recording that stopped by itself: the take is decoded like one stopped by hand.
    app.onTakePending = [this](std::shared_ptr<PendingLoad> load) {
        transport.refresh();
        startLoad(std::move(load), takeLoadTitle, [](const juce::String& msg, bool) { showRecordingMessage(msg); });
    };
    projectChanged();
    setWantsKeyboardFocus(true);
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(this);
#else
    menuBar = std::make_unique<juce::MenuBarComponent>(this);
    addAndMakeVisible(*menuBar);
#endif
    setSize(1200, 800);
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<TimelineView>(&timeline)] {
        if (safe != nullptr)
            safe->grabKeyboardFocus(); // shortcuts work immediately after startup
    });
}

MainComponent::~MainComponent()
{
    closeDialogs();
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    app.onProjectChanged = nullptr;
    app.onTakePending = nullptr;
    transport.onRecordingChanged = nullptr;
    transport.onStopRecording = nullptr;
}

void MainComponent::closeDialogs()
{
    exportTask.reset(); // stops the export thread
    if (loadTask != nullptr)
    {
        loadTask.reset(); // asks the decode to stop and joins its thread
        app.abandonLoad(); // nothing is added; the current project stays; a take keeps its file
    }
    alignWindow.reset(); // stops the align worker thread
    updater.close();     // stops an update check or download
    transport.closeDialogs();
}

void MainComponent::parentHierarchyChanged() { updateTitle(); }

void MainComponent::updateTitle()
{
    if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
        w->setName(windowTitle(app.currentFile().getFileName(), app.history().isDirty()));
}

void MainComponent::projectChanged()
{
    updateTitle();
    const auto& tracks = app.history().current().tracks;
    bool same = headers.size() == tracks.size();
    for (size_t i = 0; same && i < tracks.size(); ++i)
        same = headers[i]->getTrackId() == tracks[i].id;
    if (!same)
    {
        headers.clear();
        for (auto& t : tracks)
        {
            headers.push_back(std::make_unique<TrackHeader>(app, t.id));
            headerColumn.addAndMakeVisible(*headers.back());
        }
    }
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        headers[i]->update(tracks[i]);
        headers[i]->setEnabled(! app.isRecording());
    }

    // Drop selection entries for clips that no longer exist.
    std::set<juce::String> ids;
    for (auto& t : tracks)
        for (auto& c : t.clips)
            ids.insert(c.id);
    std::erase_if(tlState.selectedClipIds, [&](const juce::String& id) { return ids.count(id) == 0; });
    if (!ids.count(tlState.alignReferenceClipId))
        tlState.alignReferenceClipId = {};

    timeline.projectChanged();
    if (alignWindow != nullptr)
        if (auto* dlg = dynamic_cast<AlignDialog*>(alignWindow->getContentComponent()))
            dlg->projectChanged();
    menuItemsChanged(); // Undo/Redo names
    layoutHeaders();
    repaint();
}

void MainComponent::layoutHeaders()
{
    for (size_t i = 0; i < headers.size(); ++i)
        headers[i]->setBounds(0, (int) i * TimelineView::trackHeight - timeline.getViewY(), TrackHeader::width,
                              TimelineView::trackHeight);
}

namespace
{
// Native file choosers are not modal everywhere (on Linux the main window stays live), so a recording can have started
// by the time one returns. Its callback asks here before acting.
bool refuseWhileRecording(const AppController& app, const juce::String& title, const juce::String& message)
{
    if (! app.isRecording())
        return false;
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, title, message);
    return true;
}
} // namespace

void MainComponent::chooseImport()
{
    chooser = std::make_unique<juce::FileChooser>("Import audio", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems
                             | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& fc) {
                             const auto files = fc.getResults();
                             if (! files.isEmpty()
                                 && ! refuseWhileRecording(app, "Import problems", "Stop recording before importing audio."))
                                 importAndReport(files);
                             timeline.grabKeyboardFocus();
                         });
}

namespace
{
// Decodes a load's files; the window shows the file in work and the progress over the whole batch.
class LoadTask : public juce::ThreadWithProgressWindow
{
public:
    LoadTask(const juce::String& title, std::shared_ptr<PendingLoad> l, std::function<void(bool cancelled)> done,
             juce::Component* around)
        : ThreadWithProgressWindow(title, true, true, 10000, {}, around), load(std::move(l)), onDone(std::move(done))
    {
    }
    void run() override
    {
        // The worker owns the batch until it returns; the message thread reads it only in finishLoad.
        auto& items = load->batch.items;
        const int n = (int) items.size();
        int shown = -1;
        completed = load->batch.run([&](int index, float fraction) {
            if (index != shown)
            {
                shown = index;
                auto text = items[(size_t) index].file.getFileName();
                if (n > 1)
                    text += " (" + juce::String(index + 1) + " of " + juce::String(n) + ")";
                setStatusMessage(text);
            }
            setProgress(((double) index + (double) fraction) / (double) n); // files done + part of this one
            return ! threadShouldExit();
        });
    }
    void threadComplete(bool userPressedCancel) override { onDone(userPressedCancel || ! completed); }

private:
    std::shared_ptr<PendingLoad> load;
    std::function<void(bool)> onDone;
    bool completed = false;
};
} // namespace

void MainComponent::startLoad(std::shared_ptr<PendingLoad> load, const juce::String& title,
                              std::function<void(const juce::String&, bool)> done, std::function<void()> beforeCommit)
{
    if (load == nullptr)
        return;
    auto finish = [this, load, done = std::move(done), beforeCommit = std::move(beforeCommit)](bool cancelled) {
        if (! cancelled && beforeCommit)
            beforeCommit();
        const auto message = app.finishLoad(load, cancelled);
        transport.refresh();
        menuItemsChanged();
        if (done)
            done(message, cancelled);
        timeline.grabKeyboardFocus();
    };
    if (load->batch.items.empty()) // nothing to decode (a project without audio): no window
    {
        finish(false);
        return;
    }
    jassert(loadTask == nullptr); // the controller allows one load at a time
    juce::Component::SafePointer<MainComponent> safe(this);
    loadTask = std::make_unique<LoadTask>(
        title, std::move(load),
        [safe, finish = std::move(finish)](bool cancelled) {
            // Deferred: the task must not be deleted from inside its own callback.
            juce::MessageManager::callAsync([safe, finish, cancelled] {
                // No task: closeDialogs() got here first and has abandoned the load.
                if (safe == nullptr || safe->loadTask == nullptr)
                    return;
                safe->loadTask.reset();
                finish(cancelled);
            });
        },
        this);
    static_cast<LoadTask*>(loadTask.get())->launchThread();
    menuItemsChanged();
}

void MainComponent::stopRecording(std::function<void(const juce::String&)> report)
{
    juce::String msg;
    auto load = app.beginStopRecording(msg); // the recorder and the transport stop here, at once
    transport.refresh();
    if (load == nullptr)
    {
        report(msg);
        return;
    }
    startLoad(std::move(load), takeLoadTitle, [report = std::move(report)](const juce::String& m, bool) { report(m); });
}

void MainComponent::importAndReport(const juce::Array<juce::File>& files)
{
    juce::String errors;
    auto load = app.beginImport(files, errors);
    if (load == nullptr)
    {
        if (errors.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Import problems", errors);
        return;
    }
    startLoad(std::move(load), "Importing audio...", [this](const juce::String& errs, bool) {
        if (errs.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Import problems", errs);
        repaint();
    });
}

namespace
{
bool isAudioFile(const juce::String& path)
{
    return juce::File(path).hasFileExtension("wav;aif;aiff;flac;mp3");
}
} // namespace

bool MainComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (auto& f : files)
        if (isAudioFile(f))
            return true;
    return false;
}

void MainComponent::filesDropped(const juce::StringArray& files, int, int)
{
    if (busy())
        return;
    juce::Array<juce::File> audio;
    for (auto& f : files)
        if (isAudioFile(f))
            audio.add(juce::File(f));
    if (! audio.isEmpty())
        importAndReport(audio);
}

std::vector<juce::String> MainComponent::alignTargets() const
{
    std::vector<juce::String> ids;
    for (auto& t : app.history().current().tracks)
        for (auto& c : t.clips)
            if (c.id != tlState.alignReferenceClipId && tlState.selectedClipIds.count(c.id) > 0)
                ids.push_back(c.id);
    return ids;
}

bool MainComponent::canAlign() const
{
    if (tlState.alignReferenceClipId.isEmpty())
        return false;
    bool refExists = false;
    for (auto& t : app.history().current().tracks)
        for (auto& c : t.clips)
            refExists = refExists || c.id == tlState.alignReferenceClipId;
    return refExists && ! alignTargets().empty();
}

namespace
{
class AlignWindow : public juce::DialogWindow
{
public:
    explicit AlignWindow(std::function<void()> closeFn)
        : juce::DialogWindow("Align to Reference", juce::Colour(0xff26282c), true), onClose(std::move(closeFn))
    {
    }
    void closeButtonPressed() override { onClose(); }

private:
    std::function<void()> onClose;
};
} // namespace

void MainComponent::openAlignDialog()
{
    if (alignWindow != nullptr)
    {
        alignWindow->toFront(true);
        return;
    }
    if (! canAlign())
        return;
    juce::Component::SafePointer<MainComponent> safe(this);
    auto close = [safe] {
        // Deferred: the window must not be deleted from inside one of its own callbacks.
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr)
                safe->closeAlignDialog();
        });
    };
    auto win = std::make_unique<AlignWindow>(close);
    win->setContentOwned(new AlignDialog(app, tlState.alignReferenceClipId, alignTargets(), tlState.timeSelection, close),
                         true);
    win->setUsingNativeTitleBar(true);
    win->centreAroundComponent(this, win->getWidth(), win->getHeight());
    win->setVisible(true);
    alignWindow = std::move(win);
}

void MainComponent::closeAlignDialog()
{
    alignWindow.reset();
    timeline.grabKeyboardFocus();
}


void MainComponent::requestQuit()
{
    // While something is loading its progress window is up: Cancel there first.
    if (promptActive || app.isLoading())
        return;
    auto go = [safe = juce::Component::SafePointer<MainComponent>(this)] {
        if (safe != nullptr)
            safe->confirmDiscard([] { juce::JUCEApplication::quit(); });
    };
    if (! app.isRecording())
    {
        transport.refresh();
        go();
        return;
    }
    // Stop here, not in the controller, so the user sees any message. The take is in the history before the save question.
    stopRecording([this, go](const juce::String& msg) {
        if (msg.isEmpty())
        {
            go();
            return;
        }
        promptActive = true; // a second quit request must not stack another prompt while this one is up
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                         .withIconType(juce::MessageBoxIconType::WarningIcon)
                                         .withTitle("Recording")
                                         .withMessage(msg)
                                         .withButton("OK")
                                         .withAssociatedComponent(this),
                                     [safe = juce::Component::SafePointer<MainComponent>(this), go](int) {
                                         if (safe == nullptr)
                                             return;
                                         safe->promptActive = false;
                                         go();
                                     });
    });
}

void MainComponent::confirmDiscard(std::function<void()> proceed)
{
    if (! app.history().isDirty())
    {
        proceed();
        return;
    }
    promptActive = true;
    juce::Component::SafePointer<MainComponent> safe(this);
    auto opts = juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::QuestionIcon)
                    .withTitle("Unsaved changes")
                    .withMessage("Do you want to save the changes to this project?")
                    .withButton("Save")
                    .withButton("Don't Save")
                    .withButton("Cancel")
                    .withAssociatedComponent(this);
    juce::AlertWindow::showAsync(opts, [safe, proceed](int r) {
        if (safe == nullptr)
            return;
        safe->promptActive = false;
        if (r == 1) // Save
            safe->saveThen(proceed);
        else if (r == 2) // Don't Save
            proceed();
        // else Cancel: nothing
    });
}

bool MainComponent::doSave(const juce::File& f)
{
    juce::String err;
    const bool ok = app.saveProject(f, err);
    if (! ok)
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not save project", err);
    updateTitle();
    return ok;
}

void MainComponent::saveThen(std::function<void()> proceed)
{
    if (app.currentFile() != juce::File())
    {
        if (doSave(app.currentFile()) && proceed)
            proceed();
        return;
    }
    saveAs(std::move(proceed));
}

void MainComponent::saveAs(std::function<void()> proceed)
{
    const auto start = app.currentFile() != juce::File()
                           ? app.currentFile()
                           : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Untitled.aligna");
    chooser = std::make_unique<juce::FileChooser>("Save project", start, "*.aligna");
    juce::Component::SafePointer<MainComponent> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
                         [safe, proceed](const juce::FileChooser& fc) {
                             if (safe == nullptr)
                                 return;
                             auto f = fc.getResult();
                             if (f != juce::File()
                                 && ! refuseWhileRecording(safe->app, "Could not save project", "Stop recording before saving."))
                             {
                                 const auto target = f.getParentDirectory().getChildFile(withExtension(f.getFileName(), ".aligna"));
                                 safe->confirmOverwrite(f, target, [safe, target, proceed] {
                                     if (safe->doSave(target) && proceed)
                                         proceed();
                                 });
                             }
                             safe->timeline.grabKeyboardFocus();
                         });
}

void MainComponent::confirmOverwrite(const juce::File& chosen, const juce::File& target, std::function<void()> go)
{
    // The chooser already asked when the typed name itself exists.
    if (target == chosen || ! target.exists())
    {
        go();
        return;
    }
    juce::Component::SafePointer<MainComponent> safe(this);
    auto opts = juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::QuestionIcon)
                    .withTitle("Replace existing file?")
                    .withMessage(target.getFileName() + " already exists. Do you want to replace it?")
                    .withButton("Overwrite")
                    .withButton("Cancel")
                    .withAssociatedComponent(this);
    juce::AlertWindow::showAsync(opts, [safe, go](int r) {
        if (safe != nullptr && r == 1)
            go();
    });
}

void MainComponent::doNew()
{
    auto* w = new juce::AlertWindow("New project", "Sample rate of the new project:", juce::MessageBoxIconType::NoIcon, this);
    w->addComboBox("rate", { "44100 Hz", "48000 Hz" });
    w->getComboBoxComponent("rate")->setSelectedItemIndex(1);
    w->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe(this);
    w->enterModalState(true,
                       juce::ModalCallbackFunction::create([safe, w](int r) {
                           if (r != 1 || safe == nullptr)
                               return;
                           const double rate = w->getComboBoxComponent("rate")->getSelectedItemIndex() == 0 ? 44100.0 : 48000.0;
                           safe->tlState.timeSelection.reset();
                           safe->closeAlignDialog();
                           safe->timeline.closeClipTempo();
                           safe->app.newProject(rate);
                           safe->timeline.grabKeyboardFocus();
                       }),
                       true);
}

void MainComponent::doOpen()
{
    chooser = std::make_unique<juce::FileChooser>("Open project", app.currentFile(), "*.aligna");
    juce::Component::SafePointer<MainComponent> safe(this);
    // Dirty here means the user has just chosen "Don't Save" for these changes.
    const bool discardConfirmed = app.history().isDirty();
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [safe, discardConfirmed](const juce::FileChooser& fc) {
                             if (safe == nullptr)
                                 return;
                             const auto f = fc.getResult();
                             if (f != juce::File()
                                 && ! refuseWhileRecording(safe->app, "Could not open project",
                                                           "Stop recording before opening a project."))
                             {
                                 auto open = [safe, f] {
                                     if (safe != nullptr)
                                         safe->openFile(f);
                                 };
                                 // Edited while the chooser was up (it is not modal everywhere): ask before dropping that.
                                 if (safe->app.history().isDirty() && ! discardConfirmed)
                                     safe->confirmDiscard(open);
                                 else
                                     open();
                             }
                             safe->timeline.grabKeyboardFocus();
                         });
}

void MainComponent::openFile(const juce::File& f)
{
    if (refuseWhileRecording(app, "Could not open project", "Stop recording before opening a project."))
        return;
    juce::String err;
    auto load = app.beginOpen(f, err); // reads and validates the project file; the audio comes next
    if (load == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not open project", err);
        return;
    }
    // Until the decoding is through, the current project stays as it is: a cancelled open changes nothing.
    startLoad(
        std::move(load), "Opening project...",
        [this](const juce::String&, bool cancelled) {
            if (! cancelled)
                reportUnloadedSources();
            updateTitle();
        },
        [this] {
            tlState.timeSelection.reset();
            closeAlignDialog();
            timeline.closeClipTempo();
        });
}

void MainComponent::reportUnloadedSources()
{
    std::vector<juce::String> paths, lines;
    bool undecodable = false;
    for (auto& s : app.history().current().sources)
    {
        if (! s.missing)
            continue;
        const auto reason = app.sourceProblem(s.id);
        undecodable = undecodable || reason.isNotEmpty();
        paths.push_back(s.path);
        lines.push_back(s.path + juce::String::fromUTF8(" \xe2\x80\x94 ") + (reason.isNotEmpty() ? reason : juce::String("File not found")));
    }
    if (paths.empty())
        return;
    if (! undecodable)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Missing audio files",
                                               missingFilesMessage(paths));
        return;
    }
    // Some files are there but could not be decoded: say why, per file.
    constexpr size_t cap = 20; // as missingFilesMessage
    juce::String m = "These audio files could not be loaded:";
    for (size_t i = 0; i < lines.size() && i < cap; ++i)
        m += "\n" + lines[i];
    if (lines.size() > cap)
        m += "\n" + juce::String::fromUTF8("\xe2\x80\xa6") + "and " + juce::String((int) (lines.size() - cap)) + " more";
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Missing audio files", m);
}

void MainComponent::doExport()
{
    if (projectEnd(app.history().current()) <= 0)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Export mix", "Nothing to export.");
        return;
    }
    auto* w = new juce::AlertWindow("Export mix", "Format of the exported file:", juce::MessageBoxIconType::NoIcon, this);
    w->addComboBox("format", { "WAV 16-bit", "WAV 24-bit", "MP3" });
    auto* formatBox = w->getComboBoxComponent("format");
    formatBox->setSelectedItemIndex(lastExport.format == ExportSettings::Format::Wav16   ? 0
                                    : lastExport.format == ExportSettings::Format::Wav24 ? 1
                                                                                          : 2);
    w->addComboBox("bitrate", { "192 kbps", "256 kbps", "320 kbps" }, "MP3 bitrate:");
    auto* bitrateBox = w->getComboBoxComponent("bitrate");
    bitrateBox->setSelectedItemIndex(lastExport.kbps == 192 ? 0 : lastExport.kbps == 256 ? 1 : 2);
    // The bitrate only applies to MP3.
    formatBox->onChange = [formatBox, bitrateBox] { bitrateBox->setEnabled(formatBox->getSelectedItemIndex() == 2); };
    formatBox->onChange();
    w->addButton("Export...", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe(this);
    w->enterModalState(true,
                       juce::ModalCallbackFunction::create([safe, w](int r) {
                           if (r != 1 || safe == nullptr)
                               return;
                           auto& self = *safe;
                           ExportSettings settings;
                           const int fi = w->getComboBoxComponent("format")->getSelectedItemIndex();
                           settings.format = fi == 0 ? ExportSettings::Format::Wav16
                                             : fi == 1 ? ExportSettings::Format::Wav24
                                                       : ExportSettings::Format::Mp3;
                           const int bi = w->getComboBoxComponent("bitrate")->getSelectedItemIndex();
                           settings.kbps = bi == 0 ? 192 : bi == 1 ? 256 : 320;
                           self.lastExport.format = settings.format;
                           self.lastExport.kbps = settings.kbps;
                           const auto ext = extensionFor(settings.format);
                           const auto projectFile = self.app.currentFile();
                           settings.title = projectFile != juce::File() ? projectFile.getFileNameWithoutExtension() : juce::String("Untitled");
                           auto base = projectFile != juce::File()
                                           ? projectFile.getSiblingFile(settings.title + " mix" + ext)
                                           : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                 .getChildFile("Untitled mix" + ext);
                           self.chooser = std::make_unique<juce::FileChooser>("Export mix", base, "*" + ext);
                           self.chooser->launchAsync(
                               juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                   | juce::FileBrowserComponent::warnAboutOverwriting,
                               [safe, settings, ext](const juce::FileChooser& fc) {
                                   if (safe == nullptr)
                                       return;
                                   auto f = fc.getResult();
                                   if (f != juce::File()
                                       && refuseWhileRecording(safe->app, "Export failed", "Stop recording before exporting."))
                                   {
                                       safe->timeline.grabKeyboardFocus();
                                       return;
                                   }
                                   if (f != juce::File())
                                   {
                                       const auto target = f.getParentDirectory().getChildFile(exportFileName(f.getFileName(), ext));
                                       if (isProjectSource(safe->app.history().current(), target))
                                       {
                                           juce::AlertWindow::showMessageBoxAsync(
                                               juce::MessageBoxIconType::WarningIcon, "Export failed",
                                               "Can't export over one of the project's source audio files.");
                                           safe->timeline.grabKeyboardFocus();
                                           return;
                                       }
                                       safe->confirmOverwrite(f, target, [safe, settings, target] { safe->startExport(settings, target); });
                                   }
                                   else
                                       safe->timeline.grabKeyboardFocus();
                               });
                       }),
                       true);
}

namespace
{
class ExportTask : public juce::ThreadWithProgressWindow
{
public:
    ExportTask(std::shared_ptr<const MixSnapshot> s, juce::File f, ExportSettings d, std::function<void(bool, juce::String)> done,
               juce::Component* around)
        : ThreadWithProgressWindow("Exporting mix...", true, true, 10000, {}, around),
          snap(std::move(s)), file(std::move(f)), settings(std::move(d)), onDone(std::move(done))
    {
    }
    void run() override
    {
        ok = exportMix(*snap, file, settings, error, [this](float p) {
            setProgress(p);
            return ! threadShouldExit();
        });
    }
    void threadComplete(bool) override { onDone(ok, error); }

private:
    std::shared_ptr<const MixSnapshot> snap;
    juce::File file;
    ExportSettings settings;
    std::function<void(bool, juce::String)> onDone;
    bool ok = false;
    juce::String error;
};
} // namespace

void MainComponent::startExport(const ExportSettings& settings, const juce::File& f, bool allowSilentClips)
{
    if (app.isLoading()) // its progress window is up; the project is about to change
        return;
    if (const int n = countStretching(app.history().current(), app.pool(), app.stretchCache()); n > 0)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Export",
            juce::String(n) + (n == 1 ? " clip is" : " clips are") + " still stretching "
                + juce::String::fromUTF8("\xe2\x80\x94") + " export again in a moment.");
        timeline.grabKeyboardFocus();
        return;
    }
    // A clip whose stretch failed (out of memory) has no audio: say so rather than export it silent unasked.
    if (const int n = countStretchFailed(app.history().current(), app.pool(), app.stretchCache()); n > 0 && ! allowSilentClips)
    {
        juce::Component::SafePointer<MainComponent> safe(this);
        auto opts = juce::MessageBoxOptions()
                        .withIconType(juce::MessageBoxIconType::WarningIcon)
                        .withTitle("Export")
                        .withMessage(juce::String(n) + (n == 1 ? " clip" : " clips")
                                     + " could not be stretched (probably not enough memory) and will be silent in the export.")
                        .withButton("Export Anyway")
                        .withButton("Cancel")
                        .withAssociatedComponent(this);
        juce::AlertWindow::showAsync(opts, [safe, settings, f](int r) {
            if (safe == nullptr)
                return;
            if (r == 1)
                safe->startExport(settings, f, true);
            else
                safe->timeline.grabKeyboardFocus();
        });
        return;
    }
    // Snapshot on the message thread; the worker only reads it.
    auto snap = makeSnapshot(app.history().current(), app.pool(), app.stretchCache());
    juce::Component::SafePointer<MainComponent> safe(this);
    exportTask = std::make_unique<ExportTask>(
        std::move(snap), f, settings,
        [safe](bool ok, juce::String err) {
            juce::MessageManager::callAsync([safe, ok, err] {
                if (safe == nullptr)
                    return;
                safe->exportTask.reset();
                if (! ok && err != "Export cancelled")
                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export failed", err);
                safe->timeline.grabKeyboardFocus();
            });
        },
        this);
    static_cast<ExportTask*>(exportTask.get())->launchThread();
}

void MainComponent::runFileCommand(int id)
{
    if (app.isLoading() || (app.isRecording() && id != menuQuit))
        return;
    switch (id)
    {
        case menuNew: confirmDiscard([this] { doNew(); }); break;
        case menuOpen: confirmDiscard([this] { doOpen(); }); break;
        case menuSave: saveThen({}); break;
        case menuSaveAs: saveAs({}); break;
        case menuImport: chooseImport(); break;
        case menuExport: doExport(); break;
        case menuQuit: requestQuit(); break;
        default: break;
    }
}

void MainComponent::undo()
{
    if (! busy())
        app.history().undo();
}
void MainComponent::redo()
{
    if (! busy())
        app.history().redo();
}

juce::StringArray MainComponent::getMenuBarNames() { return { "File", "Edit", "Help" }; }

juce::PopupMenu MainComponent::fileMenu()
{
#if JUCE_MAC
    const juce::String mod = "Cmd+";
#else
    const juce::String mod = "Ctrl+";
#endif
    juce::PopupMenu m;
    auto add = [&](int id, const juce::String& name, const juce::String& key) {
        juce::PopupMenu::Item it(name);
        it.itemID = id;
        it.isEnabled = ! app.isLoading() && (! app.isRecording() || id == menuQuit);
        if (key.isNotEmpty())
            it.shortcutKeyDescription = mod + key;
        m.addItem(it);
    };
    add(menuNew, "New...", "N");
    add(menuOpen, "Open...", "O");
    add(menuSave, "Save", "S");
    add(menuSaveAs, "Save As...", "Shift+S");
    m.addSeparator();
    add(menuImport, "Import Audio...", "I");
    add(menuExport, "Export Mix...", "E");
#if ! JUCE_MAC
    m.addSeparator();
    add(menuQuit, "Quit", "Q");
#endif
    return m;
}

juce::PopupMenu MainComponent::editMenu()
{
    auto& h = app.history();
#if JUCE_MAC
    const char* mod = "Cmd+";
#else
    const char* mod = "Ctrl+";
#endif
    juce::PopupMenu m;
    juce::PopupMenu::Item u(h.canUndo() ? "Undo " + h.undoName() : juce::String("Undo"));
    u.itemID = menuUndo;
    u.isEnabled = h.canUndo() && ! busy();
    u.shortcutKeyDescription = juce::String(mod) + "Z";
    m.addItem(u);
    juce::PopupMenu::Item r(h.canRedo() ? "Redo " + h.redoName() : juce::String("Redo"));
    r.itemID = menuRedo;
    r.isEnabled = h.canRedo() && ! busy();
    r.shortcutKeyDescription = juce::String(mod) + "Shift+Z";
    m.addItem(r);
    m.addSeparator();
    juce::PopupMenu::Item al("Align to Reference");
    al.itemID = menuAlign;
    al.isEnabled = canAlign() && ! busy();
    al.shortcutKeyDescription = juce::String(mod) + "L";
    m.addItem(al);
    m.addSeparator();
    const bool selection = ! tlState.selectedClipIds.empty() && ! busy();
    juce::PopupMenu::Item sp("Split at Playhead");
    sp.itemID = menuSplit;
    sp.isEnabled = selection;
    sp.shortcutKeyDescription = "S";
    m.addItem(sp);
    juce::PopupMenu::Item del("Delete");
    del.itemID = menuDelete;
    del.isEnabled = selection;
    del.shortcutKeyDescription = "Del";
    m.addItem(del);
    return m;
}

juce::PopupMenu MainComponent::helpMenu()
{
    juce::PopupMenu m;
    juce::PopupMenu::Item up("Check for Updates...");
    up.itemID = menuCheckUpdates;
    up.isEnabled = ! busy() && ! updater.isBusy();
    m.addItem(up);
    return m;
}

juce::PopupMenu MainComponent::getMenuForIndex(int, const juce::String& name)
{
    if (name == "File")
        return fileMenu();
    if (name == "Edit")
        return editMenu();
    if (name == "Help")
        return helpMenu();
    return {};
}

void MainComponent::menuItemSelected(int id, int)
{
    if (id == menuUndo)
        undo();
    else if (id == menuRedo)
        redo();
    else if (id == menuAlign)
    {
        if (! busy())
            openAlignDialog();
    }
    else if (id == menuSplit || id == menuDelete)
    {
        if (! busy())
        {
            if (id == menuSplit)
                timeline.splitSelected();
            else
                timeline.deleteSelected();
        }
    }
    else if (id == menuCheckUpdates)
    {
        if (! busy())
            updater.check();
    }
    else
        runFileCommand(id);
    if (alignWindow == nullptr)
        timeline.grabKeyboardFocus();
}

bool MainComponent::keyPressed(const juce::KeyPress& k)
{
    if (app.isLoading())
        return false; // nothing while audio is being loaded: the progress window has the keyboard
    if (k == juce::KeyPress::spaceKey)
    {
        transport.togglePlay();
        return true;
    }
    const auto mods = k.getModifiers();
    const bool cmd = mods.isCommandDown() || mods.isCtrlDown();
    const auto code = juce::CharacterFunctions::toUpperCase((juce::juce_wchar) k.getKeyCode()); // may arrive lowercase
    if (code == 'R' && ! cmd && ! mods.isAltDown() && ! mods.isShiftDown())
    {
        transport.toggleRecord();
        return true;
    }
    if (app.isRecording())
    {
#if ! JUCE_MAC
        if (cmd && code == 'Q')
        {
            runFileCommand(menuQuit);
            return true;
        }
#endif
        return false; // nothing else while recording
    }
    if (cmd && code == 'Z')
    {
        if (mods.isShiftDown())
            redo();
        else
            undo();
        return true;
    }
    if (cmd)
    {
        int id = 0;
        switch (code)
        {
            case 'N': id = menuNew; break;
            case 'O': id = menuOpen; break;
            case 'S': id = mods.isShiftDown() ? menuSaveAs : menuSave; break;
            case 'I': id = menuImport; break;
            case 'E': id = menuExport; break;
#if ! JUCE_MAC
            case 'Q': id = menuQuit; break;
#endif
            default: break;
        }
        if (id != 0)
        {
            runFileCommand(id);
            return true;
        }
    }
    if (cmd && code == 'L')
    {
        if (canAlign())
            openAlignDialog();
        return true;
    }
#if ! JUCE_MAC
    if (cmd && code == 'Y')
    {
        redo();
        return true;
    }
#endif
    return false;
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1f22));
    // Corner above the headers, level with the ruler.
    g.setColour(juce::Colour(0xff26282c));
    g.fillRect(0, transport.getBottom(), TrackHeader::width, TimelineView::rulerHeight);
    if (app.history().current().tracks.empty())
    {
        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.setFont(juce::FontOptions(16.0f));
        g.drawText("Import audio files to start aligning", getLocalBounds().withTrimmedTop(transport.getBottom()),
                   juce::Justification::centred);
    }
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    if (menuBar != nullptr)
        menuBar->setBounds(r.removeFromTop(menuBarHeight));
    transport.setBounds(r.removeFromTop(48));
    auto left = r.removeFromLeft(TrackHeader::width);
    left.removeFromTop(TimelineView::rulerHeight);
    headerColumn.setBounds(left);
    timeline.setBounds(r);
    layoutHeaders();
}
}
