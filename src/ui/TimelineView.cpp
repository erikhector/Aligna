#include "ui/TimelineView.h"
#include "edit/ClipOps.h"
#include "model/Grid.h"
#include <algorithm>
#include <cmath>

namespace aligna
{
namespace
{
constexpr double thumbDirectBelow = 64.0; // below this samples-per-pixel, compute min/max straight from the buffer

juce::Colour trackColour(int i)
{
    static const float hues[] = { 0.55f, 0.08f, 0.33f, 0.80f, 0.15f, 0.48f };
    return juce::Colour::fromHSV(hues[(size_t) i % 6], 0.55f, 0.75f, 1.0f);
}

juce::String secondsLabel(double t)
{
    if (t <= 0.0)
        return "0";
    if (t >= 60.0)
        return juce::String::formatted("%d:%02d", (int) (t / 60.0), (int) std::fmod(t, 60.0));
    if (t >= 1.0 && std::abs(t - std::round(t)) < 1.0e-9)
        return juce::String((int) std::round(t)) + "s";
    if (t >= 1.0)
        return juce::String(t, 2) + "s";
    return juce::String(t * 1000.0, t * 1000.0 < 10.0 ? 1 : 0) + "ms";
}
} // namespace

TimelineView::TimelineView(AppController& a, TimelineState& s) : app(a), state(s)
{
    setOpaque(true);
    setWantsKeyboardFocus(true);
    syncThumbnails();
    startTimerHz(30);
}

TimelineView::~TimelineView() { stopTimer(); }

void TimelineView::syncThumbnails()
{
    const auto& proj = app.history().current();
    std::set<juce::String> live;
    for (auto& src : proj.sources)
    {
        live.insert(src.id);
        auto data = app.pool().get(src.id);
        auto it = thumbs.find(src.id);
        if (!data || data->getNumSamples() == 0)
        {
            if (it != thumbs.end())
                thumbs.erase(it);
            continue;
        }
        if (it != thumbs.end() && it->second.data == data)
            continue;
        Thumb t;
        t.data = data;
        t.thumb = std::make_unique<juce::AudioThumbnail>(64, formats, thumbCache);
        t.thumb->reset(data->getNumChannels(), proj.sampleRate, data->getNumSamples());
        t.thumb->addBlock(0, *data, 0, data->getNumSamples());
        thumbs[src.id] = std::move(t);
    }
    for (auto it = thumbs.begin(); it != thumbs.end();)
        it = live.count(it->first) ? std::next(it) : thumbs.erase(it);
}

void TimelineView::projectChanged()
{
    cancelDrag(); // the history changed under an in-progress drag
    syncThumbnails();
    clampView();
    repaint();
}

int TimelineView::maxViewY() const
{
    const int content = (int) app.history().current().tracks.size() * trackHeight;
    return std::max(0, content - (getHeight() - rulerHeight));
}

void TimelineView::clampView()
{
    const int y = std::clamp(viewY, 0, maxViewY());
    if (y != viewY)
    {
        viewY = y;
        if (onViewChanged)
            onViewChanged();
    }
    map.viewStart = std::max<SamplePos>(0, map.viewStart);
}

void TimelineView::setViewY(int y)
{
    viewY = y;
    clampView();
    if (onViewChanged)
        onViewChanged();
    repaint();
}

void TimelineView::resized() { clampView(); }

void TimelineView::repaintColumn(double x)
{
    if (x >= -2 && x <= getWidth() + 2)
        repaint(toPixelClamped(x, getWidth()) - 2, 0, 5, getHeight());
}

void TimelineView::timerCallback()
{
    auto& eng = app.engine();
    const auto pos = eng.getPosition();
    if (eng.isPlaying())
    {
        const double x = map.toX(pos);
        if (x < 0 || x >= getWidth())
        {
            map.viewStart = std::max<SamplePos>(0, pos - (SamplePos) (map.samplesPerPixel * getWidth() * 0.05));
            repaint();
        }
    }
    const double x = map.toX(pos);
    if (std::abs(x - lastPlayheadX) > 1.0e-6)
    {
        repaintColumn(lastPlayheadX);
        repaintColumn(x);
        lastPlayheadX = x;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------------------------------
void TimelineView::paint(juce::Graphics& g)
{
    const auto clip = g.getClipBounds();
    g.fillAll(juce::Colour(0xff1a1b1e));

    const auto& proj = shown();
    {
        juce::Graphics::ScopedSaveState ss(g);
        g.reduceClipRegion(0, rulerHeight, getWidth(), getHeight() - rulerHeight);
        for (int i = 0; i < (int) proj.tracks.size(); ++i)
        {
            const int top = rulerHeight + i * trackHeight - viewY;
            if (top + trackHeight < clip.getY() || top > clip.getBottom())
                continue;
            paintTrack(g, proj.tracks[(size_t) i], i, clip);
        }

        if (state.timeSelection)
        {
            const float x1 = (float) map.toX(state.timeSelection->getStart());
            const float x2 = (float) map.toX(state.timeSelection->getEnd());
            g.setColour(juce::Colours::white.withAlpha(0.12f));
            g.fillRect(x1, (float) rulerHeight, x2 - x1, (float) (getHeight() - rulerHeight));
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.drawVerticalLine(toPixelClamped(x1, getWidth()), (float) rulerHeight, (float) getHeight());
            g.drawVerticalLine(toPixelClamped(x2, getWidth()), (float) rulerHeight, (float) getHeight());
        }
    }

    paintRuler(g, clip);

    if (mouseX >= 0)
    {
        g.setColour(juce::Colours::white.withAlpha(0.3f));
        g.drawVerticalLine((int) mouseX, (float) rulerHeight, (float) getHeight());
    }

    const double px = map.toX(app.engine().getPosition());
    if (px >= -1 && px <= getWidth() + 1)
    {
        g.setColour(juce::Colour(0xffff5050));
        g.drawVerticalLine(toPixelClamped(px, getWidth()), 0.0f, (float) getHeight());
        juce::Path tri;
        tri.addTriangle((float) px - 5.0f, 0.0f, (float) px + 6.0f, 0.0f, (float) px + 0.5f, 8.0f);
        g.fillPath(tri);
    }
}

void TimelineView::paintRuler(juce::Graphics& g, juce::Rectangle<int> clip)
{
    if (clip.getY() >= rulerHeight)
        return;
    const auto& proj = shown();
    const int w = getWidth();
    g.setColour(juce::Colour(0xff26282c));
    g.fillRect(0, 0, w, rulerHeight);
    g.setColour(juce::Colour(0xff3a3d42));
    g.drawHorizontalLine(rulerHeight - 1, 0.0f, (float) w);
    g.drawHorizontalLine(rulerHeight / 2, 0.0f, (float) w);
    g.setFont(juce::FontOptions(11.0f));

    const int x0 = std::max(0, clip.getX()), x1 = std::min(w, clip.getRight());

    // Top row: bars and beats.
    const double spb = samplesPerBeat(proj);
    const double pxPerBeat = spb / map.samplesPerPixel;
    const int bpb = std::max(1, proj.beatsPerBar);
    const long long firstBeat = std::max(0LL, (long long) std::floor((double) map.toSample(x0) / spb) - 1);
    const long long lastBeat = (long long) std::ceil((double) map.toSample(x1) / spb) + 1;
    int barStride = 1;
    while (pxPerBeat * bpb * barStride < 48.0 && barStride < (1 << 20))
        barStride *= 2;
    for (long long b = firstBeat; b <= lastBeat; ++b)
    {
        const bool isBar = b % bpb == 0;
        const double x = map.toX((SamplePos) std::llround((double) b * spb));
        if (isBar)
        {
            const long long bar = b / bpb;
            if (bar % barStride != 0)
                continue;
            g.setColour(juce::Colours::white.withAlpha(0.7f));
            g.drawVerticalLine(toPixelClamped(x, getWidth()), 0.0f, (float) rulerHeight / 2.0f);
            g.drawText(juce::String(bar + 1), toPixelClamped(x, getWidth()) + 3, 0, 50, rulerHeight / 2,
                       juce::Justification::centredLeft, false);
        }
        else if (pxPerBeat >= 8.0 && barStride == 1)
        {
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.drawVerticalLine(toPixelClamped(x, getWidth()), (float) rulerHeight / 4.0f, (float) rulerHeight / 2.0f);
        }
    }

    // Bottom row: seconds.
    static const double steps[] = { 0.0001, 0.0002, 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
                                    1,      2,      5,      10,    15,    30,    60,   120,  300,  600,  1800, 3600 };
    const double pxPerSec = proj.sampleRate / map.samplesPerPixel;
    double step = steps[std::size(steps) - 1];
    for (double s : steps)
        if (s * pxPerSec >= 70.0)
        {
            step = s;
            break;
        }
    const double t0 = std::max(0.0, (double) map.toSample(x0) / proj.sampleRate);
    const double t1 = (double) map.toSample(x1) / proj.sampleRate;
    for (long long k = (long long) std::floor(t0 / step); (double) k * step <= t1 + step; ++k)
    {
        const double t = (double) k * step;
        const double x = map.toX((SamplePos) std::llround(t * proj.sampleRate));
        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.drawVerticalLine(toPixelClamped(x, getWidth()), (float) rulerHeight / 2.0f + 8.0f, (float) rulerHeight);
        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.drawText(secondsLabel(t), toPixelClamped(x, getWidth()) + 3, rulerHeight / 2, 70, rulerHeight / 2,
                   juce::Justification::centredLeft, false);
    }

    if (state.timeSelection)
    {
        const float a = (float) map.toX(state.timeSelection->getStart()), b = (float) map.toX(state.timeSelection->getEnd());
        g.setColour(juce::Colour(0xff4aa3ff).withAlpha(0.35f));
        g.fillRect(a, 0.0f, b - a, (float) rulerHeight);
    }
}

void TimelineView::paintTrack(juce::Graphics& g, const Track& track, int index, juce::Rectangle<int> clip)
{
    const int top = rulerHeight + index * trackHeight - viewY;
    const juce::Rectangle<int> lane(0, top, getWidth(), trackHeight);
    g.setColour(index % 2 == 0 ? juce::Colour(0xff1f2024) : juce::Colour(0xff222328));
    g.fillRect(lane);
    g.setColour(juce::Colour(0xff34363b));
    g.drawHorizontalLine(lane.getBottom() - 1, 0.0f, (float) getWidth());

    const auto& proj = shown();
    const auto colour = trackColour(index);

    // Mixer overlap order: earlier start first, later on top (stable for equal starts).
    std::vector<const Clip*> order;
    for (auto& c : track.clips)
        order.push_back(&c);
    std::stable_sort(order.begin(), order.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });

    for (size_t i = 0; i < order.size(); ++i)
    {
        const auto& c = *order[i];
        const int l = toPixelClamped(map.toX(c.start), getWidth()), r = toPixelClamped(map.toX(c.start + c.length), getWidth());
        if (r < clip.getX() || l > clip.getRight())
            continue;
        const juce::Rectangle<int> rect(l, top + 2, std::max(1, r - l), trackHeight - 4);

        // Covered by later-starting clips on this track.
        juce::RectangleList<int> covered;
        for (size_t j = i + 1; j < order.size(); ++j)
        {
            const auto& o = *order[j];
            const int ol = toPixelClamped(map.toX(o.start), getWidth()), orr = toPixelClamped(map.toX(o.start + o.length), getWidth());
            covered.addWithoutMerging(rect.getIntersection({ ol, rect.getY(), std::max(0, orr - ol), rect.getHeight() }));
        }
        covered.consolidate();
        juce::RectangleList<int> uncovered(rect);
        uncovered.subtract(covered);

        const auto* src = findSource(proj, c.sourceId);
        const bool missing = src == nullptr || src->missing || app.pool().get(c.sourceId) == nullptr;

        for (int pass = 0; pass < 2; ++pass)
        {
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(pass == 0 ? uncovered : covered);
            if (g.isClipEmpty())
                continue;
            paintClip(g, c, rect, clip, pass == 0 ? 1.0f : 0.4f, colour, missing);
        }
    }
}

void TimelineView::paintClip(juce::Graphics& g, const Clip& c, juce::Rectangle<int> r, juce::Rectangle<int> clip,
                             float alpha, juce::Colour colour, bool missing)
{
    if (missing)
    {
        g.setColour(juce::Colour(0xffa83232).withAlpha(alpha));
        g.fillRect(r);
        g.setColour(juce::Colours::white.withAlpha(alpha));
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText("missing", r.reduced(6, 4).withLeft(std::max(r.getX(), 0) + 6), juce::Justification::centredLeft, true);
    }
    else
    {
        g.setColour(colour.darker(0.75f).withAlpha(0.9f * alpha));
        g.fillRect(r);
        paintWaveform(g, c, r, clip, alpha, colour);
    }

    // Fades.
    const double fx = map.toX(c.start + c.fadeIn), tx = map.toX(c.start + c.length - c.fadeOut);
    g.setColour(juce::Colours::white.withAlpha(0.7f * alpha));
    if (c.fadeIn > 0)
        g.drawLine((float) r.getX(), (float) r.getBottom(), (float) fx, (float) r.getY() + 1.0f, 1.0f);
    if (c.fadeOut > 0)
        g.drawLine((float) tx, (float) r.getY() + 1.0f, (float) r.getRight(), (float) r.getBottom(), 1.0f);

    // Border (selection / align reference).
    const bool selected = state.selectedClipIds.count(c.id) > 0;
    const bool ref = c.id == state.alignReferenceClipId;
    g.setColour(ref ? juce::Colour(0xffff9800) : selected ? juce::Colours::white : colour.withAlpha(0.8f * alpha));
    g.drawRect(r, (selected || ref) ? 2 : 1);

    if (!missing)
    {
        const auto* src = findSource(shown(), c.sourceId);
        if (src != nullptr && r.getWidth() > 60)
        {
            g.setColour(juce::Colours::white.withAlpha(0.85f * alpha));
            g.setFont(juce::FontOptions(11.0f));
            g.drawText(juce::File(src->path).getFileName(), r.getX() + 4, r.getY() + 1, r.getWidth() - 8, 14,
                       juce::Justification::centredLeft, true);
        }
    }
}

void TimelineView::paintWaveform(juce::Graphics& g, const Clip& c, juce::Rectangle<int> r, juce::Rectangle<int> clip,
                                 float alpha, juce::Colour colour)
{
    auto it = thumbs.find(c.sourceId);
    if (it == thumbs.end())
        return;
    const auto& buf = *it->second.data;
    const int nch = buf.getNumChannels();
    const int vis0 = std::max({ r.getX(), clip.getX(), 0 }), vis1 = std::min({ r.getRight(), clip.getRight(), getWidth() });
    if (vis1 <= vis0)
        return;
    const double rate = shown().sampleRate;
    const auto body = r.reduced(0, 3);
    const int bandH = body.getHeight() / std::max(1, std::min(nch, 2));
    const double spp = map.samplesPerPixel;
    g.setColour(colour.withAlpha(alpha));

    for (int ch = 0; ch < std::min(nch, 2); ++ch)
    {
        const juce::Rectangle<int> band(body.getX(), body.getY() + ch * bandH, body.getWidth(), bandH);
        const float mid = (float) band.getCentreY(), half = (float) band.getHeight() * 0.5f - 1.0f;

        if (spp >= thumbDirectBelow)
        {
            const double t0 = (double) (c.sourceOffset + (map.toSample(vis0) - c.start)) / rate;
            const double t1 = (double) (c.sourceOffset + (map.toSample(vis1) - c.start)) / rate;
            // drawChannel maps [t0,t1] onto the rectangle, so give it exactly the visible pixel span.
            it->second.thumb->drawChannel(g, { vis0, band.getY(), vis1 - vis0, band.getHeight() }, t0, t1, ch, 1.0f);
            continue;
        }

        const float* d = buf.getReadPointer(ch);
        const auto len = (SamplePos) buf.getNumSamples();

        if (spp >= 1.0)
        {
            for (int x = vis0; x < vis1; ++x)
            {
                SamplePos a = c.sourceOffset + (map.toSample(x) - c.start);
                SamplePos b = c.sourceOffset + (map.toSample(x + 1) - c.start);
                a = std::clamp<SamplePos>(a, 0, len);
                b = std::clamp<SamplePos>(std::max(b, a + 1), 0, len);
                if (b <= a)
                    continue;
                float lo = d[a], hi = d[a];
                for (SamplePos s = a + 1; s < b; ++s)
                {
                    lo = std::min(lo, d[s]);
                    hi = std::max(hi, d[s]);
                }
                g.fillRect((float) x, mid - hi * half, 1.0f, std::max(1.0f, (hi - lo) * half));
            }
        }
        else
        {
            // Sub-pixel zoom: sample values as points joined by lines.
            const SamplePos sA = std::max<SamplePos>(0, c.sourceOffset + (map.toSample(vis0) - c.start) - 1);
            const SamplePos sB = std::min<SamplePos>(std::min(len, c.sourceOffset + c.length), c.sourceOffset + (map.toSample(vis1) - c.start) + 2);
            juce::Path path;
            bool first = true;
            for (SamplePos s = sA; s < sB; ++s)
            {
                const float x = (float) map.toX(c.start + (s - c.sourceOffset));
                const float y = mid - d[s] * half;
                if (first) { path.startNewSubPath(x, y); first = false; }
                else path.lineTo(x, y);
            }
            g.strokePath(path, juce::PathStrokeType(1.0f));
            if (spp < 0.25)
                for (SamplePos s = sA; s < sB; ++s)
                {
                    const float x = (float) map.toX(c.start + (s - c.sourceOffset));
                    g.fillEllipse(x - 2.0f, mid - d[s] * half - 2.0f, 4.0f, 4.0f);
                }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------------------------------------------------
namespace
{
constexpr int dragThreshold = 3; // pixels before a click becomes a drag

juce::MouseCursor cursorFor(HitZone z)
{
    switch (z)
    {
        case HitZone::TrimStart:
        case HitZone::TrimEnd: return juce::MouseCursor::LeftRightResizeCursor;
        case HitZone::FadeIn:
        case HitZone::FadeOut: return juce::MouseCursor::CrosshairCursor;
        case HitZone::Body: return juce::MouseCursor::DraggingHandCursor;
        case HitZone::None: break;
    }
    return juce::MouseCursor::NormalCursor;
}

class GainPopup : public juce::Component
{
public:
    GainPopup(float initialDb, std::function<void(float)> commitFn) : commit(std::move(commitFn))
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
        slider.setRange(-60.0, 12.0, 0.1);
        slider.setDoubleClickReturnValue(true, 0.0);
        slider.setTextValueSuffix(" dB");
        slider.setValue((double) initialDb, juce::dontSendNotification);
        slider.onDragEnd = [this] { commit((float) slider.getValue()); };
        // Typed values and key/wheel changes arrive without a drag; during a drag only the drag end commits.
        slider.onValueChange = [this] {
            if (! slider.isMouseButtonDown())
                commit((float) slider.getValue());
        };
        addAndMakeVisible(slider);
        setSize(280, 40);
    }
    void resized() override { slider.setBounds(getLocalBounds().reduced(8, 8)); }

private:
    juce::Slider slider;
    std::function<void(float)> commit;
};
} // namespace

bool TimelineView::Drag::changed() const
{
    switch (mode)
    {
        case DragMode::Move: return timeDelta != 0 || trackDelta != 0;
        case DragMode::TrimStart:
        case DragMode::TrimEnd:
        case DragMode::FadeIn:
        case DragMode::FadeOut: return value != origEdge;
        case DragMode::None: break;
    }
    return false;
}

bool TimelineView::Drag::apply(Project& p) const
{
    bool ok = false;
    switch (mode)
    {
        case DragMode::Move:
            for (size_t i = 0; i < ids.size(); ++i)
                ok = moveClip(p, ids[i], starts[i] + timeDelta, tracks[i] + trackDelta) || ok;
            break;
        case DragMode::TrimStart: ok = trimClipStart(p, clipId, value); break;
        case DragMode::TrimEnd: ok = trimClipEnd(p, clipId, value); break;
        case DragMode::FadeIn: ok = setClipFades(p, clipId, value, origOther); break;
        case DragMode::FadeOut: ok = setClipFades(p, clipId, origOther, value); break;
        case DragMode::None: break;
    }
    return ok;
}

void TimelineView::cancelDrag()
{
    const bool had = drag.mode != DragMode::None || preview.has_value();
    drag = {};
    preview.reset();
    if (had)
        repaint();
}

void TimelineView::setPlayhead(SamplePos s) { app.engine().setPosition(std::max<SamplePos>(0, s)); }

void TimelineView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    draggingRuler = false;
    selecting = false;
    cancelDrag();
    if (! e.mods.isLeftButtonDown()) // only the left button starts drags / ruler actions
    {
        if (e.mods.isPopupMenu() && e.y >= rulerHeight)
        {
            const auto hit = aligna::hitTest(app.history().current(), map, trackHeight,
                                             { (float) e.x, (float) (e.y - rulerHeight + viewY) });
            if (hit.zone != HitZone::None)
            {
                juce::PopupMenu m;
                m.addItem(1, "Set as align reference", true, state.alignReferenceClipId == hit.clipId);
                m.showMenuAsync(juce::PopupMenu::Options(), [this, id = hit.clipId](int r) {
                    if (r == 1)
                    {
                        state.alignReferenceClipId = id;
                        repaint();
                    }
                    grabKeyboardFocus(); // shortcuts keep working after the popup closes
                });
            }
        }
        return;
    }
    if (e.y < rulerHeight)
    {
        draggingRuler = true;
        const auto s = std::max<SamplePos>(0, map.toSample(e.x));
        if (e.mods.isShiftDown())
        {
            selecting = true;
            selectAnchor = s;
            state.timeSelection = juce::Range<SamplePos>(s, s);
            repaint();
        }
        else
            setPlayhead(s);
        return;
    }

    const auto& proj = app.history().current();
    const auto h = aligna::hitTest(proj, map, trackHeight, { (float) e.x, (float) (e.y - rulerHeight + viewY) });
    if (h.zone == HitZone::None)
    {
        if (! e.mods.isShiftDown() && ! state.selectedClipIds.empty())
        {
            state.selectedClipIds.clear();
            repaint();
        }
        return;
    }

    const bool wasSelected = state.selectedClipIds.count(h.clipId) > 0;
    if (e.mods.isShiftDown())
    {
        if (wasSelected)
            state.selectedClipIds.erase(h.clipId);
        else
            state.selectedClipIds.insert(h.clipId);
        repaint();
        if (wasSelected)
            return; // shift-click on a selected clip only deselects it
    }
    else if (! wasSelected || h.zone != HitZone::Body)
    {
        state.selectedClipIds = std::set<juce::String> { h.clipId };
        repaint();
    }

    Project base = proj;
    auto* c = findClip(base, h.clipId);
    if (c == nullptr)
        return;
    drag.down = e.getPosition();
    drag.clipId = h.clipId;
    switch (h.zone)
    {
        case HitZone::Body:
        {
            drag.mode = DragMode::Move;
            drag.name = "Move";
            drag.collapseOnUp = wasSelected && state.selectedClipIds.size() > 1 && ! e.mods.isShiftDown();
            for (auto& t : base.tracks)
                for (auto& cl : t.clips)
                    if (state.selectedClipIds.count(cl.id) > 0)
                    {
                        if (cl.id == h.clipId)
                            drag.anchor = drag.ids.size();
                        drag.ids.push_back(cl.id);
                        drag.starts.push_back(cl.start);
                    }
            for (auto& id : drag.ids)
            {
                int ti = 0;
                findClip(base, id, &ti);
                drag.tracks.push_back(ti);
            }
            break;
        }
        case HitZone::TrimStart: drag.mode = DragMode::TrimStart; drag.name = "Trim"; drag.origEdge = c->start; break;
        case HitZone::TrimEnd: drag.mode = DragMode::TrimEnd; drag.name = "Trim"; drag.origEdge = c->start + c->length; break;
        case HitZone::FadeIn:
            drag.mode = DragMode::FadeIn; drag.name = "Fade"; drag.origEdge = c->fadeIn; drag.origOther = c->fadeOut; break;
        case HitZone::FadeOut:
            drag.mode = DragMode::FadeOut; drag.name = "Fade"; drag.origEdge = c->fadeOut; drag.origOther = c->fadeIn; break;
        case HitZone::None: break;
    }
    drag.value = drag.origEdge;
    setMouseCursor(cursorFor(h.zone));
}

void TimelineView::updateDrag(const juce::MouseEvent& e)
{
    const auto& base = app.history().current();
    const bool doSnap = state.snapEnabled && ! e.mods.isAltDown();
    const double step = snapStep(base, state.subdivisionsPerBeat);
    const auto dx = (SamplePos) std::llround((double) (e.x - drag.down.x) * map.samplesPerPixel);

    switch (drag.mode)
    {
        case DragMode::Move:
        {
            std::vector<DragClip> clips;
            for (size_t i = 0; i < drag.ids.size(); ++i)
                clips.push_back({ drag.starts[i], drag.tracks[i] });
            const double ty = (double) (e.y - rulerHeight + viewY) / (double) trackHeight;
            const double ty0 = (double) (drag.down.y - rulerHeight + viewY) / (double) trackHeight;
            const int rawTracks = (int) std::floor(ty) - (int) std::floor(ty0);
            const auto d = computeMoveDelta(clips, drag.anchor, dx, rawTracks, (int) base.tracks.size(), doSnap, step);
            drag.timeDelta = d.time;
            drag.trackDelta = d.tracks;
            break;
        }
        case DragMode::TrimStart:
        case DragMode::TrimEnd: drag.value = snapEdge(drag.origEdge + dx, doSnap, step); break;
        case DragMode::FadeIn: drag.value = std::max<SamplePos>(0, drag.origEdge + dx); break;
        case DragMode::FadeOut: drag.value = std::max<SamplePos>(0, drag.origEdge - dx); break;
        case DragMode::None: return;
    }
    Project p = base;
    drag.apply(p);
    preview = std::move(p);
    repaint();
}

void TimelineView::mouseDrag(const juce::MouseEvent& e)
{
    mouseX = e.x;
    if (draggingRuler)
    {
        const auto s = std::max<SamplePos>(0, map.toSample(e.x));
        if (selecting)
        {
            state.timeSelection = juce::Range<SamplePos>(std::min(s, selectAnchor), std::max(s, selectAnchor));
            repaint();
        }
        else
            setPlayhead(s);
        return;
    }
    if (drag.mode == DragMode::None)
        return;
    if (! drag.active)
    {
        if (std::abs(e.x - drag.down.x) < dragThreshold && std::abs(e.y - drag.down.y) < dragThreshold)
            return;
        drag.active = true;
    }
    updateDrag(e);
}

void TimelineView::mouseUp(const juce::MouseEvent&)
{
    if (selecting && state.timeSelection && state.timeSelection->isEmpty())
    {
        state.timeSelection.reset();
        repaint();
    }
    draggingRuler = selecting = false;

    if (drag.mode == DragMode::None)
        return;
    Drag d = std::move(drag);
    cancelDrag(); // drops the preview before the history change re-enters projectChanged()
    if (! d.active)
    {
        if (d.collapseOnUp)
        {
            state.selectedClipIds = std::set<juce::String> { d.clipId };
            repaint();
        }
        return;
    }
    if (d.changed())
        app.history().perform(d.name, [d](Project& p) {
            const Project before = p;
            // Clamping can bring the result back to the original (e.g. trim snapped past source start): no edit then.
            return d.apply(p) && ! sameClips(before, p);
        });
    repaint();
}

void TimelineView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.y < rulerHeight)
        return;
    const auto h = aligna::hitTest(app.history().current(), map, trackHeight, { (float) e.x, (float) (e.y - rulerHeight + viewY) });
    if (h.zone != HitZone::None)
        showGainPopup(e, h.clipId);
}

void TimelineView::showGainPopup(const juce::MouseEvent& e, const juce::String& clipId)
{
    Project copy = app.history().current();
    const auto* c = findClip(copy, clipId);
    if (c == nullptr)
        return;
    auto id = clipId;
    auto content = std::make_unique<GainPopup>(c->gainDb, [this, id](float db) {
        app.history().perform("Clip Gain", [&](Project& p) {
            auto* cl = findClip(p, id);
            if (cl == nullptr || std::abs(cl->gainDb - juce::jlimit(-60.0f, 12.0f, db)) < 1.0e-4f)
                return false;
            return setClipGain(p, id, db);
        });
    });
    const auto screen = e.getScreenPosition();
    juce::CallOutBox::launchAsynchronously(std::move(content), juce::Rectangle<int>(screen.x, screen.y, 1, 1), nullptr);
}

void TimelineView::mouseMove(const juce::MouseEvent& e)
{
    repaintColumn(mouseX);
    mouseX = e.x;
    repaintColumn(mouseX);

    juce::MouseCursor cur = juce::MouseCursor::NormalCursor;
    if (e.y >= rulerHeight)
        cur = cursorFor(aligna::hitTest(shown(), map, trackHeight, { (float) e.x, (float) (e.y - rulerHeight + viewY) }).zone);
    setMouseCursor(cur);
}

void TimelineView::mouseExit(const juce::MouseEvent&)
{
    repaintColumn(mouseX);
    mouseX = -1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Keyboard editing
// ---------------------------------------------------------------------------------------------------------------------
bool TimelineView::nudgeSelected(bool right, const juce::ModifierKeys& mods)
{
    if (state.selectedClipIds.empty())
        return false;
    const auto& proj = app.history().current();
    SamplePos amount;
    if (mods.isCommandDown() || mods.isCtrlDown())
        amount = std::max<SamplePos>(1, (SamplePos) std::llround(snapStep(proj, state.subdivisionsPerBeat)));
    else if (mods.isAltDown())
        amount = nudgeAmount(NudgeSize::Sample, proj, state.subdivisionsPerBeat);
    else if (mods.isShiftDown())
        amount = nudgeAmount(NudgeSize::TenMs, proj, state.subdivisionsPerBeat);
    else
        amount = nudgeAmount(NudgeSize::OneMs, proj, state.subdivisionsPerBeat);

    std::vector<SamplePos> starts;
    for (auto& t : proj.tracks)
        for (auto& c : t.clips)
            if (state.selectedClipIds.count(c.id) > 0)
                starts.push_back(c.start);
    const auto delta = limitNudge(starts, right ? amount : -amount);
    if (delta == 0)
        return true;
    const auto ids = state.selectedClipIds;
    app.history().perform("Nudge", [&](Project& p) {
        bool ok = false;
        for (auto& id : ids)
        {
            int ti = 0;
            if (auto* c = findClip(p, id, &ti))
                ok = moveClip(p, id, c->start + delta, ti) || ok;
        }
        return ok;
    });
    return true;
}

bool TimelineView::splitSelected()
{
    if (state.selectedClipIds.empty())
        return false;
    const auto pos = app.engine().getPosition();
    const auto ids = state.selectedClipIds;
    app.history().perform("Split", [&](Project& p) {
        bool ok = false;
        for (auto& id : ids)
            ok = splitClip(p, id, pos).has_value() || ok; // clips not under the playhead are ignored
        return ok;
    });
    return true;
}

bool TimelineView::deleteSelected()
{
    if (state.selectedClipIds.empty())
        return false;
    const auto ids = state.selectedClipIds;
    state.selectedClipIds.clear();
    app.history().perform("Delete", [&](Project& p) {
        bool ok = false;
        for (auto& id : ids)
            ok = deleteClip(p, id) || ok;
        return ok;
    });
    repaint();
    return true;
}

bool TimelineView::keyPressed(const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    const int code = k.getKeyCode();
    if (code == juce::KeyPress::escapeKey && drag.mode != DragMode::None)
    {
        cancelDrag();
        return true;
    }
    if (drag.mode != DragMode::None)
        return true; // no edits while a drag is in progress
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
        return nudgeSelected(code == juce::KeyPress::rightKey, mods);
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
        return deleteSelected();
    if (! mods.isCommandDown() && ! mods.isCtrlDown() && ! mods.isAltDown())
    {
        const auto ch = juce::CharacterFunctions::toLowerCase(k.getTextCharacter());
        if (ch == 's')
            return splitSelected();
        if (ch == 'g')
        {
            state.snapEnabled = ! state.snapEnabled;
            return true;
        }
    }
    return false;
}

void TimelineView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        const double dy = std::clamp((double) w.deltaY, -1.0, 1.0);
        map.zoomAround(e.x, std::exp(-dy * 2.0));
        repaint();
    }
    else if (e.mods.isShiftDown())
    {
        const double d = std::abs(w.deltaY) > 0.0f ? w.deltaY : w.deltaX;
        map.viewStart = std::max<SamplePos>(0, map.viewStart - (SamplePos) std::llround(d * 300.0 * map.samplesPerPixel));
        repaint();
    }
    else
    {
        if (std::abs(w.deltaX) > 0.0f)
        {
            map.viewStart = std::max<SamplePos>(0, map.viewStart - (SamplePos) std::llround(w.deltaX * 300.0 * map.samplesPerPixel));
            repaint();
        }
        if (std::abs(w.deltaY) > 0.0f)
            setViewY(viewY - (int) std::lround(w.deltaY * 300.0));
    }
}
} // namespace aligna
