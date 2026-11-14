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
// A take whose recording has stopped (file closed, device released) and that still has to be decoded and placed.
struct StoppedTake
{
    juce::File file;
    juce::String name;       // of the new track
    juce::String failure;    // the recorder's failure message, if any
    juce::String stopReason; // why the recording stopped by itself, if it did
    SamplePos start = 0, preroll = 0;
    LatencyInputs latency; // project rate and manual offset included
};

// One import, project open or recorded take whose audio is decoded off the message thread. Made by a
// begin... call of the controller; then batch.run() on any one thread; then AppController::finishLoad.
struct PendingLoad
{
    enum class Kind { import, open, take };
    Kind kind = Kind::import;
    DecodeBatch batch;

    Project project;                 // open: the loaded and validated project
    juce::File projectFile;          // open
    std::vector<size_t> sourceIndex; // open: batch item -> index into project.sources
    StoppedTake take;                // take
};

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

    // The same in two steps, so that the decoding can run on a worker thread (message thread, except batch.run):
    //   auto load = beginImport(...) / beginOpen(...) / beginStopRecording(...);
    //   load->batch.run(progress);      // any thread; touches only the batch
    //   finishLoad(load, cancelled);
    // From begin to finish the history is locked, so the project cannot be edited, and no second load and no
    // recording may start (isLoading()). At most one load at a time.
    bool isLoading() const { return activeLoad != nullptr; }
    // Null when nothing is to be decoded or the import is refused (the reason is appended to `errors`).
    std::shared_ptr<PendingLoad> beginImport(const juce::Array<juce::File>& files, juce::String& errors);
    // Loads and validates the project file. Null and `error` on failure. The current project stays as it is,
    // audio and playback included, until finishLoad replaces it.
    std::shared_ptr<PendingLoad> beginOpen(const juce::File& file, juce::String& error);
    // Commits the load, or abandons it when cancelled: a cancelled import adds nothing, a cancelled open leaves
    // the current project untouched, a cancelled take keeps its file but adds no track. Returns the text for
    // the user: the per-file errors of an import, the message of a take; empty for an open.
    // A load that is not the active one (already finished or abandoned) is ignored.
    juce::String finishLoad(const std::shared_ptr<PendingLoad>& load, bool cancelled);
    void abandonLoad() { finishLoad(activeLoad, true); } // no-op when nothing is loading
    // Why a source of the opened project that exists could not be decoded; empty when it loaded or was not
    // found. Kept beside the project, not saved with it.
    juce::String sourceProblem(const juce::String& sourceId) const;
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
    // The same without the wait for the decode: stops at once (take file closed, transport stopped). Returns
    // the load that decodes and places the take, or null with the final `message` when there is nothing to
    // decode (not recording, or a take too short to keep). finishLoad returns the message otherwise.
    std::shared_ptr<PendingLoad> beginStopRecording(juce::String& message);
    // If set, a recording that stops by itself hands its take to this instead of decoding it at once on the
    // message thread; the receiver runs the load and calls finishLoad (whose message includes the reason).
    std::function<void(std::shared_ptr<PendingLoad>)> onTakePending;
    // Fired (message thread) when recording stopped by itself (device failure, failed disk write, disk full,
    // take length limit); the take is kept.
    std::function<void(juce::String message)> onRecordingStopped;

    // Fired (message thread) after every project change, once the playback snapshot has been rebuilt,
    // and again whenever a stretched buffer finishes rendering.
    std::function<void()> onProjectChanged;

private:
    void projectChanged(); // requires the project's stretched buffers, then rebuilds the snapshot
    void dropUnreferencedSources(); // pool buffers that no state of the history (undo and redo included) uses
    std::shared_ptr<PendingLoad> beginLoad(std::shared_ptr<PendingLoad>);
    juce::String commitImport(PendingLoad&);
    void commitOpen(PendingLoad&);
    // The recorder/transport half of stopRecording. Null and the final `message` if there is no take to decode.
    std::optional<StoppedTake> stopRecorder(juce::String& message);
    // Places the decoded take as one "Record" step; `data` null: the decode failed with `error`.
    juce::String commitTake(const StoppedTake&, AudioData data, const juce::String& error);
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
    std::shared_ptr<PendingLoad> activeLoad;
    std::map<juce::String, juce::String> sourceProblems; // source id -> decode error, of the opened project

    JUCE_DECLARE_WEAK_REFERENCEABLE(AppController)
};
} // namespace aligna
