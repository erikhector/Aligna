#pragma once
#include "model/Project.h"
#include <map>

namespace aligna
{
// Sibling "<name without extension> Takes".
juce::File takesFolderFor(const juce::File& projectFile);
// <userApplicationDataDirectory>/Aligna/Unsaved Takes
juce::File defaultUnsavedTakesRoot();
// root/"YYYY-MM-DD HH-MM-SS", with " (2)", " (3)"... appended if it exists. Not created.
juce::File newSessionTakesFolder(const juce::File& root, juce::Time now);
// folder/"<takeName>.wav", else "<takeName> (2).wav"...
juce::File uniqueTakeFile(const juce::File& folder, const juce::String& takeName);
// Moves every file in `from` into `to` (created), uniquely named; copies if a move fails.
// Returns old->new full paths of everything that now lives in `to`; error is non-empty if any file
// could not be moved or copied.
std::map<juce::String, juce::String> relocateTakes(const juce::File& from, const juce::File& to,
                                                   juce::String& error);
// Rewrites SourceInfo::path where it matches a key of the map.
void applyRelocation(Project&, const std::map<juce::String, juce::String>&);
} // namespace aligna
