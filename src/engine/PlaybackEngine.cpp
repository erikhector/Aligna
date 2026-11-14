#include "engine/PlaybackEngine.h"
#include <algorithm>

namespace aligna
{
namespace
{
// Message-thread transitions: retried until they apply to the state they were computed from, because the audio
// thread may advance the position (or consume a pending start) in between.
template <typename Fn>
void update(std::atomic<std::uint64_t>& transport, Fn&& next) noexcept
{
    auto w = transport.load();
    while (!transport.compare_exchange_weak(w, next(w)))
    {
    }
}
} // namespace

PlaybackEngine::~PlaybackEngine()
{
    // The owner has detached the device callback first (the PlaybackChain that pulls from this engine is
    // destroyed, or removed from the device manager, before it), so no block is in flight and none can begin.
    jassert((renderEpoch.load() & 1) == 0);
    current.store(nullptr);
    retired.clear();
    latest.reset();
}

void PlaybackEngine::setSnapshot(std::shared_ptr<const MixSnapshot> s)
{
    auto old = std::move(latest);
    latest = std::move(s);
    // Reclamation handshake with beginBlock/endBlock (one audio thread at a time, see the header):
    //
    //   audio thread                        message thread (here)
    //   A1  ++renderEpoch  (now odd)        M1  current = new
    //   A2  snap = current                  M2  e = renderEpoch
    //       ... render from snap ...
    //   A3  ++renderEpoch  (now even)
    //
    // A1, A2, M1 and M2 are all seq_cst, so they have one total order that both threads agree on. Each side
    // stores and then loads the other side's variable; with anything weaker both loads could miss the other
    // side's store (store buffering): the audio thread would take the old pointer while this thread reads an
    // even epoch and frees it.
    //
    // - e even: in that order no block is between A1 and A3 at M2. A block that uses a pointer after M2
    //   therefore has its A1 after M2, hence its A2 after M1: it loads the new pointer. Nobody holds the old
    //   one, so it can be freed now.
    // - e odd: the block that made it odd may have loaded the old pointer. Once renderEpoch != e that block
    //   has passed A3 and no longer uses it, and every later block has its A1 (so its A2) after M1.
    //
    // A snapshot retired by an earlier call obeys the same rule with its own e: the only block that can still
    // hold it is the one that was in flight when it was replaced.
    current.store(latest.get(), std::memory_order_seq_cst);
    if (old != nullptr)
        retired.push_back({ std::move(old), renderEpoch.load(std::memory_order_seq_cst) });
    purgeRetired();
}

bool PlaybackEngine::purgeRetired(bool waitForAll, int timeoutMs)
{
    auto reclaim = [this] {
        const auto now = renderEpoch.load(std::memory_order_seq_cst);
        std::erase_if(retired,
                      [now](const Retired& r) { return (r.epochAtRetire & 1) == 0 || r.epochAtRetire != now; });
    };
    reclaim();
    if (waitForAll)
    {
        // A block normally ends within milliseconds. A stalled audio thread (suspend, debugger) keeps the
        // snapshot alive: it is never freed while the epoch says it may be in use, and this never waits forever.
        const auto start = juce::Time::getMillisecondCounter();
        while (!retired.empty() && (int) (juce::Time::getMillisecondCounter() - start) < timeoutMs)
        {
            juce::Thread::sleep(1);
            reclaim();
        }
    }
    return !retired.empty();
}

std::uint64_t PlaybackEngine::seekWord(std::uint64_t w, SamplePos p, std::uint64_t flags)
{
    const auto generation = (w & generationMask) + generationUnit; // wraps
    return generation | flags | (std::uint64_t) std::clamp<SamplePos>(p, 0, maxPosition);
}

void PlaybackEngine::play()
{
    playStart = getPosition();
    update(transport, [](std::uint64_t w) { return (w & ~pendingBit) | playingBit; });
}

// Not a seek: a block in flight still advances the position, as it was heard.
void PlaybackEngine::pause()
{
    update(transport, [](std::uint64_t w) { return w & ~(playingBit | pendingBit); });
}

void PlaybackEngine::stop()
{
    update(transport, [p = playStart](std::uint64_t w) { return seekWord(w, p, 0); });
}

void PlaybackEngine::setPosition(SamplePos p)
{
    update(transport, [p](std::uint64_t w) { return seekWord(w, p, w & playingBit); });
}

void PlaybackEngine::prepareStart(SamplePos p)
{
    update(transport, [p](std::uint64_t w) { return seekWord(w, p, pendingBit); });
    playStart = std::clamp<SamplePos>(p, 0, maxPosition);
}

void PlaybackEngine::cancelPendingStart() noexcept
{
    update(transport, [](std::uint64_t w) { return w & ~pendingBit; });
}

bool PlaybackEngine::startFromAudioThread() noexcept
{
    // One attempt is enough. Only message-thread commands race with this, and each of them withdraws the
    // pending start (and so wins), except a new prepareStart(): its caller arms again, which repeats this call
    // in a later callback.
    auto w = transport.load();
    return (w & pendingBit) != 0 && transport.compare_exchange_strong(w, (w & ~pendingBit) | playingBit);
}

bool PlaybackEngine::beginBlock(Block& b) noexcept
{
    // Position and playing flag come from one load, so a message-thread pause() + setPosition() is seen either
    // as a whole or not at all.
    b.word = transport.load();
    b.position = positionOf(b.word);
    b.snapshot = nullptr;
    if ((b.word & playingBit) == 0)
        return false;
    // Enter the epoch BEFORE loading the pointer (A1, A2 in setSnapshot). This is the only place the audio
    // thread reads `current`.
    [[maybe_unused]] const auto before = renderEpoch.fetch_add(1, std::memory_order_seq_cst);
    jassert((before & 1) == 0); // a second thread is inside a block: the scheme does not allow that
    b.snapshot = current.load(std::memory_order_seq_cst);
    return true;
}

void PlaybackEngine::endBlock(const Block& b, int numSamples) noexcept
{
    renderEpoch.fetch_add(1, std::memory_order_seq_cst); // A3: b.snapshot is not used past this point

    // Advance, unless the message thread set the position during the block: its seek wins, even one to the very
    // position this block started from (the generation differs then). pause()/play() change other bits only,
    // and the advance is retried on top of them. The retries are bounded to stay wait-free: each failure takes
    // a new message-thread command inside this loop, and dropping the advance then is harmless.
    const auto seekBits = generationMask | positionMask;
    const auto advanced = (std::uint64_t) std::min<SamplePos>(maxPosition, b.position + std::max(0, numSamples));
    auto w = b.word;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        if (((w ^ b.word) & seekBits) != 0)
            return;
        if (transport.compare_exchange_strong(w, (w & ~positionMask) | advanced))
            return;
    }
}

void PlaybackEngine::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();
    Block b;
    if (!beginBlock(b))
        return;
    if (b.snapshot != nullptr)
        renderMix(*b.snapshot, b.position, *info.buffer, info.startSample, info.numSamples);
    endBlock(b, info.numSamples);
}
} // namespace aligna
