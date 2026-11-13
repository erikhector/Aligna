#pragma once
#include "model/Project.h"
#include <optional>

namespace aligna
{
/** Writes the project as JSON atomically (temp file + replace). */
bool saveProject(const Project&, const juce::File& file, juce::String& error);

/** Loads and validates a project file. Returns nullopt and sets error on failure. */
std::optional<Project> loadProject(const juce::File& file, juce::String& error);
} // namespace aligna
