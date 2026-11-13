#pragma once
#include "edit/EditHistory.h"
#include "engine/PlaybackEngine.h"
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

private:
    class DeviceCallback : public juce::AudioIODeviceCallback
    {
    public:
        explicit DeviceCallback(AppController& o) : owner(o) {}
        void audioDeviceAboutToStart(juce::AudioIODevice*) override;
        void audioDeviceStopped() override;
        void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int, int,
                                              const juce::AudioIODeviceCallbackContext&) override;

    private:
        AppController& owner;
    };

    void rebuildSnapshot();
    void syncProjectRate();

    EditHistory hist { Project {} };
    SourcePool sources;
    PlaybackEngine playback;
    juce::ResamplingAudioSource resampler { &playback, false, 2 };
    juce::AudioSourcePlayer player;
    juce::AudioDeviceManager deviceManager;
    DeviceCallback callback { *this };
    std::atomic<double> projectRate { 48000.0 };
    juce::File file;
};
} // namespace aligna
