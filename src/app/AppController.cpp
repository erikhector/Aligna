#include "app/AppController.h"
#include "edit/ClipOps.h"
#include "io/ProjectFile.h"
#include <cmath>

namespace aligna
{
void AppController::DeviceCallback::audioDeviceAboutToStart(juce::AudioIODevice* d)
{
    const auto deviceRate = d != nullptr ? d->getCurrentSampleRate() : 0.0;
    if (deviceRate > 0.0)
        owner.resampler.setResamplingRatio(owner.projectRate.load() / deviceRate);
    owner.player.audioDeviceAboutToStart(d);
}

void AppController::DeviceCallback::audioDeviceStopped() { owner.player.audioDeviceStopped(); }

void AppController::DeviceCallback::audioDeviceIOCallbackWithContext(const float* const* in, int numIn,
                                                                     float* const* out, int numOut, int n,
                                                                     const juce::AudioIODeviceCallbackContext& ctx)
{
    owner.player.audioDeviceIOCallbackWithContext(in, numIn, out, numOut, n, ctx);
}

AppController::AppController()
{
    hist.onChange = [this] { rebuildSnapshot(); };
    player.setSource(&resampler);
    deviceManager.initialise(0, 2, nullptr, true); // may fail headless; playback is then silent
    deviceManager.addAudioCallback(&callback);
    rebuildSnapshot();
}

AppController::~AppController()
{
    deviceManager.removeAudioCallback(&callback);
    player.setSource(nullptr);
    hist.onChange = nullptr;
}

void AppController::syncProjectRate()
{
    const auto r = hist.current().sampleRate;
    if (std::abs(r - projectRate.load()) < 1e-9)
        return;
    projectRate.store(r);
    // Re-runs audioDeviceAboutToStart: updates the resampling ratio and re-prepares the chain off the audio thread.
    deviceManager.restartLastAudioDevice();
}

void AppController::rebuildSnapshot() { playback.setSnapshot(makeSnapshot(hist.current(), sources)); }

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

void AppController::newProject()
{
    engine().pause();
    engine().setPosition(0);
    sources.clear();
    file = juce::File();
    hist.reset(Project {});
    syncProjectRate();
    rebuildSnapshot();
}

bool AppController::openProject(const juce::File& f, juce::String& error)
{
    auto p = loadProject(f, error);
    if (!p)
        return false;
    engine().pause();
    engine().setPosition(0);
    sources.clear();
    for (auto& s : p->sources)
        sources.load(s, p->sampleRate); // sets missing on failure
    hist.reset(std::move(*p));
    file = f;
    syncProjectRate();
    rebuildSnapshot();
    return true;
}

bool AppController::saveProject(const juce::File& f, juce::String& error)
{
    if (!aligna::saveProject(hist.current(), f, error))
        return false;
    file = f;
    hist.markSaved();
    return true;
}
} // namespace aligna
