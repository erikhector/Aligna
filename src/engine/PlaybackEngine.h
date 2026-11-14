#pragma once
#include "engine/Mixer.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace aligna
{
// Real-time transport + mix source. Control methods are message-thread; getNextAudioBlock is audio-thread.
//
// One audio thread at a time is assumed: getNextAudioBlock (beginBlock ... endBlock) is never entered concurrently
// from two threads. That holds for the device callback (PlaybackChain -> AudioSourcePlayer -> ResamplingAudioSource,
// each of which pulls its source from the one device callback, under its own callback lock) and for the tests,
// which either pull from a single thread or drive beginBlock/endBlock by hand. Nothing else renders from the
// engine: offline rendering calls renderMix on its own snapshot, and PlaybackChain::measureResamplerDelay uses a
// private source.
class PlaybackEngine : public juce::AudioSource
{
public:
    PlaybackEngine() = default;
    ~PlaybackEngine() override;

    // Positions are kept in 40 bits (see `transport`): about 66 days at 192 kHz. Seeks are clamped to this.
    static constexpr SamplePos maxPosition = (SamplePos(1) << 40) - 1;
    // How long purgeRetired(true) waits for an audio callback to leave the snapshot it is rendering.
    static constexpr int purgeTimeoutMs = 2000;

    // Message thread. The previous snapshot is retired, and freed by this or a later setSnapshot / purgeRetired
    // call (or the destructor) once no audio callback can still be rendering from it: at once when no block is
    // in flight, otherwise after that block has ended. The audio thread never owns or destroys a snapshot.
    void setSnapshot(std::shared_ptr<const MixSnapshot>);
    // Message thread. Frees the retired snapshots that no audio callback can still be using; true if some
    // remain. With waitForAll, first waits (at most timeoutMs) for the block in flight to end; when the audio
    // thread is stalled inside a block for longer, gives up and returns true with the snapshot still retired.
    bool purgeRetired(bool waitForAll = false, int timeoutMs = purgeTimeoutMs);

    void play();
    void pause();
    void stop(); // pause + return to the position where play() was last called
    void setPosition(SamplePos);
    // Message thread: pause, then position = play start = p, and a start from the audio thread becomes pending.
    // Pair with startFromAudioThread() so playback of p begins in a known device callback.
    void prepareStart(SamplePos p);
    // Any thread: withdraws the start made pending by prepareStart(). Every other transport command above
    // withdraws it too, so a command issued after prepareStart() always wins over a start that comes later.
    void cancelPendingStart() noexcept;
    // Audio thread: start playing from the current position, if the start made pending by prepareStart() has not
    // been withdrawn since. Returns whether playback started. Wait-free (a single compare-exchange).
    bool startFromAudioThread() noexcept;
    SamplePos getPosition() const { return positionOf(transport.load()); }
    bool isPlaying() const { return (transport.load() & playingBit) != 0; }

    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo&) override;

    // Audio thread. getNextAudioBlock is beginBlock + renderMix + endBlock; they are separate so that tests can
    // hold a block "in flight" and issue message-thread commands at that point. Wait-free, no allocation.
    struct Block
    {
        SamplePos position = 0;
        const MixSnapshot* snapshot = nullptr; // valid until endBlock; may be null
        std::uint64_t word = 0;                // the transport state the block started from
    };
    // False when not playing (nothing to render, no endBlock). True: the block must be ended with endBlock.
    bool beginBlock(Block&) noexcept;
    // Stops using the snapshot, then advances the position by numSamples unless a seek happened meanwhile.
    void endBlock(const Block&, int numSamples) noexcept;

private:
    struct Retired
    {
        std::shared_ptr<const MixSnapshot> snap;
        std::uint64_t epochAtRetire; // renderEpoch read after the replacement was published
    };

    // The whole transport state in one word, so that every transition is a single atomic step:
    //   bits 0..39   position
    //   bit  40      playing
    //   bit  41      a start from the audio thread is pending (prepareStart)
    //   bits 42..63  seek generation: incremented by every message-thread command that sets the position, so
    //                the audio thread's advance sees a seek even when it stores the position the block started
    //                from (wraps after 2^22 seeks; harmless unless exactly that many happen during one block)
    static constexpr int positionBits = 40;
    static constexpr std::uint64_t positionMask = (std::uint64_t(1) << positionBits) - 1;
    static constexpr std::uint64_t playingBit = std::uint64_t(1) << positionBits;
    static constexpr std::uint64_t pendingBit = playingBit << 1;
    static constexpr std::uint64_t generationUnit = pendingBit << 1;
    static constexpr std::uint64_t generationMask = ~(generationUnit - 1);
    static SamplePos positionOf(std::uint64_t w) { return (SamplePos) (w & positionMask); }
    // The word after a message-thread seek to p: next generation, nothing pending unless in `flags`.
    static std::uint64_t seekWord(std::uint64_t w, SamplePos p, std::uint64_t flags);

    std::atomic<const MixSnapshot*> current { nullptr };
    // Odd while the audio thread is between beginBlock and endBlock (it may hold a snapshot pointer), even
    // otherwise. See setSnapshot for the reclamation argument.
    std::atomic<std::uint64_t> renderEpoch { 0 };
    std::atomic<std::uint64_t> transport { 0 };
    SamplePos playStart = 0; // message thread only
    std::shared_ptr<const MixSnapshot> latest;
    std::vector<Retired> retired;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlaybackEngine)
};
} // namespace aligna
