#pragma once
#include "model/Project.h"
#include <optional>

namespace aligna
{
Clip* findClip(Project&, const juce::String& clipId, int* trackIndex = nullptr);
juce::String addTrackWithClip(Project&, const SourceInfo&, const juce::String& trackName);
bool moveClip(Project&, const juce::String& clipId, SamplePos newStart, int newTrackIndex);
bool trimClipStart(Project&, const juce::String& clipId, SamplePos newStart);
bool trimClipEnd(Project&, const juce::String& clipId, SamplePos newEnd);
std::optional<juce::String> splitClip(Project&, const juce::String& clipId, SamplePos at);
bool deleteClip(Project&, const juce::String& clipId);
bool setClipGain(Project&, const juce::String& clipId, float gainDb);
bool setClipFades(Project&, const juce::String& clipId, SamplePos fadeIn, SamplePos fadeOut);
bool setClipStretch(Project&, const juce::String& clipId, double newStretch);
bool setClipTempo(Project&, const juce::String& clipId, double sourceBpm); // 0 clears the tempo, stretch stays
bool setProjectBpm(Project&, double bpm); // also re-derives the stretch of every tempo clip
bool removeTrack(Project&, int trackIndex);
} // namespace aligna
