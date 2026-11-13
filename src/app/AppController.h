#pragma once
#include "edit/EditHistory.h"
#include "engine/PlaybackChain.h"
#include "engine/SourcePool.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace aligna
{
class AppController
{
public:
    AppController();
    ~AppController();

    EditHistory& history() { return hist; }
    SourcePool& pool() { return sources; }
    PlaybackEngine& engine() { return playback; }
    juce::AudioDeviceManager& devices() { return deviceManager; }

    // One new track per file, as a single undoable "Import". Per-file errors go to `errors`, one line each.
    void importFiles(const juce::Array<juce::File>& files, juce::String& errors);
    void newProject();
    bool openProject(const juce::File& file, juce::String& error);
    bool saveProject(const juce::File& file, juce::String& error);
    juce::File currentFile() const { return file; }

    // Fired (message thread) after every project change, once the playback snapshot has been rebuilt.
    std::function<void()> onProjectChanged;

private:
    void rebuildSnapshot();
    void syncProjectRate();

    EditHistory hist { Project {} };
    SourcePool sources;
    PlaybackEngine playback;
    PlaybackChain chain { playback };
    juce::AudioDeviceManager deviceManager;
    juce::File file;
};
} // namespace aligna
