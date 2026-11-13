#pragma once

#include "app/AppController.h"
#include "ui/AlignDialog.h"
#include "ui/TimelineState.h"
#include "ui/TimelineView.h"
#include "ui/TrackHeader.h"
#include "ui/TransportBar.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace aligna
{
class MainComponent : public juce::Component, public juce::MenuBarModel, public juce::FileDragAndDropTarget
{
public:
    explicit MainComponent(AppController&);
    ~MainComponent() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void parentHierarchyChanged() override;

    // Asks about unsaved changes (async), then quits the application. Safe to call repeatedly.
    void requestQuit();
    // Closes every dialog this window owns (Audio settings, Align, export progress). Called before shutdown.
    void closeDialogs();

    // MenuBarModel. Menus: File, Edit.
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    TimelineState& timelineState() { return tlState; }

private:
    enum MenuIds { menuUndo = 1, menuRedo, menuAlign, menuNew, menuOpen, menuSave, menuSaveAs, menuImport, menuExport, menuQuit };
    static constexpr int menuBarHeight = 24;
    juce::PopupMenu fileMenu();
    juce::PopupMenu editMenu();
    void importAndReport(const juce::Array<juce::File>&);
    void undo();
    void redo();
    void chooseImport();
    void updateTitle();
    void confirmDiscard(std::function<void()> proceed); // proceeds at once if clean; else Save / Don't Save / Cancel
    void saveThen(std::function<void()> proceed);       // Save (or Save As); proceeds only on success
    void saveAs(std::function<void()> proceed);
    bool doSave(const juce::File&);
    void runFileCommand(int menuId);
    // If `target` differs from what the chooser returned and exists, asks Overwrite / Cancel first.
    void confirmOverwrite(const juce::File& chosen, const juce::File& target, std::function<void()> go);
    void doNew();
    void doOpen();
    void doExport();
    void startExport(int bitDepth, const juce::File&);
    void projectChanged();
    void layoutHeaders();
    std::vector<juce::String> alignTargets() const; // selected clips other than the reference, project order
    bool canAlign() const;
    void openAlignDialog();
    void closeAlignDialog();

    AppController& app;
    TimelineState tlState;
    TransportBar transport;
    TimelineView timeline;
    juce::Component headerColumn; // clips its children; headers scroll vertically with the lanes
    std::vector<std::unique_ptr<TrackHeader>> headers;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::ThreadWithProgressWindow> exportTask;
    bool promptActive = false;
    std::unique_ptr<juce::DialogWindow> alignWindow; // at most one, non-modal
    std::unique_ptr<juce::MenuBarComponent> menuBar; // not used on macOS (native menu bar)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
}
