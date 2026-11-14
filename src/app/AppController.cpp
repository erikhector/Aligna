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

// The saved setup enables an input channel (JUCE writes "audioDeviceInChans" only for explicit channels).
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
    stopTimer();
    onProjectChanged = nullptr; // the UI is gone; stopRecording below still changes the project
    onRecordingStopped = nullptr;
    if (recording)
        stopRecording(); // keeps the take file
    if (auto xml = deviceManager.createStateXml())
        appSettings.setAudioDeviceState(xml.get());
    deviceManager.removeAudioCallback(&chain);
    chain.setRecorder(nullptr);
    hist.onChange = nullptr;
    stretched.onReady = nullptr;
}

void AppController::syncProjectRate() { chain.setProjectRate(hist.current().sampleRate, deviceManager); }

void AppController::projectChanged()
{
    stretched.require(hist.current(), sources);
    rebuildSnapshot();
}

void AppController::rebuildSnapshot()
{
    playback.setSnapshot(makeSnapshot(hist.current(), sources, stretched));
    if (onProjectChanged)
        onProjectChanged();
}

void AppController::importFiles(const juce::Array<juce::File>& files, juce::String& errors)
{
    const auto rate = hist.current().sampleRate;
    std::vector<SourceInfo> imported;
    for (auto& f : files)
    {
        juce::String err;
        if (auto info = sources.import(f, rate, err))
            imported.push_back(std::move(*info));
        else
            errors += f.getFileName() + ": " + err + "\n";
    }
    if (imported.empty())
        return;
    // Pool already holds the audio, so onChange's snapshot includes it.
    hist.perform("Import", [&](Project& p) {
        for (auto& s : imported)
            addTrackWithClip(p, s, juce::File(s.path).getFileNameWithoutExtension());
        return true;
    });
}

void AppController::newProject(double sampleRate)
{
    stopRecording(); // no-op unless recording; the take is kept in the outgoing project's folder
    sessionTakes = juce::File();
    engine().pause();
    engine().setPosition(0);
    sources.clear();
    file = juce::File();
    Project p;
    p.sampleRate = sampleRate;
    hist.reset(std::move(p));
    stretched.clear();
    syncProjectRate();
    projectChanged();
}

bool AppController::openProject(const juce::File& f, juce::String& error)
{
    auto p = loadProject(f, error);
    if (!p)
        return false;
    stopRecording(); // no-op unless recording
    sessionTakes = juce::File();
    engine().pause();
    engine().setPosition(0);
    sources.clear();
    for (auto& s : p->sources)
        sources.load(s, p->sampleRate); // sets missing on failure
    hist.reset(std::move(*p));
    file = f;
    stretched.clear(); // never reuse renders of an earlier project, whose sources may have been edited
    syncProjectRate();
    projectChanged();
    return true;
}

bool AppController::saveProject(const juce::File& f, juce::String& error)
{
    if (recording)
    {
        error = "Stop recording before saving.";
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
    startTimer(250);
    fail({});
}

juce::String AppController::stopRecording()
{
    if (!recording)
        return {};
    stopTimer();
    recording = false;
    chain.disarm(); // a still-pending arm must not start the engine after this stop
    engine().stop(); // back to the pre-roll start
    const auto failure = recorder.failureMessage();
    const auto res = recorder.close();
    hist.setLocked(false);

    auto withFailure = [&failure](const juce::String& msg) {
        if (failure.isEmpty())
            return msg;
        return msg.isEmpty() ? failure : failure + "\n" + msg;
    };

    const double projectRate = hist.current().sampleRate;
    if (res.numSamples <= 0)
        return withFailure("The take was too short and was not kept.");

    juce::String err;
    const auto info = sources.import(res.file, projectRate, err);
    if (!info)
        return withFailure("The take could not be read: " + err);

    auto L = recordLatency;
    L.projectRate = projectRate;
    L.manualOffsetMs = appSettings.latencyOffsetMs();
    const auto placed = placeTake(recordPreroll, recordStart, totalLatency(L), info->length, projectRate);
    if (!placed)
        return withFailure("The take was too short and was not kept.");

    const auto name = recordTakeName;
    hist.perform("Record", [&](Project& p) {
        addTakeTrack(p, *info, name, *placed);
        return true;
    });
    return withFailure({});
}

void AppController::timerCallback()
{
    if (!recording)
    {
        stopTimer();
        return;
    }
    const bool failed = recorder.hasFailed();
    const bool diskFull = !failed && isDiskAlmostFull(recordFile.getParentDirectory().getBytesFreeOnVolume());
    if (!failed && !diskFull)
        return;

    auto msg = stopRecording(); // the take is kept; starts with the recorder's failure message, if any
    if (diskFull)
    {
        const juce::String full = "Recording stopped: the disk is almost full.";
        msg = msg.isEmpty() ? full : full + "\n" + msg;
    }
    if (onRecordingStopped)
        onRecordingStopped(msg);
}
} // namespace aligna
