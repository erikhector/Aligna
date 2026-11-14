#pragma once
#include "app/AppSettings.h"
#include "edit/EditHistory.h"
#include "edit/RecordTiming.h"
#include "engine/PlaybackChain.h"
#include "engine/Recorder.h"
#include "engine/SourcePool.h"
#include "engine/StretchCache.h"
#include <juce_audio_utils/juce_audio_utils.h>

namespace aligna
{
class AppController : private juce::Timer
{
public:
    AppController();
    ~AppController() override;

    EditHistory& history() { return hist; }
    SourcePool& pool() { return sources; }
    StretchCache& stretchCache() { return stretched; }
    PlaybackEngine& engine() { return playback; }
    juce::AudioDeviceManager& devices() { return deviceManager; }

    // One new track per file, as a single undoable "Import". Per-file errors go to `errors`, one line each.
    void importFiles(const juce::Array<juce::File>& files, juce::String& errors);
    void newProject(double sampleRate = 48000); // fixed for the project's lifetime
    bool openProject(const juce::File& file, juce::String& error);
    bool saveProject(const juce::File& file, juce::String& error);
    juce::File currentFile() const { return file; }

    AppSettings& settings() { return appSettings; }

    // Recording (message thread). While recording, the history is locked and the transport must not be moved.
    bool isRecording() const { return recording; }
    SamplePos recordingStart() const { return recordStart; } // valid while recording
    float inputPeak() { return recorder.getAndResetPeak(); }
    // done(error): empty error = started. Called later, after the microphone permission check (which may prompt).
    // Already recording or a check pending: done({}) at once.
    void startRecording(std::function<void(juce::String error)> done);
    // Stops, keeps the take file and adds the take as one "Record" step. Returns a message for the user, empty
    // if all went well. Not recording: returns {}.
    juce::String stopRecording();
    // Fired (message thread) when recording stopped by itself (device failure, failed disk write, disk full,
    // take length limit); the take is kept.
    std::function<void(juce::String message)> onRecordingStopped;

    // Fired (message thread) after every project change, once the playback snapshot has been rebuilt,
    // and again whenever a stretched buffer finishes rendering.
    std::function<void()> onProjectChanged;

private:
    void projectChanged(); // requires the project's stretched buffers, then rebuilds the snapshot
    void rebuildSnapshot();
    void syncProjectRate();
    // Stops playback and drops every buffer of the outgoing project: empty snapshot, pool and stretch cache.
    void releaseProjectAudio();
    void beginRecording(const std::function<void(juce::String)>& done); // inputs are enabled
    int activeInputChannels() const;
    bool enableFirstInput();
    // Frees retired playback snapshots; polls a running recording for failure, free disk space and length.
    // Runs while recording or while a retired snapshot is waiting to be freed.
    void timerCallback() override;

    EditHistory hist { Project {} };
    SourcePool sources;
    StretchCache stretched; // render jobs hold their own copies of the source audio
    PlaybackEngine playback;
    PlaybackChain chain { playback };
    juce::AudioDeviceManager deviceManager;
    juce::File file;
    AppSettings appSettings;
    Recorder recorder;
    bool recording = false, permissionPending = false;
    SamplePos recordStart = 0, recordPreroll = 0;
    LatencyInputs recordLatency;  // of the device the take is recorded on, captured at start
    juce::String recordTakeName;
    juce::File recordFile;
    juce::File sessionTakes; // unsaved project's takes folder; created lazily, reset per new/opened project

    JUCE_DECLARE_WEAK_REFERENCEABLE(AppController)
};
} // namespace aligna
