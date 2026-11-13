#include "engine/StretchCache.h"
#include "engine/Stretcher.h"
#include <juce_events/juce_events.h>
#include <new>

namespace aligna
{
// Owned by the pool (deleted when finished). Shares only its JobState with the cache, and reaches the cache only
// through the liveness token, on the message thread.
class StretchCache::Job final : public juce::ThreadPoolJob
{
public:
    Job(std::shared_ptr<JobState> s, std::weak_ptr<Token> t)
        : juce::ThreadPoolJob("Stretch"), state(std::move(s)), token(std::move(t))
    {
    }

    JobStatus runJob() override
    {
        auto& s = *state;
        if (! s.cancelled.load())
        {
            try
            {
                s.result = stretchBuffer(*s.input, s.sampleRate, s.ratio,
                                         [this](float) { return ! state->cancelled.load() && ! shouldExit(); });
            }
            catch (const std::bad_alloc&)
            {
                s.result.reset(); // treated like a failed render: the clip stays silent
            }
        }
        s.input.reset(); // the message thread never reads it after queuing
        const bool deliver = s.result.has_value() && ! s.cancelled.load();
        s.done.store(true, std::memory_order_release);
        if (deliver)
            juce::MessageManager::callAsync([t = token] {
                if (auto alive = t.lock())
                    alive->owner->deliver();
            });
        return jobHasFinished;
    }

private:
    std::shared_ptr<JobState> state;
    std::weak_ptr<Token> token;
};

StretchCache::StretchCache(size_t softCapBytes)
    : cap(softCapBytes),
      alive(std::make_shared<Token>(Token { this })),
      pool(juce::ThreadPoolOptions {}.withThreadName("Stretch").withNumberOfThreads(2))
{
}

StretchCache::~StretchCache()
{
    for (auto& [key, job] : jobs)
        job->cancelled.store(true);
    alive.reset();                // queued deliveries become no-ops
    pool.removeAllJobs(true, -1); // drops queued jobs, waits for running ones (they stop at the next progress call)
}

void StretchCache::clear()
{
    for (auto& [key, job] : jobs)
        job->cancelled.store(true);
    jobs.clear();
    needed.clear();
    entries.clear();
    used = 0;
    alive = std::make_shared<Token>(Token { this }); // older jobs hold a weak_ptr to the previous token
}

int countStretching(const Project& p, const SourcePool&, const StretchCache& cache)
{
    int n = 0;
    for (auto& t : p.tracks)
        for (auto& c : t.clips)
        {
            if (nearlyEqual(c.stretch, 1.0))
                continue;
            auto* s = findSource(p, c.sourceId);
            if (s == nullptr || s->missing)
                continue;
            if (cache.get(bufferKey(c)) == nullptr)
                ++n;
        }
    return n;
}

void StretchCache::require(const Project& p, const SourcePool& sources)
{
    collectFinished(); // keep results that finished but were not delivered yet

    struct Want
    {
        AudioData input;
        double ratio;
    };
    std::map<juce::String, Want> want;
    for (auto& t : p.tracks)
        for (auto& c : t.clips)
        {
            if (nearlyEqual(c.stretch, 1.0) || c.stretch < kMinStretch - 1e-9 || c.stretch > kMaxStretch + 1e-9)
                continue;
            auto* s = findSource(p, c.sourceId);
            if (s == nullptr || s->missing)
                continue;
            if (auto in = sources.get(c.sourceId))
                want.emplace(bufferKey(c), Want { std::move(in), c.stretch });
        }

    needed.clear();
    for (auto& [key, w] : want)
        needed.insert(key);

    for (auto it = jobs.begin(); it != jobs.end();)
    {
        if (needed.count(it->first) != 0)
        {
            ++it;
            continue;
        }
        it->second->cancelled.store(true);
        it = jobs.erase(it);
    }

    for (auto& [key, w] : want)
    {
        if (auto e = entries.find(key); e != entries.end())
        {
            e->second.lastUsed = ++clock;
            continue;
        }
        if (jobs.count(key) != 0)
            continue;
        auto state = std::make_shared<JobState>();
        state->input = w.input;
        state->sampleRate = p.sampleRate;
        state->ratio = w.ratio;
        jobs[key] = state;
        pool.addJob(new Job(state, alive), true);
    }
    evict();
}

AudioData StretchCache::get(const juce::String& key) const
{
    auto it = entries.find(key);
    return it == entries.end() ? nullptr : it->second.data;
}

bool StretchCache::isPending(const juce::String& key) const { return jobs.count(key) != 0; }

void StretchCache::waitForIdle()
{
    while (pool.getNumJobs() > 0)
        juce::Thread::sleep(2);
    deliver();
}

void StretchCache::deliver()
{
    if (collectFinished() && onReady)
        onReady();
}

bool StretchCache::collectFinished()
{
    bool stored = false;
    for (auto it = jobs.begin(); it != jobs.end();)
    {
        auto& state = *it->second;
        if (! state.done.load(std::memory_order_acquire))
        {
            ++it;
            continue;
        }
        if (state.result)
        {
            store(it->first, std::move(*state.result));
            stored = true;
        }
        it = jobs.erase(it);
    }
    if (stored)
        evict();
    return stored;
}

void StretchCache::store(const juce::String& key, juce::AudioBuffer<float>&& buffer)
{
    const auto bytes = (size_t) buffer.getNumChannels() * (size_t) buffer.getNumSamples() * sizeof(float);
    auto& e = entries[key];
    used -= e.bytes;
    e.data = std::make_shared<const juce::AudioBuffer<float>>(std::move(buffer));
    e.bytes = bytes;
    e.lastUsed = ++clock;
    used += bytes;
}

void StretchCache::evict()
{
    while (used > cap)
    {
        auto victim = entries.end();
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (needed.count(it->first) == 0 && (victim == entries.end() || it->second.lastUsed < victim->second.lastUsed))
                victim = it;
        if (victim == entries.end())
            return; // only needed buffers left: they are always kept
        used -= victim->second.bytes;
        entries.erase(victim);
    }
}
} // namespace aligna
