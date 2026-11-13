#pragma once
#include "app/AppController.h"
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

    void projectChanged(); // call after the project/pool changed
    TimelineMapping& mapping() { return map; }
    int getViewY() const { return viewY; }
    void setViewY(int y);
    std::function<void()> onViewChanged; // fired when vertical scroll changes (headers follow)

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void resized() override;

private:
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
    void paintRuler(juce::Graphics&, juce::Rectangle<int> clip);
    void paintTrack(juce::Graphics&, const Track&, int index, juce::Rectangle<int> clip);
    void paintClip(juce::Graphics&, const Clip&, juce::Rectangle<int> laneRect, juce::Rectangle<int> clip, float alpha,
                   juce::Colour colour, bool missing);
    void paintWaveform(juce::Graphics&, const Clip&, juce::Rectangle<int> r, juce::Rectangle<int> clip, float alpha,
                       juce::Colour colour);

    AppController& app;
    TimelineState& state;
    TimelineMapping map;
    int viewY = 0;

    juce::AudioFormatManager formats; // no formats registered; thumbnails are filled from memory
    juce::AudioThumbnailCache thumbCache { 64 };
    std::map<juce::String, Thumb> thumbs;

    double mouseX = -1;
    double lastPlayheadX = -1;
    bool draggingRuler = false, selecting = false;
    SamplePos selectAnchor = 0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineView)
};
} // namespace aligna
