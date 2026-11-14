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
// A stretched buffer and where it sits in the stretched source: data holds the samples
// [start, start + data->getNumSamples()) of what a render of the whole source at that ratio would hold.
struct StretchedAudio
{
    AudioData data; // nullptr if not ready
    SamplePos start = 0;
};

// Source audio rendered beyond what the clips play, on each side (less at the source's ends), so that Rubber Band
// has settled (its longest analysis window is well under 0.2 s) before the first sample a clip can reach.
constexpr double kStretchPadSeconds = 0.5;
// Rendered ranges start and end on multiples of this much source audio (or at the source's ends), so trims, splits
// and edge drags inside a block never cause a new render. 10 s of stereo float at 48 kHz is under 4 MB per ratio.
constexpr double kStretchBlockSeconds = 10.0;

// Source samples [start, end) a clip plays, clamped to [0, sourceLength]. The clip's sourceOffset and length are in
// stretched samples: stretched index s is source sample s / stretch.
juce::Range<SamplePos> clipSourceRange(const Clip&, SamplePos sourceLength);

// The part of a source that gets rendered for clips playing `needed` of it.
struct StretchSpan
{
    juce::Range<SamplePos> padded; // `needed` plus kStretchPadSeconds on each side: what an entry must hold
    juce::Range<SamplePos> render; // `padded` widened to whole kStretchBlockSeconds blocks: what a new job renders
};
StretchSpan stretchSpanFor(juce::Range<SamplePos> needed, SamplePos sourceLength, double sampleRate);

// Stretched copies of sources, keyed by bufferKey(clip), rendered in the background. There is one buffer per key,
// shared by every clip with that source and ratio (so the halves of a split stretched clip play one continuous
// render), and it holds only the part of the source those clips play: the hull of their source ranges, padded and
// widened to whole blocks. When the clips come to need more, the larger hull is rendered and replaces the entry once
// it is ready; until then the old buffer keeps being served. An entry larger than needed is kept (no new render)
// unless the cache is over its cap and the entry is more than twice the size a fresh render would be.
// All methods are message-thread only. Renders run on an internal 2-thread pool; each job works on its own copy of
// the input AudioData and never touches Project or SourcePool. Finished renders come back to the message thread via
// MessageManager::callAsync, guarded by a liveness token, so a destroyed cache is never touched.
// The audio thread never sees this object: it only reads AudioData held inside a MixSnapshot.
class StretchCache
{
public:
    // How a job renders its buffer: stretchRange unless replaced (tests inject failures). Runs on a pool thread; a
    // nullopt result without cancellation, or a thrown std::exception, is a failed render.
    // The result must be laid out like stretchRange's: the source samples [srcBegin, srcEnd) of `in`, stretched.
    using RenderFn = std::function<std::optional<juce::AudioBuffer<float>>(
        const juce::AudioBuffer<float>& in, SamplePos srcBegin, SamplePos srcEnd, double sampleRate, double ratio,
        const std::function<bool(float)>& progress)>;

    explicit StretchCache(size_t softCapBytes = size_t(2) << 30, RenderFn render = {});
    ~StretchCache(); // cancels every render and waits for running ones

    // Ensure every key the project needs is ready, with all the audio its clips play, or queued; cancel
    // queued/running jobs no longer needed.
    // Clips whose source is missing or not loaded are never queued. Ratio 1 is never rendered.
    // A key whose render failed (e.g. out of memory) is remembered and not queued again for as long as it stays
    // needed; it is forgotten, and so retried later, once no clip needs it or after clear().
    void require(const Project&, const SourcePool&);
    // The buffer held for a key. It may lack part of what a clip plays while a larger render is on its way (see
    // covers): readers subtract `start` from stretched-source indices and treat anything outside as silence.
    StretchedAudio find(const juce::String& key) const;
    AudioData get(const juce::String& key) const { return find(key).data; } // nullptr if not ready
    // True if the buffer for the clip's key is there and holds everything the clip plays.
    bool covers(const Clip&) const;
    juce::Range<SamplePos> sourceRange(const juce::String& key) const; // source samples the buffer was rendered from
    bool isPending(const juce::String& key) const; // queued or running; never true for a failed key
    bool hasFailed(const juce::String& key) const; // the render failed: the clip stays silent, nothing is pending
    int numFailed() const { return (int) failed.size(); }
    std::function<void()> onReady; // called on the message thread after any render completes or fails
    void waitForIdle();            // tests only: block until no jobs are queued/running, then deliver onReady
    size_t bytesUsed() const { return used; }
    // Cancel and forget every job and drop every entry (bytesUsed() == 0). Safe with jobs running: late deliveries
    // from them are ignored, as after destruction.
    void clear();

private:
    struct JobState
    {
        AudioData input;
        juce::Range<SamplePos> source; // the part of `input` to render
        SamplePos sourceLength = 0;
        double sampleRate = 0, ratio = 1;
        std::shared_ptr<const RenderFn> render;
        std::atomic<bool> cancelled { false }, done { false };
        std::optional<juce::AudioBuffer<float>> result; // written by the worker before `done` is set
    };
    struct Entry
    {
        AudioData data;
        juce::Range<SamplePos> source; // source samples it was rendered from
        SamplePos sourceLength = 0;    // length of that source
        SamplePos start = 0;           // stretched-source index of data's first sample
        size_t bytes = 0;
        std::uint64_t lastUsed = 0;
    };
    struct Token
    {
        StretchCache* owner;
    };
    class Job;

    void deliver();         // message thread: collect finished jobs and call onReady if any were stored or failed
    bool collectFinished(); // stores finished results, notes failures; true if anything was stored or failed
    void store(const juce::String& key, JobState& job);
    void evict();

    const size_t cap;
    size_t used = 0;
    std::uint64_t clock = 0;
    std::map<juce::String, Entry> entries;
    std::map<juce::String, std::shared_ptr<JobState>> jobs; // queued, running or finished-but-not-collected
    std::set<juce::String> needed;                          // keys the last required project uses
    std::set<juce::String> failed;                          // needed keys whose render failed; never auto-retried
    std::shared_ptr<const RenderFn> render;                 // shared read-only with the jobs
    std::shared_ptr<Token> alive; // replaced by clear() so deliveries from older jobs expire
    juce::ThreadPool pool;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StretchCache)
};

// Clips that need a stretched buffer (source present and not missing, ratio != 1) which the cache doesn't have yet,
// or has only in part (StretchCache::covers is false), and may still deliver. Clips whose render failed are not
// counted: they will never become ready on their own.
int countStretching(const Project&, const SourcePool&, const StretchCache&);
// Clips (same conditions) whose stretched buffer failed to render; they stay silent in playback and export.
int countStretchFailed(const Project&, const SourcePool&, const StretchCache&);
} // namespace aligna
