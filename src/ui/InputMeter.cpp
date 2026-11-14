#include "ui/InputMeter.h"
#include <cmath>

namespace aligna
{
void InputMeter::push(float peak, double dtMs)
{
    const float decay = (float) std::exp(-dtMs / 300.0); // ~300 ms time constant
    const float next = std::max(peak, shown * decay);
    if (std::abs(next - shown) > 0.002f)
        repaint();
    shown = next < 0.001f ? 0.0f : next;
}

void InputMeter::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff15161a));
    g.fillRoundedRectangle(r, 2.0f);
    // Linear amplitude shown on a dB scale: -60 dB .. 0 dB.
    const float db = juce::Decibels::gainToDecibels(shown, -60.0f);
    const float frac = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
    g.setColour(shown >= 0.98f ? juce::Colour(0xffe04040) : shown >= 0.5f ? juce::Colour(0xffe0b030) : juce::Colour(0xff3fbf6f));
    g.fillRoundedRectangle(r.reduced(1.0f).withWidth((r.getWidth() - 2.0f) * frac), 1.5f);
}
} // namespace aligna
