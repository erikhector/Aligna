#include "ui/MainComponent.h"

#include "engine/Mixer.h"
#include "io/FileNaming.h"
#include <set>

namespace aligna
{
MainComponent::MainComponent(AppController& a) : app(a), transport(a, tlState), timeline(a, tlState)
{
    addAndMakeVisible(transport);
    addAndMakeVisible(timeline);
    addAndMakeVisible(headerColumn);
    headerColumn.setInterceptsMouseClicks(false, true);
    timeline.onViewChanged = [this] { layoutHeaders(); };
    app.onProjectChanged = [this] { projectChanged(); };
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
}

void MainComponent::closeDialogs()
{
    exportTask.reset(); // stops the export thread
    alignWindow.reset(); // stops the align worker thread
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
        headers[i]->update(tracks[i]);

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

void MainComponent::chooseImport()
{
    chooser = std::make_unique<juce::FileChooser>("Import audio", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems
                             | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& fc) {
                             importAndReport(fc.getResults());
                             timeline.grabKeyboardFocus();
                         });
}

void MainComponent::importAndReport(const juce::Array<juce::File>& files)
{
    juce::String errors;
    app.importFiles(files, errors);
    if (errors.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Import problems", errors);
    repaint();
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
    if (promptActive)
        return;
    confirmDiscard([] { juce::JUCEApplication::quit(); });
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
                             if (f != juce::File())
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
                           safe->app.newProject(rate);
                           safe->timeline.grabKeyboardFocus();
                       }),
                       true);
}

void MainComponent::doOpen()
{
    chooser = std::make_unique<juce::FileChooser>("Open project", app.currentFile(), "*.aligna");
    juce::Component::SafePointer<MainComponent> safe(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [safe](const juce::FileChooser& fc) {
                             if (safe == nullptr)
                                 return;
                             const auto f = fc.getResult();
                             if (f != juce::File())
                             {
                                 juce::String err;
                                 safe->tlState.timeSelection.reset();
                                 safe->closeAlignDialog();
                                 if (! safe->app.openProject(f, err))
                                     juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                            "Could not open project", err);
                                 else
                                 {
                                     std::vector<juce::String> missing;
                                     for (auto& s : safe->app.history().current().sources)
                                         if (s.missing)
                                             missing.push_back(s.path);
                                     if (! missing.empty())
                                         juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                                "Missing audio files",
                                                                                missingFilesMessage(missing));
                                 }
                                 safe->updateTitle();
                             }
                             safe->timeline.grabKeyboardFocus();
                         });
}

void MainComponent::doExport()
{
    if (projectEnd(app.history().current()) <= 0)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Export mix", "Nothing to export.");
        return;
    }
    auto* w = new juce::AlertWindow("Export mix", "Bit depth of the exported WAV file:", juce::MessageBoxIconType::NoIcon, this);
    w->addComboBox("depth", { "16-bit", "24-bit" });
    w->getComboBoxComponent("depth")->setSelectedItemIndex(1);
    w->addButton("Export...", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> safe(this);
    w->enterModalState(true,
                       juce::ModalCallbackFunction::create([safe, w](int r) {
                           if (r != 1 || safe == nullptr)
                               return;
                           const int depth = w->getComboBoxComponent("depth")->getSelectedItemIndex() == 0 ? 16 : 24;
                           auto& self = *safe;
                           auto base = self.app.currentFile() != juce::File()
                                           ? self.app.currentFile().getSiblingFile(self.app.currentFile().getFileNameWithoutExtension() + " mix.wav")
                                           : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                 .getChildFile("Untitled mix.wav");
                           self.chooser = std::make_unique<juce::FileChooser>("Export mix", base, "*.wav");
                           self.chooser->launchAsync(
                               juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                   | juce::FileBrowserComponent::warnAboutOverwriting,
                               [safe, depth](const juce::FileChooser& fc) {
                                   if (safe == nullptr)
                                       return;
                                   auto f = fc.getResult();
                                   if (f != juce::File())
                                   {
                                       const auto target = f.getParentDirectory().getChildFile(withExtension(f.getFileName(), ".wav"));
                                       if (isProjectSource(safe->app.history().current(), target))
                                       {
                                           juce::AlertWindow::showMessageBoxAsync(
                                               juce::MessageBoxIconType::WarningIcon, "Export failed",
                                               "Can't export over one of the project's source audio files.");
                                           safe->timeline.grabKeyboardFocus();
                                           return;
                                       }
                                       safe->confirmOverwrite(f, target, [safe, depth, target] { safe->startExport(depth, target); });
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
    ExportTask(std::shared_ptr<const MixSnapshot> s, juce::File f, int d, std::function<void(bool, juce::String)> done,
               juce::Component* around)
        : ThreadWithProgressWindow("Exporting mix...", true, true, 10000, {}, around),
          snap(std::move(s)), file(std::move(f)), depth(d), onDone(std::move(done))
    {
    }
    void run() override
    {
        ok = exportMix(*snap, file, depth, error, [this](float p) {
            setProgress(p);
            return ! threadShouldExit();
        });
    }
    void threadComplete(bool) override { onDone(ok, error); }

private:
    std::shared_ptr<const MixSnapshot> snap;
    juce::File file;
    int depth;
    std::function<void(bool, juce::String)> onDone;
    bool ok = false;
    juce::String error;
};
} // namespace

void MainComponent::startExport(int bitDepth, const juce::File& f)
{
    if (const int n = countStretching(app.history().current(), app.pool(), app.stretchCache()); n > 0)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon, "Export",
            juce::String(n) + (n == 1 ? " clip is" : " clips are") + " still stretching "
                + juce::String::fromUTF8("\xe2\x80\x94") + " export again in a moment.");
        timeline.grabKeyboardFocus();
        return;
    }
    // Snapshot on the message thread; the worker only reads it.
    auto snap = makeSnapshot(app.history().current(), app.pool(), app.stretchCache());
    juce::Component::SafePointer<MainComponent> safe(this);
    exportTask = std::make_unique<ExportTask>(
        std::move(snap), f, bitDepth,
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

void MainComponent::undo() { app.history().undo(); }
void MainComponent::redo() { app.history().redo(); }

juce::StringArray MainComponent::getMenuBarNames() { return { "File", "Edit" }; }

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
    u.isEnabled = h.canUndo();
    u.shortcutKeyDescription = juce::String(mod) + "Z";
    m.addItem(u);
    juce::PopupMenu::Item r(h.canRedo() ? "Redo " + h.redoName() : juce::String("Redo"));
    r.itemID = menuRedo;
    r.isEnabled = h.canRedo();
    r.shortcutKeyDescription = juce::String(mod) + "Shift+Z";
    m.addItem(r);
    m.addSeparator();
    juce::PopupMenu::Item al("Align to Reference");
    al.itemID = menuAlign;
    al.isEnabled = canAlign();
    al.shortcutKeyDescription = juce::String(mod) + "L";
    m.addItem(al);
    return m;
}

juce::PopupMenu MainComponent::getMenuForIndex(int, const juce::String& name)
{
    if (name == "File")
        return fileMenu();
    if (name == "Edit")
        return editMenu();
    return {};
}

void MainComponent::menuItemSelected(int id, int)
{
    if (id == menuUndo)
        undo();
    else if (id == menuRedo)
        redo();
    else if (id == menuAlign)
        openAlignDialog();
    else
        runFileCommand(id);
    if (alignWindow == nullptr)
        timeline.grabKeyboardFocus();
}

bool MainComponent::keyPressed(const juce::KeyPress& k)
{
    if (k == juce::KeyPress::spaceKey)
    {
        transport.togglePlay();
        return true;
    }
    const auto mods = k.getModifiers();
    const bool cmd = mods.isCommandDown() || mods.isCtrlDown();
    const auto code = juce::CharacterFunctions::toUpperCase((juce::juce_wchar) k.getKeyCode()); // may arrive lowercase
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
