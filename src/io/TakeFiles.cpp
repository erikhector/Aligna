#include "io/TakeFiles.h"

namespace aligna
{
juce::File takesFolderFor(const juce::File& projectFile)
{
    return projectFile.getSiblingFile(projectFile.getFileNameWithoutExtension() + " Takes");
}

juce::File defaultUnsavedTakesRoot()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Aligna")
        .getChildFile("Unsaved Takes");
}

juce::File newSessionTakesFolder(const juce::File& root, juce::Time now)
{
    const auto base = now.formatted("%Y-%m-%d %H-%M-%S");
    auto f = root.getChildFile(base);
    for (int n = 2; f.exists(); ++n)
        f = root.getChildFile(base + " (" + juce::String(n) + ")");
    return f;
}

juce::File uniqueTakeFile(const juce::File& folder, const juce::String& takeName)
{
    auto f = folder.getChildFile(takeName + ".wav");
    for (int n = 2; f.exists(); ++n)
        f = folder.getChildFile(takeName + " (" + juce::String(n) + ").wav");
    return f;
}

std::map<juce::String, juce::String> relocateTakes(const juce::File& from, const juce::File& to,
                                                   juce::String& error)
{
    std::map<juce::String, juce::String> result;
    error = {};
    if (from.getFullPathName() == to.getFullPathName())
        return result;
    if (!to.createDirectory())
    {
        error = "Could not create folder " + to.getFullPathName();
        return result;
    }

    for (auto& f : from.findChildFiles(juce::File::findFiles, false))
    {
        auto target = to.getChildFile(f.getFileName());
        for (int n = 2; target.exists(); ++n)
            target = to.getChildFile(f.getFileNameWithoutExtension() + " (" + juce::String(n) + ")" + f.getFileExtension());
        const auto oldPath = f.getFullPathName();
        bool ok = f.moveFileTo(target);
        if (!ok && f.copyFileTo(target))
        {
            ok = true;
            f.deleteFile(); // best effort; the copy is what matters
        }
        if (ok)
            result[oldPath] = target.getFullPathName();
        else
            error = "Could not move " + f.getFileName();
    }
    return result;
}

void applyRelocation(Project& p, const std::map<juce::String, juce::String>& m)
{
    for (auto& s : p.sources)
        if (auto it = m.find(s.path); it != m.end())
            s.path = it->second;
}
} // namespace aligna
