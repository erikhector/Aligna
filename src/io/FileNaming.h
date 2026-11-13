#pragma once
#include <juce_core/juce_core.h>
#include "model/Project.h"
#include <vector>

namespace aligna
{
// "Aligna — <file name>" (or "Untitled"), with a trailing " *" when dirty.
juce::String windowTitle(const juce::String& fileName, bool dirty);

// Appends `ext` (with or without leading dot) unless the name already ends with it (case-insensitive).
juce::String withExtension(const juce::String& name, const juce::String& ext);
// Like withExtension, but first drops a different export extension (.wav/.mp3), so "song.wav" -> "song.mp3".
juce::String exportFileName(const juce::String& name, const juce::String& ext);

// "These audio files could not be found:" + one path per line (max 20, then "…and N more"). Empty if none.
juce::String missingFilesMessage(const std::vector<juce::String>& paths);
} // namespace aligna

namespace aligna
{
// True if `f` is the file of one of the project's sources (never export or save over those).
bool isProjectSource(const Project&, const juce::File& f);
} // namespace aligna
