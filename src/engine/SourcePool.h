#pragma once
#include "model/Project.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <map>
#include <memory>
#include <optional>

namespace aligna
{
using AudioData = std::shared_ptr<const juce::AudioBuffer<float>>;

// Loads audio files fully into memory at the project sample rate.
// Message-thread only; get() returns a shared_ptr copy so snapshots can keep buffers alive.
class SourcePool
{
public:
    SourcePool();

    std::optional<SourceInfo> import(const juce::File& file, double projectRate, juce::String& error);
    bool load(SourceInfo& info, double projectRate); // sets info.missing on failure
    AudioData get(const juce::String& id) const;     // nullptr if not loaded
    void clear();
    // Forgets one buffer (no-op for an unknown id). Snapshots and render jobs share ownership, so anything
    // still using it keeps it alive.
    void remove(const juce::String& id);

private:
    AudioData decode(const juce::File& file, double projectRate, juce::String& error); // catches bad_alloc
    AudioData decodeImpl(const juce::File& file, double projectRate, juce::String& error);

    juce::AudioFormatManager formats;
    std::map<juce::String, AudioData> buffers;
};
} // namespace aligna
