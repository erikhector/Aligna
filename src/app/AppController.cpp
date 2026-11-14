#include "app/AppController.h"
#include "app/MicPermission.h"
#include "edit/ClipOps.h"
#include "io/ProjectFile.h"
#include "io/TakeFiles.h"
#include <algorithm>
#include <cmath>

namespace aligna
{
namespace
{
const juce::String noInputMessage = "No audio input available. Choose one in Audio Settings.";

// Must match maxSeconds in engine/SourcePool.cpp: a longer take could not be imported when recording stops.
constexpr double maxSourceSeconds = 2.0 * 60.0 * 60.0;
// Recording stops by itself this long before the limit (resampling to the project rate may round up).
constexpr double takeLimitMarginSeconds = 60.0;
constexpr int timerMs = 250;

// The saved setup enables an input channel (JUCE writes "audioDeviceInChans" only for explicit channels).
// Puts the recorder's failure message, if any, in front of a take's message.
juce::String withFailure(const juce::String& failure, const juce::String& msg)
{
    if (failure.isEmpty())
        return msg;
    return msg.isEmpty() ? failure : failure + "\n" + msg;
}

bool savedHasInputs(const juce::XmlElement* xml)
{
    return xml != nullptr && xml->getStringAttribute("audioDeviceInChans").containsChar('1');
}
} // namespace

AppController::AppController()
{
    hist.onChange = [this] { projectChanged(); };
    stretched.onReady = [this] { rebuildSnapshot(); };
    // No inputs unless the saved setup has one, so there is no microphone prompt at launch.
    const auto saved = appSettings.audioDeviceState();
    deviceManager.initialise(savedHasInputs(saved.get()) ? 2 : 0, 2, saved.get(), true); // may fail headless
    chain.setRecorder(&recorder);
    deviceManager.addAudioCallback(&chain);
    projectChanged();
}

AppController::~AppController()
{
    masterReference.clear(); // pending permission callbacks see a null controller from here on
    stopTimer();
    onProjectChanged = nullptr; // the UI is gone; stopRecording below still changes the project
    onRecordingStopped = nullptr;
    onTakePending = nullptr;
    abandonLoad(); // its worker, if any, was the UI's and is gone; a pending take keeps its file
    if (recording)
        stopRecording(); // keeps the take file
    if (auto xml = deviceManager.createStateXml())
        appSettings.setAudioDeviceState(xml.get());
    deviceManager.removeAudioCallback(&chain);
    chain.setRecorder(nullptr);
    hist.onChange = nullptr;
    stretched.onReady = nullptr;
    stopTimer(); // stopRecording's snapshot change restarted it
}

void AppController::syncProjectRate() { chain.setProjectRate(hist.current().sampleRate, deviceManager); }

void AppController::projectChanged()
{
    dropUnreferencedSources();
    stretched.require(hist.current(), sources);
    rebuildSnapshot();
}

void AppController::dropUnreferencedSources()
{
    // An undone import stays loaded while redo can bring it back, and goes once its state leaves the history.
    std::set<juce::String> used;
    hist.rewriteAllStates([&used](Project& p) { // only reads: the history has no other way to visit its states
        for (auto& s : p.sources)
            used.insert(s.id);
    });
    sources.retainOnly(used);
}

void AppController::rebuildSnapshot()
{
    playback.setSnapshot(makeSnapshot(hist.current(), sources, stretched));
    if (!isTimerRunning())
        startTimer(timerMs); // frees the snapshot just retired without waiting for another edit
    if (onProjectChanged)
        onProjectChanged();
}

void AppController::releaseProjectAudio()
{
    engine().pause();
    engine().setPosition(0);
    playback.setSnapshot(std::make_shared<const MixSnapshot>()); // the old snapshot owned every source buffer
    if (!isTimerRunning())
        startTimer(timerMs);
    sources.clear();
    stretched.clear(); // never reuse renders of an earlier project, whose sources may have been edited
}

std::shared_ptr<PendingLoad> AppController::beginLoad(std::shared_ptr<PendingLoad> load)
{
    abandonLoad(); // never two at once (the callers have already checked)
    activeLoad = load;
    hist.setLocked(true);
    return load;
}

juce::String AppController::finishLoad(const std::shared_ptr<PendingLoad>& load, bool cancelled)
{
    if (load == nullptr || load != activeLoad)
        return {};
    activeLoad = nullptr;
    hist.setLocked(false);
    if (load->kind == PendingLoad::Kind::take)
    {
        const auto& take = load->take;
        auto msg = cancelled ? withFailure(take.failure, "The take was not added to the project.\nIts file was kept: "
                                                              + take.file.getFullPathName())
                             : commitTake(take, load->batch.items[0].audio, load->batch.items[0].error);
        if (take.stopReason.isNotEmpty())
            msg = msg.isEmpty() ? take.stopReason : take.stopReason + "\n" + msg;
        return msg;
    }
    if (cancelled)
        return {}; // the batch's buffers go with the load
    if (load->kind == PendingLoad::Kind::import)
        return commitImport(*load);
    commitOpen(*load);
    return {};
}

std::shared_ptr<PendingLoad> AppController::beginImport(const juce::Array<juce::File>& files, juce::String& errors)
{
    if (files.isEmpty())
        return nullptr;
    if (recording || permissionPending || hist.isLocked())
    {
        // The history refuses edits now, so the decoded audio could not become a track.
        errors += isLoading() ? "Wait until loading has finished before importing files.\n"
                              : "Stop recording before importing files.\n";
        return nullptr;
    }
    auto load = std::make_shared<PendingLoad>();
    load->kind = PendingLoad::Kind::import;
    load->batch.projectRate = hist.current().sampleRate;
    for (auto& f : files)
        load->batch.items.push_back({ f, nullptr, {} });
    return beginLoad(std::move(load));
}

juce::String AppController::commitImport(PendingLoad& load)
{
    juce::String errors;
    std::vector<SourceInfo> imported;
    for (auto& item : load.batch.items)
    {
        if (item.audio != nullptr)
            imported.push_back(sources.store(item.file, std::move(item.audio)));
        else
            errors += item.file.getFileName() + ": " + item.error + "\n";
    }
    if (imported.empty())
        return errors;
    // Pool already holds the audio, so onChange's snapshot includes it.
    hist.perform("Import", [&](Project& p) {
        for (auto& s : imported)
            addTrackWithClip(p, s, juce::File(s.path).getFileNameWithoutExtension());
        return true;
    });
    return errors;
}

void AppController::importFiles(const juce::Array<juce::File>& files, juce::String& errors)
{
    if (auto load = beginImport(files, errors))
    {
        load->batch.run();
        errors += finishLoad(load, false);
    }
}

void AppController::newProject(double sampleRate)
{
    abandonLoad(); // a load still running would otherwise land in the new project
    stopRecording(); // no-op unless recording; the take is kept in the outgoing project's folder
    sessionTakes = juce::File();
    releaseProjectAudio();
    sourceProblems.clear();
    file = juce::File();
    Project p;
    p.sampleRate = sampleRate;
    hist.reset(std::move(p));
    syncProjectRate();
    projectChanged();
}

std::shared_ptr<PendingLoad> AppController::beginOpen(const juce::File& f, juce::String& error)
{
    if (recording || permissionPending || hist.isLocked())
    {
        error = isLoading() ? "Wait until loading has finished before opening a project."
                            : "Stop recording before opening a project.";
        return nullptr;
    }
    auto p = loadProject(f, error);
    if (!p)
        return nullptr;
    auto load = std::make_shared<PendingLoad>();
    load->kind = PendingLoad::Kind::open;
    load->project = std::move(*p);
    load->projectFile = f;
    load->batch.projectRate = load->project.sampleRate;
    for (size_t i = 0; i < load->project.sources.size(); ++i)
    {
        const auto& s = load->project.sources[i];
        if (s.missing) // the loader already ruled it out (not a regular file, or a network path): never open it
            continue;
        load->batch.items.push_back({ juce::File(s.path), nullptr, {} });
        load->sourceIndex.push_back(i);
    }
    return beginLoad(std::move(load));
}

void AppController::commitOpen(PendingLoad& load)
{
    sessionTakes = juce::File();
    // The outgoing project is given up only now, with the new audio already decoded: the two are in memory
    // together while decoding, which is the price of a cancelled open leaving the current project untouched.
    releaseProjectAudio();
    sourceProblems.clear();
    for (size_t k = 0; k < load.batch.items.size(); ++k)
    {
        auto& item = load.batch.items[k];
        auto& s = load.project.sources[load.sourceIndex[k]];
        if (!sources.store(s, std::move(item.audio))) // sets missing on failure
            sourceProblems[s.id] = item.error;
    }
    hist.reset(std::move(load.project));
    file = load.projectFile;
    syncProjectRate();
    projectChanged();
}

bool AppController::openProject(const juce::File& f, juce::String& error)
{
    abandonLoad();
    stopRecording(); // no-op unless recording
    auto load = beginOpen(f, error);
    if (load == nullptr)
        return false;
    load->batch.run();
    finishLoad(load, false);
    return true;
}

juce::String AppController::sourceProblem(const juce::String& sourceId) const
{
    const auto it = sourceProblems.find(sourceId);
    return it == sourceProblems.end() ? juce::String() : it->second;
}

bool AppController::saveProject(const juce::File& f, juce::String& error)
{
    if (recording)
    {
        error = "Stop recording before saving.";
        return false;
    }
    if (isLoading()) // saving may move the take files, one of which may be being read right now
    {
        error = "Wait until loading has finished before saving.";
        return false;
    }
    // First save of an unsaved project: move every take of this session next to the project file.
    if (file == juce::File() && sessionTakes != juce::File() && sessionTakes.isDirectory())
    {
        juce::String err;
        const auto map = relocateTakes(sessionTakes, takesFolderFor(f), err);
        hist.rewriteAllStates([&map](Project& p) { applyRelocation(p, map); });
        if (err.isNotEmpty())
        {
            error = err;
            return false; // the session folder is kept, so a retry moves what is left
        }
    }
    if (!aligna::saveProject(hist.current(), f, error))
        return false;
    file = f;
    sessionTakes = juce::File(); // later takes go to takesFolderFor(file)
    hist.markSaved();
    return true;
}

int AppController::activeInputChannels() const
{
    auto* d = deviceManager.getCurrentAudioDevice();
    return d != nullptr ? d->getActiveInputChannels().countNumberOfSetBits() : 0;
}

bool AppController::enableFirstInput()
{
    const auto prev = deviceManager.getAudioDeviceSetup();
    auto setup = prev;
    if (setup.inputDeviceName.isEmpty())
    {
        auto* type = deviceManager.getCurrentDeviceTypeObject();
        if (type == nullptr)
            return false;
        const auto names = type->getDeviceNames(true);
        if (names.isEmpty())
            return false;
        setup.inputDeviceName = names[std::max(0, type->getDefaultDeviceIndex(true))];
    }
    setup.inputChannels.clear();
    setup.inputChannels.setBit(0);
    setup.useDefaultInputChannels = false;
    // Restarts the device; the chain's audioDeviceAboutToStart runs again with the new input.
    if (deviceManager.setAudioDeviceSetup(setup, true).isEmpty() && activeInputChannels() > 0)
        return true;
    // A failed open deletes the current device, output included: go back to the previous setup, without
    // saving the failed attempt as the user's choice.
    deviceManager.setAudioDeviceSetup(prev, false);
    return false;
}

void AppController::startRecording(std::function<void(juce::String error)> done)
{
    if (recording || permissionPending)
    {
        if (done)
            done({});
        return;
    }
    permissionPending = true;
    juce::WeakReference<AppController> self(this);
    // Every time: an open input does not mean permission (macOS gives silence when it is denied).
    requestMicrophoneAccess([self, done](bool granted) {
        // May arrive later (permission prompt): the controller may be gone, or be recording by then.
        juce::MessageManager::callAsync([self, done, granted] {
            auto* c = self.get();
            if (c == nullptr)
                return;
            c->permissionPending = false;
            if (c->recording)
            {
                if (done)
                    done({});
                return;
            }
            if (!granted || (c->activeInputChannels() <= 0 && !c->enableFirstInput()))
            {
                if (done)
                    done(noInputMessage);
                return;
            }
            c->beginRecording(done);
        });
    });
}

void AppController::beginRecording(const std::function<void(juce::String)>& done)
{
    auto fail = [&done](const juce::String& e) {
        if (done)
            done(e);
    };
    const int inputs = activeInputChannels();
    const double deviceRate = chain.getDeviceRate();
    if (inputs <= 0 || deviceRate <= 0.0)
        return fail(noInputMessage);

    const auto& project = hist.current();
    const SamplePos start = engine().getPosition(); // playing or not
    const SamplePos pre = prerollStart(project, start, appSettings.prerollOn());

    juce::File folder;
    if (file != juce::File())
        folder = takesFolderFor(file);
    else
    {
        if (sessionTakes == juce::File())
            sessionTakes = newSessionTakesFolder(defaultUnsavedTakesRoot(), juce::Time::getCurrentTime());
        folder = sessionTakes;
    }
    if (isDiskAlmostFull(folder))
        return fail("Cannot record: the disk is almost full.");
    const auto takeName = nextTakeName(project);
    const auto takeFile = uniqueTakeFile(folder, takeName);

    juce::String error;
    if (!recorder.open(takeFile, deviceRate, std::min(2, inputs), error))
        return fail(error);

    recordStart = start;
    recordPreroll = pre;
    recordTakeName = takeName;
    recordFile = takeFile;
    recordLatency = { chain.getInputLatency(), chain.getOutputLatency(), chain.getChainDelay(), deviceRate,
                      project.sampleRate, 0.0 };
    recording = true;

    hist.setLocked(true);
    engine().prepareStart(pre);
    chain.armRecording();
    startTimer(timerMs);
    fail({});
}

std::optional<StoppedTake> AppController::stopRecorder(juce::String& message)
{
    message = {};
    if (!recording)
        return std::nullopt;
    recording = false; // the timer stops by itself
    chain.disarm(); // a still-pending arm must not start the engine after this stop
    engine().stop(); // back to the pre-roll start
    const auto res = recorder.close();
    const auto failure = recorder.failureMessage(); // after close: writing out the last samples may fail too
    hist.setLocked(false);

    if (res.numSamples <= 0)
    {
        res.file.deleteFile(); // nothing but a header
        message = withFailure(failure, "The take was too short and was not kept.");
        return std::nullopt;
    }

    StoppedTake take;
    take.file = res.file;
    take.name = recordTakeName;
    take.failure = failure;
    take.start = recordStart;
    take.preroll = recordPreroll;
    take.latency = recordLatency;
    take.latency.projectRate = hist.current().sampleRate;
    take.latency.manualOffsetMs = appSettings.latencyOffsetMs();
    return take;
}

juce::String AppController::commitTake(const StoppedTake& take, AudioData data, const juce::String& error)
{
    if (data == nullptr) // the file is kept: it may hold audio that another program can recover
        return withFailure(take.failure,
                           "The take could not be read: " + error + "\nIts file was kept: " + take.file.getFullPathName());

    const auto placed = placeTake(take.preroll, take.start, totalLatency(take.latency), data->getNumSamples(),
                                  take.latency.projectRate);
    if (!placed)
    {
        take.file.deleteFile(); // the decoded audio never reached the pool
        return withFailure(take.failure, "The take was too short and was not kept.");
    }

    const auto info = sources.store(take.file, std::move(data));
    hist.perform("Record", [&](Project& p) {
        addTakeTrack(p, info, take.name, *placed);
        return true;
    });
    return withFailure(take.failure, {});
}

juce::String AppController::stopRecording()
{
    juce::String message;
    const auto take = stopRecorder(message);
    if (!take)
        return message;
    auto decoded = SourcePool::decodeFile(take->file, take->latency.projectRate);
    return commitTake(*take, std::move(decoded.audio), decoded.error);
}

std::shared_ptr<PendingLoad> AppController::beginStopRecording(juce::String& message)
{
    auto take = stopRecorder(message);
    if (!take)
        return nullptr;
    auto load = std::make_shared<PendingLoad>();
    load->kind = PendingLoad::Kind::take;
    load->batch.projectRate = take->latency.projectRate;
    load->batch.items.push_back({ take->file, nullptr, {} });
    load->take = std::move(*take);
    return beginLoad(std::move(load));
}

void AppController::timerCallback()
{
    const bool retiredLeft = playback.purgeRetired();
    if (!recording)
    {
        if (!retiredLeft)
            stopTimer();
        return;
    }
    // A failed disk write is reported by the recorder, like a device failure or an overflow.
    const bool failed = recorder.hasFailed();
    const bool diskFull = !failed && isDiskAlmostFull(recordFile.getParentDirectory());
    const double seconds = recordLatency.deviceRate > 0.0
                               ? (double) recorder.samplesCaptured() / recordLatency.deviceRate : 0.0;
    const bool tooLong = !failed && !diskFull && seconds >= maxSourceSeconds - takeLimitMarginSeconds;
    if (!failed && !diskFull && !tooLong)
        return;

    const juce::String reason = diskFull  ? "Recording stopped: the disk is almost full."
                                : tooLong ? "Recording stopped: a take can be at most 2 hours long."
                                          : "";
    juce::String msg; // the take is kept; starts with the recorder's failure message, if any
    if (onTakePending)
    {
        if (auto load = beginStopRecording(msg))
        {
            load->take.stopReason = reason; // finishLoad puts it in front of the take's message
            onTakePending(std::move(load));
            return;
        }
    }
    else
        msg = stopRecording();
    if (reason.isNotEmpty())
        msg = msg.isEmpty() ? reason : reason + "\n" + msg;
    if (onRecordingStopped)
        onRecordingStopped(msg);
}
} // namespace aligna
