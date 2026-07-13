#include "TimelineView.h"

#include "UiMetrics.h"

#include <cstdio>
#include <memory>

namespace daw {

TimelineView::TimelineView(BRect frame, Project* project, CommandStack* stack)
    : BView(frame, "timeline", B_FOLLOW_ALL_SIDES,
            B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
      fProject(project),
      fStack(stack),
      fFramesPerPixel(kDefaultFramesPerPixel),
      fScrollFrame(0) {
    SetViewColor(ColBackground());
}

// --- Header control geometry (relative to a lane's top edge) ----------
static constexpr float kMaxGain = 1.5f;   // fader top of travel

static BRect MuteRect(BRect lane)  { return BRect(6,  lane.top + 20, 26,  lane.top + 38); }
static BRect SoloRect(BRect lane)  { return BRect(30, lane.top + 20, 50,  lane.top + 38); }
static BRect GainRect(BRect lane)  { return BRect(58, lane.top + 22, 154, lane.top + 34); }
static BRect PanRect(BRect lane)   { return BRect(58, lane.top + 40, 154, lane.top + 52); }

float TimelineView::FrameToX(Frame f) const {
    return kHeaderWidth
         + static_cast<float>((f - fScrollFrame) / fFramesPerPixel);
}

Frame TimelineView::XToFrame(float x) const {
    return fScrollFrame
         + static_cast<Frame>((x - kHeaderWidth) * fFramesPerPixel);
}

void TimelineView::Draw(BRect updateRect) {
    DrawLanes(updateRect);
    DrawPlayhead();          // over lanes, under the ruler
    DrawRuler(updateRect);   // ruler last so it sits above lane content
}

void TimelineView::SetPlayhead(Frame f) {
    if (f == fPlayhead)
        return;
    const float xOld = FrameToX(fPlayhead);
    const float xNew = FrameToX(f);
    fPlayhead = f;
    // Repaint the two 1-px columns (a hair wide for the AA'd line) from the
    // ruler bottom to the view bottom.
    BRect b = Bounds();
    Invalidate(BRect(xOld - 1, kRulerHeight, xOld + 1, b.bottom));
    Invalidate(BRect(xNew - 1, kRulerHeight, xNew + 1, b.bottom));
}

int TimelineView::TrackIndexAt(BPoint where) const {
    if (!fProject || where.y < kRulerHeight)
        return -1;
    const float rel = where.y - kRulerHeight;
    const int   idx = static_cast<int>(rel / (kTrackHeight + kTrackGap));
    if (idx < 0 || idx >= static_cast<int>(fProject->Tracks().size()))
        return -1;
    // Reject clicks that land in the gap between lanes.
    if (where.y > LaneRect(idx).bottom)
        return -1;
    return idx;
}

void TimelineView::MouseDown(BPoint where) {
    if (!fProject || !fStack)
        return;
    const int idx = TrackIndexAt(where);
    if (idx < 0)
        return;
    BRect lane = LaneRect(idx);
    const Track& t = fProject->Tracks()[idx];

    if (where.x < kHeaderWidth)
        HandleHeaderClick(t, lane, where);
    // Content-area clicks (clip drag, click-seek) arrive in later steps.
}

void TimelineView::HandleHeaderClick(const Track& t, BRect lane, BPoint where) {
    const TrackId id = t.id;
    std::unique_ptr<Command> cmd;

    if (MuteRect(lane).Contains(where)) {
        cmd = std::make_unique<SetTrackMuteCommand>(id, !t.muted);
    } else if (SoloRect(lane).Contains(where)) {
        cmd = std::make_unique<SetTrackSoloCommand>(id, !t.soloed);
    } else if (GainRect(lane).Contains(where)) {
        BRect g = GainRect(lane);
        float v = (where.x - g.left) / g.Width() * kMaxGain;
        if (v < 0) v = 0; if (v > kMaxGain) v = kMaxGain;
        cmd = std::make_unique<SetTrackGainCommand>(id, v);
    } else if (PanRect(lane).Contains(where)) {
        BRect pr = PanRect(lane);
        float v = ((where.x - pr.left) / pr.Width()) * 2.0f - 1.0f;
        if (v < -1) v = -1; if (v > 1) v = 1;
        cmd = std::make_unique<SetTrackPanCommand>(id, v);
    }

    if (cmd) {
        fStack->Execute(std::move(cmd), *fProject);
        Invalidate(lane);   // repaint just this lane's header
    }
}

void TimelineView::DrawPlayhead() {
    const float x = FrameToX(fPlayhead);
    if (x < kHeaderWidth || x > Bounds().right)
        return;
    SetHighColor(ColPlayhead());
    StrokeLine(BPoint(x, kRulerHeight), BPoint(x, Bounds().bottom));
}

void TimelineView::DrawRuler(BRect update) {
    BRect r = Bounds();
    r.bottom = kRulerHeight;

    SetHighColor(ColRuler());
    FillRect(r);

    if (!fProject)
        return;

    // Tick marks every second along the visible span. Convert the content's
    // left/right pixels back to frames, then step in one-second increments.
    const double rate = fProject->sampleRate;
    const Frame  secFrames = static_cast<Frame>(rate);
    const Frame  leftFrame  = XToFrame(kHeaderWidth);
    const Frame  rightFrame = XToFrame(Bounds().right);

    Frame firstSec = (leftFrame / secFrames) * secFrames;
    if (firstSec < 0) firstSec = 0;

    SetHighColor(ColGrid());
    SetLowColor(ColRuler());
    for (Frame f = firstSec; f <= rightFrame; f += secFrames) {
        const float x = FrameToX(f);
        if (x < kHeaderWidth) continue;
        StrokeLine(BPoint(x, 0), BPoint(x, kRulerHeight));

        char label[16];
        std::snprintf(label, sizeof(label), "%lld s",
                      static_cast<long long>(f / secFrames));
        SetHighColor(ColText());
        DrawString(label, BPoint(x + 3, kRulerHeight - 8));
        SetHighColor(ColGrid());
    }
}

BRect TimelineView::LaneRect(int index) const {
    const float top = kRulerHeight + index * (kTrackHeight + kTrackGap);
    return BRect(0, top, const_cast<TimelineView*>(this)->Bounds().right,
                 top + kTrackHeight);
}

void TimelineView::DrawLanes(BRect update) {
    if (!fProject)
        return;

    int idx = 0;
    for (const Track& t : fProject->Tracks()) {
        BRect lane = LaneRect(idx);

        SetHighColor((idx & 1) ? ColLaneAlt() : ColLane());
        FillRect(lane);

        // Second-grid lines through the lane content area.
        const double rate = fProject->sampleRate;
        const Frame  secFrames = static_cast<Frame>(rate);
        const Frame  leftFrame  = XToFrame(kHeaderWidth);
        const Frame  rightFrame = XToFrame(Bounds().right);
        Frame firstSec = (leftFrame / secFrames) * secFrames;
        if (firstSec < 0) firstSec = 0;
        SetHighColor(ColGrid());
        for (Frame f = firstSec; f <= rightFrame; f += secFrames) {
            const float x = FrameToX(f);
            if (x < kHeaderWidth) continue;
            StrokeLine(BPoint(x, lane.top), BPoint(x, lane.bottom));
        }

        for (const Clip& c : t.clips)
            DrawClip(c, lane);

        DrawTrackHeader(t, lane);   // header gutter on top of the lane

        idx++;
    }
}

// The left gutter for one track: name, mute/solo toggles, gain fader, pan bar.
// Custom-drawn (not BControls) so it stays pixel-aligned with the lane and
// needs no per-track child-view bookkeeping.
void TimelineView::DrawTrackHeader(const Track& t, BRect lane) {
    BRect hdr(0, lane.top, kHeaderWidth, lane.bottom);
    SetHighColor(ColHeader());
    FillRect(hdr);
    SetHighColor(ColGrid());
    StrokeLine(BPoint(kHeaderWidth, lane.top), BPoint(kHeaderWidth, lane.bottom));

    SetHighColor(ColText());
    DrawString(t.name.c_str(), BPoint(6, lane.top + 14));

    // Mute / Solo toggle boxes: filled when active.
    BRect m = MuteRect(lane), s = SoloRect(lane);
    SetHighColor(t.muted ? ColPlayhead() : ColLane());
    FillRect(m);
    SetHighColor(t.soloed ? Rgb(210, 190, 70) : ColLane());
    FillRect(s);
    SetHighColor(ColGrid());
    StrokeRect(m); StrokeRect(s);
    SetHighColor(ColText());
    DrawString("M", BPoint(m.left + 5, m.bottom - 5));
    DrawString("S", BPoint(s.left + 6, s.bottom - 5));

    // Gain fader: filled proportion = gain / kMaxGain.
    BRect g = GainRect(lane);
    SetHighColor(ColLane());  FillRect(g);
    float gf = t.gain / kMaxGain; if (gf < 0) gf = 0; if (gf > 1) gf = 1;
    BRect gfill = g; gfill.right = g.left + (g.Width()) * gf;
    SetHighColor(ColClip());  FillRect(gfill);
    SetHighColor(ColGrid());  StrokeRect(g);

    // Pan bar: a marker at the pan position, center line for reference.
    BRect pr = PanRect(lane);
    SetHighColor(ColLane());  FillRect(pr);
    const float cx = (pr.left + pr.right) * 0.5f;
    SetHighColor(ColGrid());
    StrokeLine(BPoint(cx, pr.top), BPoint(cx, pr.bottom));
    float px = cx + (t.pan * 0.5f) * pr.Width();
    SetHighColor(ColClipBorder());
    FillRect(BRect(px - 2, pr.top, px + 2, pr.bottom));
    SetHighColor(ColGrid());  StrokeRect(pr);
}

void TimelineView::DrawClip(const Clip& c, BRect lane) {
    float x0 = FrameToX(c.startFrame);
    float x1 = FrameToX(c.startFrame + c.lengthFrames);
    if (x1 < kHeaderWidth || x0 > lane.right)
        return;                       // fully outside the content area
    if (x0 < kHeaderWidth) x0 = kHeaderWidth;

    BRect block(x0, lane.top + 3, x1, lane.bottom - 3);
    SetHighColor(ColClip());
    FillRect(block);

    DrawClipWave(c, block);

    SetHighColor(ColClipBorder());
    StrokeRect(block);

    // Clip label (source file basename), clipped to the block width.
    const std::string& p = c.sourcePath;
    size_t slash = p.find_last_of('/');
    std::string name = (slash == std::string::npos) ? p : p.substr(slash + 1);
    SetHighColor(ColText());
    DrawString(name.c_str(), BPoint(block.left + 4, block.top + 14));
}

// Paint the min/max envelope inside a clip block: one vertical line per pixel
// column, from the column's min sample to its max. Reads a handful of peak
// buckets per column (never scans the audio). No cache for this clip's source
// -> just the flat filled block.
void TimelineView::DrawClipWave(const Clip& c, BRect block) {
    if (!fPeaks)
        return;
    auto it = fPeaks->find(c.sourcePath);
    if (it == fPeaks->end() || !it->second.IsValid())
        return;
    const PeakCache& pc = it->second;

    const float mid  = (block.top + block.bottom) * 0.5f;
    const float half = (block.bottom - block.top) * 0.5f - 1.0f;

    SetHighColor(ColWave());
    const int xL = static_cast<int>(block.left);
    const int xR = static_cast<int>(block.right);
    for (int x = xL; x <= xR; x++) {
        // Timeline frames this column spans -> source frames within the clip.
        const Frame tf0 = XToFrame(static_cast<float>(x));
        const Frame tf1 = XToFrame(static_cast<float>(x + 1));
        const Frame s0 = c.sourceOffset + (tf0 - c.startFrame);
        const Frame s1 = c.sourceOffset + (tf1 - c.startFrame);
        if (s1 <= 0)
            continue;

        Peak pk = pc.Range(s0 < 0 ? 0 : s0, s1);
        const float yMax = mid - pk.max * half;   // max amplitude -> up
        const float yMin = mid - pk.min * half;   // min amplitude -> down
        StrokeLine(BPoint(x, yMin), BPoint(x, yMax));
    }
}

} // namespace daw
