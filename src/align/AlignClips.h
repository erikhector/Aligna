#pragma once
#include "align/Correlate.h"
#include "engine/Mixer.h"
#include <juce_core/juce_core.h>

namespace aligna
{
struct ClipAlignment
{
    // Where the target clip's first sample belongs. Negative when that is before the project start: the clip then
    // has to lose its first -newStart samples and start at 0 (applyAlignments does this). Never so far negative that
    // nothing of the clip would remain.
    SamplePos newStart;
    float confidence;
    bool polarityInverted;
};

constexpr double kMaxRegionSeconds = 60.0;
constexpr double kMaxShiftSecondsLimit = 300.0;

// Samples rendered between two cancellation checks (about 1 s of audio; a few milliseconds of work).
constexpr SamplePos kAlignRenderBlock = 48000;

// Renders `clip` alone (mono = channel average, unity gain, no fades) over timeline [from, from + n).
// Samples outside the clip's extent or the decoded buffer are 0. Rendered in blocks of blockSize samples;
// progress(0..1), if given, is asked before each block and returning false cancels (nullopt). The block size
// never changes the result.
std::optional<std::vector<float>> renderClipAlone(const MixSnapshot&, const Clip&, SamplePos from, SamplePos n,
                                                  const std::function<bool(float)>& progress = {},
                                                  SamplePos blockSize = kAlignRenderBlock);

// Works out where the target clip should start so that it lines up with the reference clip.
// Region: selection ∩ reference clip if a selection is given, else reference clip ∩ (target clip widened by
// maxShiftSeconds); capped to its first 60 s. Only works out a shift; never touches the project.
// Runs synchronously (call it from a background thread); progress returning false cancels (nullopt, error
// "Cancelled") within one render block or one stage of findOffset, and is not called again after that.
std::optional<ClipAlignment> computeAlignment(const MixSnapshot&, const juce::String& referenceClipId,
                                              const juce::String& targetClipId,
                                              std::optional<juce::Range<SamplePos>> selection, double maxShiftSeconds,
                                              const std::function<bool(float)>& progress, juce::String& error);
} // namespace aligna
