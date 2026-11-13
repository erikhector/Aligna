#pragma once

#include "app/AppController.h"
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

    // MenuBarModel. Menus are listed in menuNames(); Task 13 adds "File" there and in menuFor()/menuItemSelected().
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    TimelineState& timelineState() { return tlState; }

private:
    enum MenuIds { menuUndo = 1, menuRedo };
    static constexpr int menuBarHeight = 24;
    juce::PopupMenu editMenu();
    void importAndReport(const juce::Array<juce::File>&);
    void undo();
    void redo();
    void chooseImport();
    void projectChanged();
    void layoutHeaders();

    AppController& app;
    TimelineState tlState;
    TransportBar transport;
    TimelineView timeline;
    juce::Component headerColumn; // clips its children; headers scroll vertically with the lanes
    std::vector<std::unique_ptr<TrackHeader>> headers;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::MenuBarComponent> menuBar; // not used on macOS (native menu bar)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
}
