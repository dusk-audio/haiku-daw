#include "TimelineView.h"

#include "UiMetrics.h"

#include <MenuItem.h>
#include <PopUpMenu.h>
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

// MIDI piano-roll vertical range: pitches [kMidiLow, kMidiLow+kMidiRange).
static constexpr int kMidiLow   = 36;
static constexpr int kMidiRange = 48;

// How close (px) to a block's right edge counts as a resize grab.
static constexpr float kEdgeGrab = 5.0f;

// Edits snap to this grid resolution (16th notes) unless Shift is held.
static constexpr int kSnapDivision = 4;

static BRect FxRect(BRect lane)    { return BRect(116, lane.top + 2,  152, lane.top + 17); }
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

Grid TimelineView::GridOf() const {
    Grid g;
    g.sampleRate  = fProject->sampleRate;
    g.tempoBPM    = fProject->tempoBPM;
    g.beatsPerBar = fProject->timeSig.numerator;
    return g;
}

Frame TimelineView::Snapped(Frame f) const {
    if (modifiers() & B_SHIFT_KEY)   // hold Shift for free placement
        return f;
    return GridOf().Snap(f, kSnapDivision);
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

int TimelineView::PitchAt(BRect lane, float y) const {
    const float rel = (lane.bottom - y) / lane.Height();
    int pitch = kMidiLow + (int)(rel * kMidiRange + 0.5f);
    if (pitch < 0) pitch = 0;
    if (pitch > 127) pitch = 127;
    return pitch;
}

int TimelineView::NoteIndexAt(const Track& t, BRect lane, BPoint where) const {
    const float h = lane.Height();
    for (size_t i = 0; i < t.notes.size(); i++) {
        const MidiNote& n = t.notes[i];
        float x0 = FrameToX(n.startFrame);
        float x1 = FrameToX(n.startFrame + n.lengthFrames);
        int p = n.pitch - kMidiLow;
        if (p < 0) p = 0;
        if (p >= kMidiRange) p = kMidiRange - 1;
        const float ny = lane.bottom - (float)p / kMidiRange * h;
        const float nh = h / kMidiRange + 1.0f;
        BRect r(x0, ny - nh, x1, ny);
        r.InsetBy(-2, -2);   // a little slop for easy clicking
        if (r.Contains(where))
            return (int)i;
    }
    return -1;
}

void TimelineView::MouseDown(BPoint where) {
    if (!fProject || !fStack)
        return;

    // Secondary (right) button = delete the thing under the cursor.
    int32 buttons = 0;
    if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
        m->FindInt32("buttons", &buttons);
    const bool rightClick = (buttons & B_SECONDARY_MOUSE_BUTTON) != 0;

    // Click in the ruler -> move the playhead (seek). Content x only.
    if (where.y < kRulerHeight && where.x >= kHeaderWidth) {
        Frame f = Snapped(XToFrame(where.x));
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

    // MIDI track content: right-click deletes; on a note, drag to move or
    // (near its right edge) resize; on empty space, add a note.
    if (t.type == TrackType::Midi) {
        const int hit = NoteIndexAt(t, lane, where);
        if (rightClick) {
            if (hit >= 0)
                fStack->Execute(std::make_unique<RemoveNoteCommand>(t.id, (size_t)hit),
                                *fProject);
            Invalidate(lane);
            return;
        }
        if (hit >= 0) {
            const MidiNote& n = t.notes[(size_t)hit];
            const float xStart = FrameToX(n.startFrame);
            const float xEnd   = FrameToX(n.startFrame + n.lengthFrames);
            const bool  wide   = (xEnd - xStart) > 2 * kEdgeGrab;
            fDragTrack     = t.id;
            fDragLane      = idx;
            fDragNote      = hit;
            fDragNoteOrig  = n;
            if (wide && where.x >= xEnd - kEdgeGrab)
                fDrag = Drag::NoteResize;
            else {
                fDrag = Drag::Note;
                fDragGrabOffset  = XToFrame(where.x) - n.startFrame;
                fDragPitchOffset = n.pitch - PitchAt(lane, where.y);
            }
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            return;
        }
        Frame time = Snapped(XToFrame(where.x));
        if (time < 0) time = 0;
        MidiNote n;
        n.pitch = PitchAt(lane, where.y);
        n.velocity = 100;
        n.startFrame = time;
        // Default length = one beat, so added notes land on the grid.
        n.lengthFrames = (Frame)GridOf().FramesPerBeat();
        fStack->Execute(std::make_unique<AddNoteCommand>(t.id, n), *fProject);
        Invalidate(lane);
        return;
    }

    // Content area: right-click deletes the clip under the cursor; otherwise
    // start dragging it.
    const Frame at = XToFrame(where.x);
    for (const Clip& c : t.clips) {
        if (at >= c.startFrame && at < c.startFrame + c.lengthFrames) {
            if (rightClick) {
                fStack->Execute(std::make_unique<RemoveClipCommand>(t.id, c.id),
                                *fProject);
                Invalidate(lane);
                return;
            }
            fDragTrack       = t.id;
            fDragLane        = idx;
            fDragClip        = c.id;
            fDragClipOrig    = c.startFrame;
            fDragClipOrigLen = c.lengthFrames;
            const float xStart = FrameToX(c.startFrame);
            const float xEnd   = FrameToX(c.startFrame + c.lengthFrames);
            const bool  wide   = (xEnd - xStart) > 2 * kEdgeGrab;
            if (wide && where.x >= xEnd - kEdgeGrab) {
                fDrag = Drag::ClipResize;
            } else {
                fDrag = Drag::Clip;
                fDragGrabOffset = at - c.startFrame;
            }
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
    if (FxRect(lane).Contains(where)) {
        // Effect picker. Effects apply on the next Play (chain built at Load).
        BPopUpMenu* menu = new BPopUpMenu("fx", false, false);
        menu->AddItem(new BMenuItem("Add Low-pass", NULL));
        menu->AddItem(new BMenuItem("Add High-pass", NULL));
        menu->AddItem(new BMenuItem("Add Delay", NULL));
        menu->AddSeparatorItem();
        menu->AddItem(new BMenuItem("Clear Effects", NULL));
        BMenuItem* sel = menu->Go(ConvertToScreen(where), false, true);
        const int32 pick = sel ? menu->IndexOf(sel) : -1;
        delete menu;

        std::unique_ptr<Command> cmd;
        switch (pick) {
            case 0: cmd = std::make_unique<AddEffectCommand>(id, LowPassDesc(800.0f)); break;
            case 1: cmd = std::make_unique<AddEffectCommand>(id, HighPassDesc(200.0f)); break;
            case 2: cmd = std::make_unique<AddEffectCommand>(id, DelayDesc()); break;
            case 4: if (!t.fx.empty()) cmd = std::make_unique<ClearEffectsCommand>(id); break;
            default: break;
        }
        if (cmd) {
            fStack->Execute(std::move(cmd), *fProject);
            Invalidate(lane);
        }
        return;
    }
    if (ArmRect(lane).Contains(where)) {
        // Arm is transient transport state, not an undoable document edit:
        // toggle it directly. Multiple tracks may be armed; recording writes
        // one take from the single input and drops it on every armed track.
        if (Track* tr = fProject->FindTrack(id))
            tr->armed = !tr->armed;
        Invalidate(lane);
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
            Frame start = Snapped(XToFrame(where.x) - fDragGrabOffset);
            if (start < 0) start = 0;
            c->startFrame = start;   // preview only; sort fixed up on release
        }
    } else if (fDrag == Drag::ClipResize) {
        Clip* c = t->FindClip(fDragClip);
        if (c) {
            Frame len = Snapped(XToFrame(where.x)) - c->startFrame;
            if (len < 1) len = 1;
            c->lengthFrames = len;
        }
    } else if (fDrag == Drag::Note || fDrag == Drag::NoteResize) {
        if (fDragNote >= 0 && (size_t)fDragNote < t->notes.size()) {
            MidiNote& n = t->notes[(size_t)fDragNote];
            if (fDrag == Drag::Note) {
                Frame start = Snapped(XToFrame(where.x) - fDragGrabOffset);
                if (start < 0) start = 0;
                n.startFrame = start;
                int pitch = PitchAt(lane, where.y) + fDragPitchOffset;
                if (pitch < 0) pitch = 0;
                if (pitch > 127) pitch = 127;
                n.pitch = pitch;
            } else {
                Frame len = Snapped(XToFrame(where.x)) - n.startFrame;
                if (len < 1) len = 1;
                n.lengthFrames = len;
            }
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
        // Only push a command when the gesture actually changed something, so
        // a bare click (down + up, no drag) doesn't pollute the undo stack.
        std::unique_ptr<Command> cmd;
        if (fDrag == Drag::Gain) {
            const float v = t->gain;
            t->gain = fDragOrig;
            if (v != fDragOrig)
                cmd = std::make_unique<SetTrackGainCommand>(fDragTrack, v);
        } else if (fDrag == Drag::Pan) {
            const float v = t->pan;
            t->pan = fDragOrig;
            if (v != fDragOrig)
                cmd = std::make_unique<SetTrackPanCommand>(fDragTrack, v);
        } else if (fDrag == Drag::Clip) {
            if (Clip* c = t->FindClip(fDragClip)) {
                const Frame v = c->startFrame;
                c->startFrame = fDragClipOrig;
                if (v != fDragClipOrig)
                    cmd = std::make_unique<MoveClipCommand>(fDragTrack, fDragClip, v);
            }
        } else if (fDrag == Drag::ClipResize) {
            if (Clip* c = t->FindClip(fDragClip)) {
                const Frame v = c->lengthFrames;
                c->lengthFrames = fDragClipOrigLen;
                if (v != fDragClipOrigLen)
                    cmd = std::make_unique<ResizeClipCommand>(fDragTrack, fDragClip, v);
            }
        } else if (fDrag == Drag::Note || fDrag == Drag::NoteResize) {
            if (fDragNote >= 0 && (size_t)fDragNote < t->notes.size()) {
                MidiNote& n = t->notes[(size_t)fDragNote];
                const int   vp = n.pitch;
                const Frame vs = n.startFrame;
                const Frame vl = n.lengthFrames;
                n = fDragNoteOrig;   // restore for a clean single undo step
                if (vp != fDragNoteOrig.pitch || vs != fDragNoteOrig.startFrame
                    || vl != fDragNoteOrig.lengthFrames)
                    cmd = std::make_unique<NoteEditCommand>(fDragTrack,
                            (size_t)fDragNote, vp, vs, vl);
            }
        }
        if (cmd)
            fStack->Execute(std::move(cmd), *fProject);
    }
    fDrag = Drag::None;
    fDragNote = -1;
    Invalidate(LaneRect(fDragLane));
}

void TimelineView::SetRecording(bool active, Frame start, Frame length) {
    fRecording = active;
    fRecStart  = start;
    fRecLen    = length;
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

    // Bar/beat ticks. Beats are drawn only when there's room; bars always,
    // with a "bar" number label.
    const Grid   grid = GridOf();
    const double fpb   = grid.FramesPerBeat();
    const double fbar  = grid.FramesPerBar();
    if (fpb < 1.0) return;
    const bool drawBeats = (fpb / fFramesPerPixel) >= 8.0;

    const Frame rightFrame = XToFrame(Bounds().right);
    long firstBeat = (long)(XToFrame(kHeaderWidth) / fpb);
    if (firstBeat < 0) firstBeat = 0;

    for (long beat = firstBeat; ; beat++) {
        const Frame f = (Frame)(beat * fpb);
        if (f > rightFrame) break;
        const float x = FrameToX(f);
        if (x < kHeaderWidth) continue;
        const bool isBar = ((Frame)(beat * fpb) % (Frame)fbar) < fpb;

        if (!isBar && !drawBeats) continue;
        SetHighColor(isBar ? ColText() : ColGrid());
        StrokeLine(BPoint(x, isBar ? 0 : kRulerHeight - 8),
                   BPoint(x, kRulerHeight));
        if (isBar) {
            char label[16];
            std::snprintf(label, sizeof(label), "%ld",
                          (long)(f / (Frame)fbar) + 1);
            DrawString(label, BPoint(x + 3, kRulerHeight - 9));
        }
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

        // Bar/beat grid lines through the lane content area (bars brighter).
        const Grid   grid = GridOf();
        const double fpb  = grid.FramesPerBeat();
        const double fbar = grid.FramesPerBar();
        if (fpb >= 1.0) {
            const bool drawBeats = (fpb / fFramesPerPixel) >= 8.0;
            const Frame rightFrame = XToFrame(Bounds().right);
            long firstBeat = (long)(XToFrame(kHeaderWidth) / fpb);
            if (firstBeat < 0) firstBeat = 0;
            for (long beat = firstBeat; ; beat++) {
                const Frame f = (Frame)(beat * fpb);
                if (f > rightFrame) break;
                const float x = FrameToX(f);
                if (x < kHeaderWidth) continue;
                const bool isBar = ((Frame)(beat * fpb) % (Frame)fbar) < fpb;
                if (!isBar && !drawBeats) continue;
                SetHighColor(isBar ? ColGrid() : ColLaneAlt());
                StrokeLine(BPoint(x, lane.top), BPoint(x, lane.bottom));
            }
        }

        for (const Clip& c : t.clips)
            DrawClip(c, lane);

        if (t.type == TrackType::Midi)
            DrawMidiNotes(t, lane);

        // Live recording region on each armed track (grows each poll).
        if (fRecording && t.armed && fRecLen > 0) {
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

    // FX toggle box: lit green when the track has an effect chain.
    BRect fxr = FxRect(lane);
    SetHighColor(t.fx.empty() ? ColLane() : Rgb(80, 170, 110));
    FillRect(fxr);
    SetHighColor(ColGrid());
    StrokeRect(fxr);
    SetHighColor(ColText());
    DrawString("FX", BPoint(fxr.left + 9, fxr.bottom - 4));

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

// Draw a Midi track's notes as a simple piano roll: time across, pitch up.
void TimelineView::DrawMidiNotes(const Track& t, BRect lane) {
    const float h = lane.Height();
    SetHighColor(Rgb(120, 200, 140));
    for (const MidiNote& n : t.notes) {
        float x0 = FrameToX(n.startFrame);
        float x1 = FrameToX(n.startFrame + n.lengthFrames);
        if (x1 < kHeaderWidth || x0 > lane.right)
            continue;
        if (x0 < kHeaderWidth) x0 = kHeaderWidth;
        int p = n.pitch - kMidiLow;
        if (p < 0) p = 0;
        if (p >= kMidiRange) p = kMidiRange - 1;
        const float ny = lane.bottom - (float)p / kMidiRange * h;
        const float nh = h / kMidiRange + 1.0f;
        FillRect(BRect(x0, ny - nh, x1, ny));
    }
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
