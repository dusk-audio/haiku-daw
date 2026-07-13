#include "TimelineView.h"

#include "UiMetrics.h"

#include <Window.h>

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
static BRect ArmRect(BRect lane)   { return BRect(54, lane.top + 20, 74,  lane.top + 38); }
static BRect GainRect(BRect lane)  { return BRect(80, lane.top + 22, 154, lane.top + 34); }
static BRect PanRect(BRect lane)   { return BRect(80, lane.top + 40, 154, lane.top + 52); }

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

    // Click in the ruler -> move the playhead (seek). Content x only.
    if (where.y < kRulerHeight && where.x >= kHeaderWidth) {
        Frame f = XToFrame(where.x);
        if (f < 0) f = 0;
        fProject->transport.playhead = f;
        SetPlayhead(f);
        if (BWindow* w = Window())
            w->PostMessage(kMsgSeek);
        return;
    }

    const int idx = TrackIndexAt(where);
    if (idx < 0)
        return;
    BRect lane = LaneRect(idx);
    const Track& t = fProject->Tracks()[idx];

    if (where.x < kHeaderWidth) {
        HandleHeaderClick(t, lane, where);
        return;
    }

    // Content area: start dragging a clip if the cursor is over one.
    const Frame at = XToFrame(where.x);
    for (const Clip& c : t.clips) {
        if (at >= c.startFrame && at < c.startFrame + c.lengthFrames) {
            fDrag           = Drag::Clip;
            fDragTrack      = t.id;
            fDragLane       = idx;
            fDragClip       = c.id;
            fDragClipOrig   = c.startFrame;
            fDragGrabOffset = at - c.startFrame;
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            break;
        }
    }
}

void TimelineView::HandleHeaderClick(const Track& t, BRect lane, BPoint where) {
    const TrackId id = t.id;

    // Mute / solo: immediate toggle commands.
    if (MuteRect(lane).Contains(where)) {
        fStack->Execute(std::make_unique<SetTrackMuteCommand>(id, !t.muted),
                        *fProject);
        Invalidate(lane);
        return;
    }
    if (SoloRect(lane).Contains(where)) {
        fStack->Execute(std::make_unique<SetTrackSoloCommand>(id, !t.soloed),
                        *fProject);
        Invalidate(lane);
        return;
    }
    if (ArmRect(lane).Contains(where)) {
        // Arm is transient transport state, not an undoable document edit:
        // toggle it directly. One input -> arming a track disarms the others
        // (single-input capture), so recording targets are unambiguous.
        if (Track* tr = fProject->FindTrack(id)) {
            const bool nowArmed = !tr->armed;
            for (Track& other : fProject->Tracks())
                other.armed = false;
            tr->armed = nowArmed;
            Invalidate();   // other lanes' arm boxes changed too
        }
        return;
    }

    // Gain / pan: begin a drag. Grab the pointer so we keep getting move/up
    // events even if the cursor leaves the fader.
    Drag mode = Drag::None;
    if (GainRect(lane).Contains(where))     mode = Drag::Gain;
    else if (PanRect(lane).Contains(where)) mode = Drag::Pan;
    if (mode == Drag::None)
        return;

    // TrackIndexAt() matched this lane, so this index is valid.
    fDragLane  = static_cast<int>((lane.top - kRulerHeight)
                                  / (kTrackHeight + kTrackGap) + 0.5f);
    fDrag      = mode;
    fDragTrack = id;
    fDragOrig  = (mode == Drag::Gain) ? t.gain : t.pan;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    PreviewDrag(where);
}

// Compute the dragged value from the cursor x and write it straight into the
// model for live feedback. This is a transient preview only; the undoable
// command is pushed in MouseUp.
void TimelineView::PreviewDrag(BPoint where) {
    Track* t = fProject->FindTrack(fDragTrack);
    if (!t) return;
    BRect lane = LaneRect(fDragLane);

    if (fDrag == Drag::Gain) {
        BRect g = GainRect(lane);
        float v = (where.x - g.left) / g.Width() * kMaxGain;
        if (v < 0) v = 0; if (v > kMaxGain) v = kMaxGain;
        t->gain = v;
    } else if (fDrag == Drag::Pan) {
        BRect pr = PanRect(lane);
        float v = ((where.x - pr.left) / pr.Width()) * 2.0f - 1.0f;
        if (v < -1) v = -1; if (v > 1) v = 1;
        t->pan = v;
    } else if (fDrag == Drag::Clip) {
        Clip* c = t->FindClip(fDragClip);
        if (c) {
            Frame start = XToFrame(where.x) - fDragGrabOffset;
            if (start < 0) start = 0;
            c->startFrame = start;   // preview only; sort fixed up on release
        }
    }
    Invalidate(lane);
}

void TimelineView::MouseMoved(BPoint where, uint32, const BMessage*) {
    if (fDrag != Drag::None)
        PreviewDrag(where);
}

void TimelineView::MouseUp(BPoint) {
    if (fDrag == Drag::None)
        return;
    Track* t = fProject->FindTrack(fDragTrack);
    if (t) {
        // Restore the pre-drag value, then apply the whole gesture as one
        // undoable command (which records the correct "old" value itself).
        std::unique_ptr<Command> cmd;
        if (fDrag == Drag::Gain) {
            const float v = t->gain;
            t->gain = fDragOrig;
            cmd = std::make_unique<SetTrackGainCommand>(fDragTrack, v);
        } else if (fDrag == Drag::Pan) {
            const float v = t->pan;
            t->pan = fDragOrig;
            cmd = std::make_unique<SetTrackPanCommand>(fDragTrack, v);
        } else if (fDrag == Drag::Clip) {
            if (Clip* c = t->FindClip(fDragClip)) {
                const Frame v = c->startFrame;
                c->startFrame = fDragClipOrig;
                cmd = std::make_unique<MoveClipCommand>(fDragTrack, fDragClip, v);
            }
        }
        if (cmd)
            fStack->Execute(std::move(cmd), *fProject);
    }
    fDrag = Drag::None;
    Invalidate(LaneRect(fDragLane));
}

void TimelineView::SetRecording(TrackId track, Frame start, Frame length) {
    fRecTrack = track;
    fRecStart = start;
    fRecLen   = length;
    Invalidate();   // simplest; the region grows every poll anyway
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

        // Live recording region on the armed track (grows each poll).
        if (t.id == fRecTrack && fRecLen > 0) {
            float rx0 = FrameToX(fRecStart);
            float rx1 = FrameToX(fRecStart + fRecLen);
            if (rx0 < kHeaderWidth) rx0 = kHeaderWidth;
            if (rx1 > lane.right)   rx1 = lane.right;
            if (rx1 > rx0) {
                BRect rb(rx0, lane.top + 3, rx1, lane.bottom - 3);
                SetHighColor(Rgb(150, 50, 50));
                FillRect(rb);
                SetHighColor(ColPlayhead());
                StrokeRect(rb);
                SetHighColor(ColText());
                DrawString("\xE2\x97\x8F REC", BPoint(rb.left + 4, rb.top + 14));
            }
        }

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

    // Mute / Solo / Arm toggle boxes: filled when active.
    BRect m = MuteRect(lane), s = SoloRect(lane), a = ArmRect(lane);
    SetHighColor(t.muted ? ColPlayhead() : ColLane());
    FillRect(m);
    SetHighColor(t.soloed ? Rgb(210, 190, 70) : ColLane());
    FillRect(s);
    SetHighColor(t.armed ? Rgb(220, 60, 60) : ColLane());
    FillRect(a);
    SetHighColor(ColGrid());
    StrokeRect(m); StrokeRect(s); StrokeRect(a);
    SetHighColor(ColText());
    DrawString("M", BPoint(m.left + 5, m.bottom - 5));
    DrawString("S", BPoint(s.left + 6, s.bottom - 5));
    DrawString("R", BPoint(a.left + 6, a.bottom - 5));

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

    // Timeline frames are at the project rate; the envelope indexes source
    // frames. Scale by source/project rate so the waveform tracks the audio
    // regardless of the file's sample rate.
    const double projRate = fProject->sampleRate;
    const double srcRate  = pc.SampleRate();
    const double toSrc = (srcRate > 0 && projRate > 0) ? srcRate / projRate : 1.0;

    SetHighColor(ColWave());
    const int xL = static_cast<int>(block.left);
    const int xR = static_cast<int>(block.right);
    for (int x = xL; x <= xR; x++) {
        // Timeline frames this column spans -> source frames within the clip.
        const Frame tf0 = XToFrame(static_cast<float>(x));
        const Frame tf1 = XToFrame(static_cast<float>(x + 1));
        const Frame s0 = c.sourceOffset + (Frame)((tf0 - c.startFrame) * toSrc);
        const Frame s1 = c.sourceOffset + (Frame)((tf1 - c.startFrame) * toSrc);
        if (s1 <= 0)
            continue;

        Peak pk = pc.Range(s0 < 0 ? 0 : s0, s1);
        const float yMax = mid - pk.max * half;   // max amplitude -> up
        const float yMin = mid - pk.min * half;   // min amplitude -> down
        StrokeLine(BPoint(x, yMin), BPoint(x, yMax));
    }
}

} // namespace daw
