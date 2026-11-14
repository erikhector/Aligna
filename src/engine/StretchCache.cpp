#include "engine/StretchCache.h"
#include "engine/Stretcher.h"
#include <juce_events/juce_events.h>
#include <cmath>
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
                s.result = (*s.render)(*s.input, s.source.getStart(), s.source.getEnd(), s.sampleRate, s.ratio,
                                       [this](float) { return ! state->cancelled.load() && ! shouldExit(); });
            }
            catch (const std::exception&) // bad_alloc above all
            {
                s.result.reset(); // a failed render: the clip stays silent
            }
        }
        s.input.reset(); // the message thread never reads it after queuing
        // A failure is delivered too, so the cache can record it instead of leaving the key pending for ever.
        const bool deliver = ! s.cancelled.load();
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

StretchCache::StretchCache(size_t softCapBytes, RenderFn renderFn)
    : cap(softCapBytes),
      render(std::make_shared<const RenderFn>(
          renderFn ? std::move(renderFn)
                   : RenderFn([](const juce::AudioBuffer<float>& in, SamplePos srcBegin, SamplePos srcEnd,
                                 double sampleRate, double ratio, const std::function<bool(float)>& progress) {
                         return stretchRange(in, srcBegin, srcEnd, sampleRate, ratio, progress);
                     }))),
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
    failed.clear();
    entries.clear();
    used = 0;
    alive = std::make_shared<Token>(Token { this }); // older jobs hold a weak_ptr to the previous token
}

juce::Range<SamplePos> clipSourceRange(const Clip& c, SamplePos sourceLength)
{
    if (! (c.stretch > 0.0))
        return {};
    const SamplePos len = std::max<SamplePos>(0, sourceLength);
    const auto lo = (SamplePos) std::floor((double) c.sourceOffset / c.stretch);
    const auto hi = (SamplePos) std::ceil((double) (c.sourceOffset + c.length) / c.stretch);
    const SamplePos a = std::clamp<SamplePos>(lo, 0, len);
    return { a, std::clamp<SamplePos>(hi, a, len) };
}

StretchSpan stretchSpanFor(juce::Range<SamplePos> needed, SamplePos sourceLength, double sampleRate)
{
    const SamplePos len = std::max<SamplePos>(0, sourceLength);
    const auto pad = (SamplePos) std::llround(kStretchPadSeconds * sampleRate);
    const auto block = std::max<SamplePos>(1, (SamplePos) std::llround(kStretchBlockSeconds * sampleRate));
    const SamplePos a = std::clamp<SamplePos>(needed.getStart() - pad, 0, len);
    const SamplePos b = std::clamp<SamplePos>(needed.getEnd() + pad, a, len);
    StretchSpan s;
    s.padded = { a, b };
    s.render = { a / block * block, std::min(len, (b + block - 1) / block * block) };
    return s;
}

namespace
{
// Clips that need a stretched buffer the cache does not hold in full, split by whether its render failed.
int countNotReady(const Project& p, const StretchCache& cache, bool wantFailed)
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
            const auto key = bufferKey(c);
            if (! cache.covers(c) && cache.hasFailed(key) == wantFailed)
                ++n;
        }
    return n;
}
} // namespace

int countStretching(const Project& p, const SourcePool&, const StretchCache& cache)
{
    return countNotReady(p, cache, false);
}

int countStretchFailed(const Project& p, const SourcePool&, const StretchCache& cache)
{
    return countNotReady(p, cache, true);
}

void StretchCache::require(const Project& p, const SourcePool& sources)
{
    collectFinished(); // keep results that finished but were not delivered yet

    struct Want
    {
        AudioData input;
        double ratio;
        juce::Range<SamplePos> hull; // source samples the key's clips play; empty until one plays any
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
            auto in = sources.get(c.sourceId);
            if (in == nullptr)
                continue;
            const auto range = clipSourceRange(c, in->getNumSamples());
            auto& w = want.try_emplace(bufferKey(c), Want { std::move(in), c.stretch, range }).first->second;
            // The hull, not the pieces: one continuous render per key keeps clips of one key sample-continuous.
            if (w.hull.isEmpty())
                w.hull = range;
            else if (! range.isEmpty())
                w.hull = w.hull.getUnionWith(range);
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
    // A failure is only remembered while its key is needed; needing it again later is a fresh attempt.
    std::erase_if(failed, [this](const juce::String& key) { return needed.count(key) == 0; });

    for (auto& [key, w] : want)
    {
        const SamplePos sourceLength = w.input->getNumSamples();
        const auto span = stretchSpanFor(w.hull, sourceLength, p.sampleRate);
        // A render serves if it holds the padded hull and is not wastefully large: more than twice a fresh render
        // while the cache is over its cap. Short of that a larger one is kept, so shrinking never renders again.
        const auto serves = [&](juce::Range<SamplePos> rendered, SamplePos renderedSourceLength) {
            return renderedSourceLength == sourceLength && rendered.contains(span.padded)
                   && ! (used > cap && rendered.getLength() > 2 * span.render.getLength());
        };
        const auto job = jobs.find(key);
        if (auto e = entries.find(key); e != entries.end())
        {
            e->second.lastUsed = ++clock;
            if (serves(e->second.source, e->second.sourceLength))
            {
                if (job != jobs.end()) // a larger render that is not needed after all
                {
                    job->second->cancelled.store(true);
                    jobs.erase(job);
                }
                continue;
            }
            // Otherwise the entry stays, and is served, until its replacement lands.
        }
        if (job != jobs.end())
        {
            if (serves(job->second->source, job->second->sourceLength))
                continue; // already on its way
            job->second->cancelled.store(true);
            jobs.erase(job);
        }
        if (failed.count(key) != 0)
            continue; // not retried automatically
        auto state = std::make_shared<JobState>();
        state->render = render;
        state->input = w.input;
        state->source = span.render;
        state->sourceLength = sourceLength;
        state->sampleRate = p.sampleRate;
        state->ratio = w.ratio;
        jobs[key] = state;
        pool.addJob(new Job(state, alive), true);
    }
    evict();
}

StretchedAudio StretchCache::find(const juce::String& key) const
{
    auto it = entries.find(key);
    return it == entries.end() ? StretchedAudio {} : StretchedAudio { it->second.data, it->second.start };
}

bool StretchCache::covers(const Clip& c) const
{
    auto it = entries.find(bufferKey(c));
    if (it == entries.end())
        return false;
    const auto range = clipSourceRange(c, it->second.sourceLength);
    return range.isEmpty() || it->second.source.contains(range); // empty: the clip lies beyond the source's end
}

juce::Range<SamplePos> StretchCache::sourceRange(const juce::String& key) const
{
    auto it = entries.find(key);
    return it == entries.end() ? juce::Range<SamplePos>() : it->second.source;
}

bool StretchCache::isPending(const juce::String& key) const { return jobs.count(key) != 0; }

bool StretchCache::hasFailed(const juce::String& key) const { return failed.count(key) != 0; }

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
    bool stored = false, anyFailed = false;
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
            store(it->first, state);
            stored = true;
        }
        else
        {
            // Cancelled jobs were already dropped from `jobs`, so a finished one without a result has failed.
            failed.insert(it->first);
            anyFailed = true;
        }
        it = jobs.erase(it);
    }
    if (stored)
        evict();
    return stored || anyFailed;
}

void StretchCache::store(const juce::String& key, JobState& job)
{
    auto& buffer = *job.result;
    const auto bytes = (size_t) buffer.getNumChannels() * (size_t) buffer.getNumSamples() * sizeof(float);
    auto& e = entries[key];
    used -= e.bytes;
    e.data = std::make_shared<const juce::AudioBuffer<float>>(std::move(buffer));
    e.source = job.source;
    e.sourceLength = job.sourceLength;
    e.start = stretchedIndex(job.source.getStart(), job.ratio);
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
