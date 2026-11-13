#include "ui/MainComponent.h"

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
    transport.importButton.onClick = [this] { chooseImport(); };
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
    alignWindow.reset(); // stops the worker thread before anything else goes away
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    app.onProjectChanged = nullptr;
}

void MainComponent::projectChanged()
{
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

void MainComponent::undo() { app.history().undo(); }
void MainComponent::redo() { app.history().redo(); }

juce::StringArray MainComponent::getMenuBarNames() { return { "Edit" }; }

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
