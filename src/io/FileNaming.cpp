#include "io/FileNaming.h"

namespace aligna
{
juce::String windowTitle(const juce::String& fileName, bool dirty)
{
    const juce::String dash(juce::CharPointer_UTF8("\xE2\x80\x94"));
    return "Aligna " + dash + " " + (fileName.isEmpty() ? juce::String("Untitled") : fileName) + (dirty ? " *" : "");
}

juce::String withExtension(const juce::String& name, const juce::String& ext)
{
    const auto e = ext.startsWithChar('.') ? ext : "." + ext;
    return name.endsWithIgnoreCase(e) ? name : name + e;
}

juce::String missingFilesMessage(const std::vector<juce::String>& paths)
{
    if (paths.empty())
        return {};
    constexpr size_t cap = 20;
    juce::String m = "These audio files could not be found:";
    for (size_t i = 0; i < paths.size() && i < cap; ++i)
        m += "\n" + paths[i];
    if (paths.size() > cap)
        m += "\n" + juce::String(juce::CharPointer_UTF8("\xE2\x80\xA6")) + "and " + juce::String((int) (paths.size() - cap))
             + " more";
    return m;
}
} // namespace aligna

namespace aligna
{
bool isProjectSource(const Project& p, const juce::File& f)
{
    for (auto& s : p.sources)
        if (juce::File(s.path) == f)
            return true;
    return false;
}
} // namespace aligna
