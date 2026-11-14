#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace aligna
{
// Horizontal input level bar. Feed it the peak since the last call; it holds and decays it (~300 ms).
class InputMeter : public juce::Component
{
public:
    // dtMs: time since the previous call.
    void push(float peak, double dtMs);
    float level() const { return shown; }
    void paint(juce::Graphics&) override;

private:
    float shown = 0.0f;
};
} // namespace aligna
