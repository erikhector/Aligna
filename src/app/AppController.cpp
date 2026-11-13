#include "app/AppController.h"
#include "edit/ClipOps.h"
#include "io/ProjectFile.h"
#include <cmath>

namespace aligna
{
AppController::AppController()
{
    hist.onChange = [this] { projectChanged(); };
    stretched.onReady = [this] { rebuildSnapshot(); };
    deviceManager.initialise(0, 2, nullptr, true); // may fail headless; playback is then silent
    deviceManager.addAudioCallback(&chain);
    projectChanged();
}

AppController::~AppController()
{
    deviceManager.removeAudioCallback(&chain);
    hist.onChange = nullptr;
    stretched.onReady = nullptr;
    onProjectChanged = nullptr;
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
    engine().pause();
    engine().setPosition(0);
    sources.clear();
    file = juce::File();
    Project p;
    p.sampleRate = sampleRate;
    hist.reset(std::move(p));
    syncProjectRate();
    projectChanged();
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
    projectChanged();
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
