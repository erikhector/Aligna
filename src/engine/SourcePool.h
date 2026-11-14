#pragma once
#include "model/Project.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace aligna
{
using AudioData = std::shared_ptr<const juce::AudioBuffer<float>>;

// Result of decoding one file: the audio, or an error for the user, or `cancelled` (no audio, no error).
struct DecodedAudio
{
    AudioData audio;
    juce::String error;
    bool cancelled = false;
};

// Called with the fraction done (0..1) while a file is decoded; returning false cancels the decode.
using DecodeProgress = std::function<bool(float)>;

// Files to decode in one go, off the message thread: run() touches nothing but the batch itself.
struct DecodeBatch
{
    struct Item
    {
        juce::File file;
        AudioData audio;    // set by run(), nullptr on failure
        juce::String error; // set by run() when the file could not be decoded
    };
    std::vector<Item> items;
    double projectRate = 48000;

    // Decodes the items in order. progress(index of the file, fraction of that file) returning false cancels:
    // run() then returns false and every buffer decoded so far is dropped.
    bool run(const std::function<bool(int, float)>& progress = {});
};

// Holds audio files fully in memory at the project sample rate.
// Decoding (decodeFile) is a free-standing step that may run on any thread; the pool itself is
// message-thread only. get() returns a shared_ptr copy so snapshots can keep buffers alive.
class SourcePool
{
public:
    // Decodes a whole file at the project rate. Uses no pool state, so it may run on a worker thread (and on
    // several at once). Cancelling is prompt: progress is asked while reading and inside the resampler.
    static DecodedAudio decodeFile(const juce::File& file, double projectRate, const DecodeProgress& progress = {});

    // Message thread: puts decoded audio into the pool as a new source.
    SourceInfo store(const juce::File& file, AudioData data);
    // Message thread: puts decoded audio into the pool under info.id and updates info. A null buffer (the
    // decode failed) sets info.missing, forgets any buffer of that id and returns false.
    bool store(SourceInfo& info, AudioData data);

    // Synchronous decode + store.
    std::optional<SourceInfo> import(const juce::File& file, double projectRate, juce::String& error);
    bool load(SourceInfo& info, double projectRate); // sets info.missing on failure
    AudioData get(const juce::String& id) const;     // nullptr if not loaded
    void clear();
    // Forgets one buffer (no-op for an unknown id). Snapshots and render jobs share ownership, so anything
    // still using it keeps it alive.
    void remove(const juce::String& id);
    // Forgets every buffer whose id is not in `ids`.
    void retainOnly(const std::set<juce::String>& ids);
    int size() const { return (int) buffers.size(); }

private:
    std::map<juce::String, AudioData> buffers;
};
} // namespace aligna
