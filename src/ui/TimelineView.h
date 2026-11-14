#pragma once
#include "app/AppController.h"
#include "edit/DragMath.h"
#include "ui/TimelineMapping.h"
#include "ui/TimelineState.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>

namespace aligna
{
class TimelineView : public juce::Component, private juce::Timer
{
public:
    static constexpr int rulerHeight = 32;
    static constexpr int trackHeight = 100;

    TimelineView(AppController&, TimelineState&);
    ~TimelineView() override;

    // Drops any drag in progress and closes the clip-tempo dialog (its OK would be refused while recording).
    void recordingStarted()
    {
        cancelDrag();
        draggingRuler = selecting = false;
        tempoWindow.reset();
    }
    void projectChanged(); // call after the project/pool changed
    TimelineMapping& mapping() { return map; }
    int getViewY() const { return viewY; }
    void setViewY(int y);
    std::function<void()> onViewChanged; // fired when vertical scroll changes (headers follow)

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void resized() override;

private:
    enum class DragMode { None, Move, TrimStart, TrimEnd, Stretch, FadeIn, FadeOut };
    struct Drag
    {
        DragMode mode = DragMode::None;
        bool active = false; // moved past the click threshold
        juce::Point<int> down;
        juce::String clipId; // the clip under the mouse
        std::vector<juce::String> ids;
        std::vector<SamplePos> starts;
        std::vector<int> tracks;
        size_t anchor = 0;
        SamplePos origEdge = 0; // trim edge / fade length at mouse-down
        SamplePos origOther = 0; // the other fade length
        SamplePos timeDelta = 0;
        int trackDelta = 0;
        SamplePos value = 0; // new trim edge / fade length
        bool collapseOnUp = false;
        juce::String name;
        bool changed() const;
        bool apply(Project&) const;
    };

    struct Thumb
    {
        AudioData data; // keeps the buffer alive; pointer identity detects replacement
        std::unique_ptr<juce::AudioThumbnail> thumb;
    };

    void timerCallback() override;
    void syncThumbnails();
    void clampView();
    int maxViewY() const;
    void repaintColumn(double x);
    void setPlayhead(SamplePos);
    const Project& shown() const { return preview ? *preview : app.history().current(); }
    void cancelDrag();
    void updateDrag(const juce::MouseEvent&);
    void showGainPopup(const juce::MouseEvent&, const juce::String& clipId);
    bool nudgeSelected(bool right, const juce::ModifierKeys&);
    bool splitSelected();
    bool deleteSelected();
    void paintRuler(juce::Graphics&, juce::Rectangle<int> clip);
    void paintTrack(juce::Graphics&, const Track&, int index, juce::Rectangle<int> clip);
    void showClipTempo(const juce::String& clipId);
    void paintClip(juce::Graphics&, const Clip&, juce::Rectangle<int> laneRect, juce::Rectangle<int> clip, float alpha,
                   juce::Colour colour, bool missing, bool stretching);
    void paintWaveform(juce::Graphics&, const Clip&, juce::Rectangle<int> r, juce::Rectangle<int> clip, float alpha,
                       juce::Colour colour);

    AppController& app;
    TimelineState& state;
    TimelineMapping map;
    int viewY = 0;

    juce::AudioFormatManager formats; // no formats registered; thumbnails are filled from memory
    juce::AudioThumbnailCache thumbCache { 64 };
    std::map<juce::String, Thumb> thumbs;

    Drag drag;
    std::optional<Project> preview; // live drag preview; history is untouched until mouse-up
    std::optional<Project> dragBase; // project at mouse-down; a drag survives changes that leave the clips alone
    std::unique_ptr<juce::DialogWindow> tempoWindow;
    double mouseX = -1, mouseY = -1;
    double lastPlayheadX = -1;
    bool draggingRuler = false, selecting = false;
    SamplePos selectAnchor = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineView)
};
} // namespace aligna
