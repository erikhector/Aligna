#pragma once
#include "engine/SourcePool.h"
#include "model/Project.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace aligna
{
// Stretched copies of sources, keyed by bufferKey(clip), rendered in the background.
// All methods are message-thread only. Renders run on an internal 2-thread pool; each job works on its own copy of
// the input AudioData and never touches Project or SourcePool. Finished renders come back to the message thread via
// MessageManager::callAsync, guarded by a liveness token, so a destroyed cache is never touched.
// The audio thread never sees this object: it only reads AudioData held inside a MixSnapshot.
class StretchCache
{
public:
    explicit StretchCache(size_t softCapBytes = size_t(2) << 30);
    ~StretchCache(); // cancels every render and waits for running ones

    // Ensure every key the project needs is ready or queued; cancel queued/running jobs no longer needed.
    // Clips whose source is missing or not loaded are never queued. Ratio 1 is never rendered.
    void require(const Project&, const SourcePool&);
    AudioData get(const juce::String& key) const; // nullptr if not ready
    bool isPending(const juce::String& key) const;
    std::function<void()> onReady; // called on the message thread after any render completes
    void waitForIdle();            // tests only: block until no jobs are queued/running, then deliver onReady
    size_t bytesUsed() const { return used; }
    // Cancel and forget every job and drop every entry (bytesUsed() == 0). Safe with jobs running: late deliveries
    // from them are ignored, as after destruction.
    void clear();

private:
    struct JobState
    {
        AudioData input;
        double sampleRate = 0, ratio = 1;
        std::atomic<bool> cancelled { false }, done { false };
        std::optional<juce::AudioBuffer<float>> result; // written by the worker before `done` is set
    };
    struct Entry
    {
        AudioData data;
        size_t bytes = 0;
        std::uint64_t lastUsed = 0;
    };
    struct Token
    {
        StretchCache* owner;
    };
    class Job;

    void deliver();         // message thread: collect finished results and call onReady if any were stored
    bool collectFinished(); // stores finished results; true if anything was stored
    void store(const juce::String& key, juce::AudioBuffer<float>&& buffer);
    void evict();

    const size_t cap;
    size_t used = 0;
    std::uint64_t clock = 0;
    std::map<juce::String, Entry> entries;
    std::map<juce::String, std::shared_ptr<JobState>> jobs; // queued, running or finished-but-not-collected
    std::set<juce::String> needed;                          // keys the last required project uses
    std::shared_ptr<Token> alive; // replaced by clear() so deliveries from older jobs expire
    juce::ThreadPool pool;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StretchCache)
};

// Clips that need a stretched buffer (source present and not missing, ratio != 1) which the cache doesn't have yet.
int countStretching(const Project&, const SourcePool&, const StretchCache&);
} // namespace aligna
