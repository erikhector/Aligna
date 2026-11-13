#include "ui/MainComponent.h"

namespace aligna
{
MainComponent::MainComponent(AppController& a) : app(a), transport(a), timeline(a, tlState)
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
    setSize(1200, 800);
}

MainComponent::~MainComponent() { app.onProjectChanged = nullptr; }

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
                             juce::String errors;
                             app.importFiles(fc.getResults(), errors);
                             if (errors.isNotEmpty())
                                 juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                        "Import problems", errors);
                             repaint();
                         });
}

bool MainComponent::keyPressed(const juce::KeyPress& k)
{
    if (k == juce::KeyPress::spaceKey)
    {
        transport.togglePlay();
        return true;
    }
    return false;
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1f22));
    // Corner above the headers, level with the ruler.
    g.setColour(juce::Colour(0xff26282c));
    g.fillRect(0, 48, TrackHeader::width, TimelineView::rulerHeight);
    if (app.history().current().tracks.empty())
    {
        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.setFont(juce::FontOptions(16.0f));
        g.drawText("Import audio files to start aligning", getLocalBounds().withTrimmedTop(48),
                   juce::Justification::centred);
    }
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    transport.setBounds(r.removeFromTop(48));
    auto left = r.removeFromLeft(TrackHeader::width);
    left.removeFromTop(TimelineView::rulerHeight);
    headerColumn.setBounds(left);
    timeline.setBounds(r);
    layoutHeaders();
}
}
