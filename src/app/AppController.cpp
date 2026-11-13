#include "app/AppController.h"
#include "edit/ClipOps.h"
#include "io/ProjectFile.h"
#include <cmath>

namespace aligna
{
AppController::AppController()
{
    hist.onChange = [this] { rebuildSnapshot(); };
    deviceManager.initialise(0, 2, nullptr, true); // may fail headless; playback is then silent
    deviceManager.addAudioCallback(&chain);
    rebuildSnapshot();
}

AppController::~AppController()
{
    deviceManager.removeAudioCallback(&chain);
    hist.onChange = nullptr;
    onProjectChanged = nullptr;
}

void AppController::syncProjectRate() { chain.setProjectRate(hist.current().sampleRate, deviceManager); }

void AppController::rebuildSnapshot()
{
    playback.setSnapshot(makeSnapshot(hist.current(), sources));
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
