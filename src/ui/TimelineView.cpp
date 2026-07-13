#include "TimelineView.h"

#include "UiMetrics.h"
#include "EffectsWindow.h"
#include "SendsWindow.h"
#include "RenameWindow.h"

#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Window.h>

#include <cmath>
#include <cstdio>
#include <memory>

namespace daw {

TimelineView::TimelineView(BRect frame, Project* project, CommandStack* stack)
    : BView(frame, "timeline", B_FOLLOW_ALL_SIDES,
            B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE | B_NAVIGABLE),
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

static BRect RouteRect(BRect lane) { return BRect(84,  lane.top + 2,  112, lane.top + 17); }
static BRect FxRect(BRect lane)    { return BRect(116, lane.top + 2,  152, lane.top + 17); }
static BRect MuteRect(BRect lane)  { return BRect(6,  lane.top + 20, 26,  lane.top + 38); }
static BRect SoloRect(BRect lane)  { return BRect(30, lane.top + 20, 50,  lane.top + 38); }
static BRect ArmRect(BRect lane)   { return BRect(54, lane.top + 20, 74,  lane.top + 38); }
static BRect GainRect(BRect lane)  { return BRect(80, lane.top + 22, 154, lane.top + 34); }
static BRect PanRect(BRect lane)   { return BRect(80, lane.top + 40, 154, lane.top + 52); }
static BRect SndRect(BRect lane)   { return BRect(6,  lane.top + 54, 60,  lane.top + 69); }
static BRect AutoRect(BRect lane)  { return BRect(64, lane.top + 54, 118, lane.top + 69); }

float TimelineView::FrameToX(Frame f) const {
    return kHeaderWidth
         + static_cast<float>((f - fScrollFrame) / fFramesPerPixel);
}

Frame TimelineView::XToFrame(float x) const {
    return fScrollFrame
         + static_cast<Frame>((x - kHeaderWidth) * fFramesPerPixel);
}

void TimelineView::AttachedToWindow() {
    MakeFocus(true);   // receive arrow/zoom keys
}

void TimelineView::ZoomBy(double factor) {
    double fpp = fFramesPerPixel * factor;
    if (fpp < 16.0)    fpp = 16.0;      // most zoomed-in
    if (fpp > 65536.0) fpp = 65536.0;   // most zoomed-out
    fFramesPerPixel = fpp;
    Invalidate();
}

void TimelineView::PanBy(Frame deltaFrames) {
    fScrollFrame += deltaFrames;
    if (fScrollFrame < 0) fScrollFrame = 0;
    // Don't scroll past the content (last clip/note end).
    Frame end = 0;
    for (const Track& t : fProject->Tracks()) {
        for (const Clip& c : t.clips)
            if (c.startFrame + c.lengthFrames > end) end = c.startFrame + c.lengthFrames;
        for (const MidiNote& n : t.notes)
            if (n.startFrame + n.lengthFrames > end) end = n.startFrame + n.lengthFrames;
    }
    if (fScrollFrame > end) fScrollFrame = end;
    Invalidate();
}

void TimelineView::KeyDown(const char* bytes, int32 numBytes) {
    if (numBytes < 1) { BView::KeyDown(bytes, numBytes); return; }
    // One page = the visible content width in frames.
    const Frame page = (Frame)((Bounds().right - kHeaderWidth) * fFramesPerPixel);
    switch (bytes[0]) {
        case B_LEFT_ARROW:  PanBy(-page / 4); break;
        case B_RIGHT_ARROW: PanBy(page / 4);  break;
        case B_HOME:        fScrollFrame = 0; Invalidate(); break;
        case '+': case '=': ZoomBy(0.5); break;   // zoom in
        case '-': case '_': ZoomBy(2.0); break;   // zoom out
        default: BView::KeyDown(bytes, numBytes);
    }
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

// A tiny Copy/Delete popup for a right-clicked clip or note.
int TimelineView::ContextMenu(BPoint where, bool withSplit) const {
    BPopUpMenu* m = new BPopUpMenu("ctx", false, false);
    m->AddItem(new BMenuItem("Copy", NULL));      // 0
    m->AddItem(new BMenuItem("Delete", NULL));    // 1
    if (withSplit)
        m->AddItem(new BMenuItem("Split here", NULL));  // 2
    BMenuItem* sel = m->Go(const_cast<TimelineView*>(this)->ConvertToScreen(where),
                           false, true);
    const int idx = sel ? m->IndexOf(sel) : -1;
    delete m;
    return idx;
}

bool TimelineView::PastePopup(BPoint where) const {
    BPopUpMenu* m = new BPopUpMenu("paste", false, false);
    m->AddItem(new BMenuItem("Paste here", NULL));
    BMenuItem* sel = m->Go(const_cast<TimelineView*>(this)->ConvertToScreen(where),
                           false, true);
    const bool ok = (sel != NULL);
    delete m;
    return ok;
}

void TimelineView::PasteToTrack(TrackId track, Frame at, TrackType type) {
    if (type == TrackType::Audio && fHasClipClip) {
        Clip c = fClipClip;
        c.id = kInvalidClipId;
        c.startFrame = at;
        fStack->Execute(std::make_unique<AddClipCommand>(track, c), *fProject);
        Invalidate();
    } else if (type == TrackType::Midi && fHasClipNote) {
        MidiNote n = fClipNote;
        n.startFrame = at;
        fStack->Execute(std::make_unique<AddNoteCommand>(track, n), *fProject);
        Invalidate();
    }
}

void TimelineView::PasteAtPlayhead() {
    const Frame at = fProject->transport.playhead;
    if (fHasClipClip) {
        // Prefer a track of the source type; fall back to the first such track.
        TrackId target = kInvalidTrackId;
        for (const Track& t : fProject->Tracks())
            if (t.type == TrackType::Audio) { target = t.id; break; }
        if (target != kInvalidTrackId) {
            Clip c = fClipClip;
            c.id = kInvalidClipId;      // AddClipCommand assigns a fresh id
            c.startFrame = at;
            fStack->Execute(std::make_unique<AddClipCommand>(target, c), *fProject);
            Invalidate();
        }
    } else if (fHasClipNote) {
        TrackId target = kInvalidTrackId;
        for (const Track& t : fProject->Tracks())
            if (t.type == TrackType::Midi) { target = t.id; break; }
        if (target != kInvalidTrackId) {
            MidiNote n = fClipNote;
            n.startFrame = at;
            fStack->Execute(std::make_unique<AddNoteCommand>(target, n), *fProject);
            Invalidate();
        }
    }
}

void TimelineView::Draw(BRect updateRect) {
    DrawLanes(updateRect);
    if (fDrag == Drag::Clip && fDragCurLane >= 0)
        DrawDragGhost();     // clip-move preview
    DrawPlayhead();          // over lanes, under the ruler
    DrawRuler(updateRect);   // ruler last so it sits above lane content
}

void TimelineView::DrawDragGhost() {
    if (fDragCurLane >= (int)fProject->Tracks().size())
        return;
    BRect lane = LaneRect(fDragCurLane);
    float x0 = FrameToX(fDragCurStart);
    float x1 = FrameToX(fDragCurStart + fDragClipOrigLen);
    if (x0 < kHeaderWidth) x0 = kHeaderWidth;
    if (x1 <= x0) return;
    BRect g(x0, lane.top + 3, x1, lane.bottom - 3);
    SetHighColor(Rgb(210, 225, 255));
    StrokeRect(g);
    StrokeLine(BPoint(g.left, g.top + 1), BPoint(g.right, g.top + 1));
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

    // Ruler: click seeks, drag sets a loop region. Begin a ruler drag; on
    // release we decide seek-vs-loop by how far it moved.
    if (where.y < kRulerHeight && where.x >= kHeaderWidth) {
        Frame f = Snapped(XToFrame(where.x));
        if (f < 0) f = 0;
        fDrag        = Drag::RulerLoop;
        fLoopAnchor  = f;
        fLoopDragged = false;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        return;
    }

    const int idx = TrackIndexAt(where);
    if (idx < 0)
        return;
    BRect lane = LaneRect(idx);
    const Track& t = fProject->Tracks()[idx];

    if (where.x < kHeaderWidth) {
        // Track name strip: right-click deletes the track, double-click renames.
        if (where.y <= lane.top + 18 && where.x < 114) {
            if (rightClick) {
                BPopUpMenu* mm = new BPopUpMenu("trk", false, false);
                mm->AddItem(new BMenuItem("Move Up", NULL));     // 0
                mm->AddItem(new BMenuItem("Move Down", NULL));   // 1
                mm->AddItem(new BMenuItem("Delete", NULL));      // 2
                BMenuItem* sel = mm->Go(ConvertToScreen(where), false, true);
                const int pick = sel ? mm->IndexOf(sel) : -1;
                delete mm;
                if (pick == 0)
                    fStack->Execute(std::make_unique<MoveTrackCommand>(t.id, -1),
                                    *fProject);
                else if (pick == 1)
                    fStack->Execute(std::make_unique<MoveTrackCommand>(t.id, +1),
                                    *fProject);
                else if (pick == 2)
                    fStack->Execute(std::make_unique<RemoveTrackCommand>(t.id),
                                    *fProject);
                Invalidate();
                return;
            }
            int32 clicks = 1;
            if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
                m->FindInt32("clicks", &clicks);
            if (clicks >= 2) {
                BPoint sp = ConvertToScreen(where);
                BRect wr(sp.x, sp.y, sp.x + 260, sp.y + 74);
                (new RenameWindow(wr, t.id, t.name.c_str(),
                                  BMessenger(Window())))->Show();
                return;
            }
        }
        HandleHeaderClick(t, lane, where);
        return;
    }

    // Automation mode: the content area edits the active gain/pan curve instead
    // of clips/notes. Cycle the header "Auto" box back to Off to edit clips.
    {
        auto am = fAutoMode.find(t.id);
        if (am != fAutoMode.end() && am->second != 0) {
            HandleAutoMouseDown(t, lane, idx, where, rightClick);
            return;
        }
    }

    // MIDI track content: right-click deletes; on a note, drag to move or
    // (near its right edge) resize; on empty space, add a note.
    if (t.type == TrackType::Midi) {
        const int hit = NoteIndexAt(t, lane, where);
        if (rightClick) {
            if (hit >= 0) {
                const int pick = ContextMenu(where);
                if (pick == 0) {          // Copy
                    fClipNote = t.notes[(size_t)hit];
                    fHasClipNote = true; fHasClipClip = false;
                    fClipType = TrackType::Midi;
                } else if (pick == 1) {   // Delete
                    fStack->Execute(std::make_unique<RemoveNoteCommand>(
                        t.id, (size_t)hit), *fProject);
                }
            } else if (fHasClipNote) {     // empty lane: offer paste
                if (PastePopup(where))
                    PasteToTrack(t.id, Snapped(XToFrame(where.x)), TrackType::Midi);
            }
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
            if (modifiers() & B_CONTROL_KEY)
                fDrag = Drag::NoteVelocity;   // Ctrl-drag = set velocity
            else if (wide && where.x >= xEnd - kEdgeGrab)
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
                const int pick = ContextMenu(where, /*withSplit=*/true);
                if (pick == 0) {          // Copy
                    fClipClip = c;
                    fHasClipClip = true; fHasClipNote = false;
                    fClipType = TrackType::Audio;
                } else if (pick == 1) {   // Delete
                    fStack->Execute(std::make_unique<RemoveClipCommand>(t.id, c.id),
                                    *fProject);
                } else if (pick == 2) {   // Split here
                    Frame at = Snapped(XToFrame(where.x));
                    fStack->Execute(std::make_unique<SplitClipCommand>(
                        t.id, c.id, at), *fProject);
                }
                Invalidate(lane);
                return;
            }
            fDragTrack       = t.id;
            fDragLane        = idx;
            fDragClip        = c.id;
            fDragClipOrig    = c.startFrame;
            fDragClipOrigLen = c.lengthFrames;
            fDragFadeInOrig  = c.fadeInFrames;
            fDragFadeOutOrig = c.fadeOutFrames;
            const float xStart = FrameToX(c.startFrame);
            const float xEnd   = FrameToX(c.startFrame + c.lengthFrames);
            const bool  wide   = (xEnd - xStart) > 2 * kEdgeGrab;
            const bool  topBand = where.y <= lane.top + 14;
            if (modifiers() & B_CONTROL_KEY) {
                fDrag = Drag::ClipGain;        // Ctrl-drag vertical = clip gain
                fDragOrig = c.gain;
            } else if (topBand && where.x <= xStart + 12) {
                fDrag = Drag::ClipFadeIn;      // top-left corner = fade in
            } else if (topBand && where.x >= xEnd - 12) {
                fDrag = Drag::ClipFadeOut;     // top-right corner = fade out
            } else if (wide && where.x >= xEnd - kEdgeGrab) {
                fDrag = Drag::ClipResize;
            } else {
                fDrag = Drag::Clip;
                fDragGrabOffset = at - c.startFrame;
                fDragCurLane  = idx;
                fDragCurStart = c.startFrame;
            }
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            break;
        }
    }

    // Right-click on an empty part of an audio lane: offer paste here.
    if (fDrag == Drag::None && rightClick && fHasClipClip) {
        if (PastePopup(where))
            PasteToTrack(t.id, Snapped(XToFrame(where.x)), TrackType::Audio);
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
    if (RouteRect(lane).Contains(where)) {
        // Output routing: Master + every bus track (except this one).
        BPopUpMenu* menu = new BPopUpMenu("route", false, false);
        menu->AddItem(new BMenuItem("Master", NULL));
        std::vector<TrackId> targets;   // parallel to items after index 0
        for (const Track& bt : fProject->Tracks()) {
            if (bt.type != TrackType::Bus || bt.id == id) continue;
            menu->AddItem(new BMenuItem(bt.name.c_str(), NULL));
            targets.push_back(bt.id);
        }
        BMenuItem* sel = menu->Go(ConvertToScreen(where), false, true);
        const int32 pick = sel ? menu->IndexOf(sel) : -1;
        delete menu;
        if (pick == 0) {
            fStack->Execute(std::make_unique<SetTrackOutputCommand>(
                id, kInvalidTrackId), *fProject);
            Invalidate(lane);
        } else if (pick > 0 && (size_t)(pick - 1) < targets.size()) {
            fStack->Execute(std::make_unique<SetTrackOutputCommand>(
                id, targets[(size_t)(pick - 1)]), *fProject);
            Invalidate(lane);
        }
        return;
    }
    if (FxRect(lane).Contains(where)) {
        // Open the per-track effects editor. It runs on its own thread with a
        // snapshot of the chain and posts edits back to the window (main
        // thread) via kMsgApplyFx. Effects apply on the next Play.
        BPoint p = ConvertToScreen(where);
        BRect  wr(p.x, p.y, p.x + 300, p.y + 560);   // tall enough for EQ bands
        EffectsWindow* w = new EffectsWindow(wr, t.fx, id, BMessenger(Window()));
        w->Show();
        return;
    }
    if (SndRect(lane).Contains(where)) {
        // Open the aux-sends editor: snapshot of this track's sends + the list
        // of bus targets. Edits post back via kMsgApplySends.
        std::vector<std::pair<TrackId, std::string>> buses;
        for (const Track& bt : fProject->Tracks())
            if (bt.type == TrackType::Bus && bt.id != id)
                buses.push_back({bt.id, bt.name});
        BPoint p = ConvertToScreen(where);
        BRect  wr(p.x, p.y, p.x + 340, p.y + 320);
        SendsWindow* w = new SendsWindow(wr, t.sends, buses, id,
                                         BMessenger(Window()));
        w->Show();
        return;
    }
    if (AutoRect(lane).Contains(where)) {
        // Cycle the automation edit mode: Off -> Gain -> Pan -> Off.
        int& m = fAutoMode[id];
        m = (m + 1) % 3;
        Invalidate(lane);
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
    // Ruler loop drag isn't tied to a track.
    if (fDrag == Drag::RulerLoop) {
        Frame f = Snapped(XToFrame(where.x));
        if (f < 0) f = 0;
        fLoopDragged = true;
        Transport& tr = fProject->transport;
        tr.loopStart = fLoopAnchor < f ? fLoopAnchor : f;
        tr.loopEnd   = fLoopAnchor < f ? f : fLoopAnchor;
        tr.loopEnabled = (tr.loopEnd > tr.loopStart);
        Invalidate(BRect(0, 0, Bounds().right, kRulerHeight));
        return;
    }

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
        // Ghost preview: track the position + target lane; don't touch the
        // model until drop.
        Frame start = Snapped(XToFrame(where.x) - fDragGrabOffset);
        if (start < 0) start = 0;
        fDragCurStart = start;
        const int dstIdx = TrackIndexAt(where);
        if (dstIdx >= 0 && fProject->Tracks()[dstIdx].type == TrackType::Audio)
            fDragCurLane = dstIdx;
        Invalidate();
        return;   // ghost is drawn in Draw(); no per-lane model change
    } else if (fDrag == Drag::ClipResize) {
        Clip* c = t->FindClip(fDragClip);
        if (c) {
            Frame len = Snapped(XToFrame(where.x)) - c->startFrame;
            if (len < 1) len = 1;
            c->lengthFrames = len;
        }
    } else if (fDrag == Drag::ClipFadeIn) {
        Clip* c = t->FindClip(fDragClip);
        if (c) {
            Frame f = XToFrame(where.x) - c->startFrame;
            if (f < 0) f = 0;
            if (f > c->lengthFrames) f = c->lengthFrames;
            c->fadeInFrames = f;
        }
    } else if (fDrag == Drag::ClipFadeOut) {
        Clip* c = t->FindClip(fDragClip);
        if (c) {
            Frame f = (c->startFrame + c->lengthFrames) - XToFrame(where.x);
            if (f < 0) f = 0;
            if (f > c->lengthFrames) f = c->lengthFrames;
            c->fadeOutFrames = f;
        }
    } else if (fDrag == Drag::ClipGain) {
        Clip* c = t->FindClip(fDragClip);
        if (c) {
            // Vertical position over the lane maps to 0..kMaxGain (top = max).
            float f = (lane.bottom - where.y) / lane.Height();
            float v = f * kMaxGain;
            if (v < 0) v = 0; if (v > kMaxGain) v = kMaxGain;
            c->gain = v;
        }
    } else if (fDrag == Drag::Note || fDrag == Drag::NoteResize
               || fDrag == Drag::NoteVelocity) {
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
            } else if (fDrag == Drag::NoteResize) {
                Frame len = Snapped(XToFrame(where.x)) - n.startFrame;
                if (len < 1) len = 1;
                n.lengthFrames = len;
            } else {   // NoteVelocity: vertical position sets velocity
                float f = (lane.bottom - where.y) / lane.Height();
                int vel = (int)(f * 127.0f + 0.5f);
                if (vel < 1) vel = 1;
                if (vel > 127) vel = 127;
                n.velocity = vel;
            }
        }
    }
    Invalidate(lane);
}

void TimelineView::MouseMoved(BPoint where, uint32, const BMessage*) {
    if (fAutoDragging) {
        Track* t = fProject->FindTrack(fAutoTrack);
        if (!t) return;
        const int mode = (fAutoKind == AutoLaneKind::Pan) ? 2 : 1;
        AutomationLane& live = (mode == 2) ? t->panAuto : t->gainAuto;
        int i = -1;
        for (size_t k = 0; k < live.Count(); k++)
            if (live.At(k).frame == fAutoDragFrame) { i = (int)k; break; }
        if (i < 0) return;
        const BRect lane = LaneRect(fDragLane);
        Frame nf = Snapped(XToFrame(where.x));
        if (nf < 0) nf = 0;
        const float nv = AutoYToValue(lane, mode, where.y);
        live.RemovePoint((size_t)i);
        live.AddPoint(nf, nv);        // re-sorts; frame-key stays unique
        fAutoDragFrame = nf;
        Invalidate(lane);
        return;
    }
    if (fDrag != Drag::None)
        PreviewDrag(where);
}

void TimelineView::MouseUp(BPoint where) {
    if (fAutoDragging) {
        fAutoDragging = false;
        Track* t = fProject->FindTrack(fAutoTrack);
        if (t) {
            AutomationLane& live = (fAutoKind == AutoLaneKind::Pan) ? t->panAuto
                                                                    : t->gainAuto;
            AutomationLane edited = live;  // edited result
            live = fAutoOrig;              // restore pre-gesture state
            fStack->Execute(std::make_unique<SetAutoLaneCommand>(
                fAutoTrack, fAutoKind, edited), *fProject);
        }
        Invalidate();
        return;
    }
    if (fDrag == Drag::None)
        return;

    // Ruler: a real drag leaves a loop region; a bare click (no movement)
    // seeks and clears any loop — regardless of prior loop state.
    if (fDrag == Drag::RulerLoop) {
        Transport& tr = fProject->transport;
        if (!fLoopDragged) {
            tr.loopEnabled = false;
            tr.playhead = fLoopAnchor;
            SetPlayhead(fLoopAnchor);
            if (BWindow* w = Window())
                w->PostMessage(kMsgSeek);
        }
        fDrag = Drag::None;
        Invalidate();
        return;
    }

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
            // Model was never mutated during the drag (ghost preview). Apply
            // the drop: same lane -> reposition; different lane -> move track.
            const Frame v = fDragCurStart;
            TrackId dstId = fDragTrack;
            if (fDragCurLane >= 0
                && fDragCurLane < (int)fProject->Tracks().size()
                && fProject->Tracks()[fDragCurLane].type == TrackType::Audio)
                dstId = fProject->Tracks()[fDragCurLane].id;
            if (dstId != fDragTrack)
                cmd = std::make_unique<MoveClipToTrackCommand>(
                    fDragTrack, fDragClip, dstId, v);
            else if (v != fDragClipOrig)
                cmd = std::make_unique<MoveClipCommand>(fDragTrack, fDragClip, v);
        } else if (fDrag == Drag::ClipResize) {
            if (Clip* c = t->FindClip(fDragClip)) {
                const Frame v = c->lengthFrames;
                c->lengthFrames = fDragClipOrigLen;
                if (v != fDragClipOrigLen)
                    cmd = std::make_unique<ResizeClipCommand>(fDragTrack, fDragClip, v);
            }
        } else if (fDrag == Drag::ClipGain) {
            if (Clip* c = t->FindClip(fDragClip)) {
                const float v = c->gain;
                c->gain = fDragOrig;
                if (v != fDragOrig)
                    cmd = std::make_unique<SetClipGainCommand>(fDragTrack,
                            fDragClip, v);
            }
        } else if (fDrag == Drag::ClipFadeIn || fDrag == Drag::ClipFadeOut) {
            if (Clip* c = t->FindClip(fDragClip)) {
                const Frame fin = c->fadeInFrames, fout = c->fadeOutFrames;
                c->fadeInFrames = fDragFadeInOrig;
                c->fadeOutFrames = fDragFadeOutOrig;
                if (fin != fDragFadeInOrig || fout != fDragFadeOutOrig)
                    cmd = std::make_unique<SetClipFadeCommand>(fDragTrack,
                            fDragClip, fin, fout);
            }
        } else if (fDrag == Drag::Note || fDrag == Drag::NoteResize
                   || fDrag == Drag::NoteVelocity) {
            if (fDragNote >= 0 && (size_t)fDragNote < t->notes.size()) {
                MidiNote& n = t->notes[(size_t)fDragNote];
                const MidiNote final = n;
                n = fDragNoteOrig;   // restore for a clean single undo step
                if (final.pitch != fDragNoteOrig.pitch
                    || final.startFrame != fDragNoteOrig.startFrame
                    || final.lengthFrames != fDragNoteOrig.lengthFrames
                    || final.velocity != fDragNoteOrig.velocity)
                    cmd = std::make_unique<NoteEditCommand>(fDragTrack,
                            (size_t)fDragNote, final);
            }
        }
        if (cmd)
            fStack->Execute(std::move(cmd), *fProject);
    }
    fDrag = Drag::None;
    fDragNote = -1;
    fDragCurLane = -1;
    Invalidate();   // a clip may have moved to another lane
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

    // Loop region highlight.
    const Transport& tr = fProject->transport;
    if (tr.loopEnabled && tr.loopEnd > tr.loopStart) {
        float lx0 = FrameToX(tr.loopStart);
        float lx1 = FrameToX(tr.loopEnd);
        if (lx0 < kHeaderWidth) lx0 = kHeaderWidth;
        if (lx1 > lx0) {
            SetHighColor(Rgb(70, 110, 90));
            FillRect(BRect(lx0, 0, lx1, kRulerHeight));
        }
    }

    // Bar/beat ticks: bars full-height + numbered, beats short (when zoomed in).
    ForEachGridLine([&](float x, bool isBar, long bar) {
        SetHighColor(isBar ? ColText() : ColGrid());
        StrokeLine(BPoint(x, isBar ? 0 : kRulerHeight - 8),
                   BPoint(x, kRulerHeight));
        if (isBar) {
            char label[16];
            std::snprintf(label, sizeof(label), "%ld", bar);
            DrawString(label, BPoint(x + 3, kRulerHeight - 9));
        }
    });
}

// Walk the visible bar/beat gridlines once, invoking fn for each. Shared by
// the ruler and the lane background so their grids can't drift apart.
void TimelineView::ForEachGridLine(
        const std::function<void(float, bool, long)>& fn) const {
    const Grid   grid = GridOf();
    const double fpb  = grid.FramesPerBeat();
    const double fbar = grid.FramesPerBar();
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
        fn(x, isBar, (long)(f / (Frame)fbar) + 1);
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
        ForEachGridLine([&](float x, bool isBar, long) {
            SetHighColor(isBar ? ColGrid() : ColLaneAlt());
            StrokeLine(BPoint(x, lane.top), BPoint(x, lane.bottom));
        });

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

        // Automation curve overlay when this track's Auto mode is on.
        if (auto am = fAutoMode.find(t.id);
            am != fAutoMode.end() && am->second != 0)
            DrawAutomation(t, lane, am->second);

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

    // Output routing box: "->M" master, "->B" a bus.
    BRect rr = RouteRect(lane);
    SetHighColor(t.output == kInvalidTrackId ? ColLane() : Rgb(70, 90, 130));
    FillRect(rr);
    SetHighColor(ColGrid());
    StrokeRect(rr);
    SetHighColor(ColText());
    DrawString(t.output == kInvalidTrackId ? "\xE2\x86\x92" "M" : "\xE2\x86\x92" "B",
               BPoint(rr.left + 4, rr.bottom - 4));

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

    // Sends box: lit amber when the track has aux sends, shows the count.
    BRect sr = SndRect(lane);
    SetHighColor(t.sends.empty() ? ColLane() : Rgb(180, 140, 60));
    FillRect(sr);
    SetHighColor(ColGrid());  StrokeRect(sr);
    SetHighColor(ColText());
    char sl[16];
    std::snprintf(sl, sizeof(sl), "Snd %d", (int)t.sends.size());
    DrawString(sl, BPoint(sr.left + 4, sr.bottom - 4));

    // Automation mode box: Off / Gain / Pan (lit when editing).
    BRect ar = AutoRect(lane);
    int amode = 0;
    if (auto it = fAutoMode.find(t.id); it != fAutoMode.end()) amode = it->second;
    SetHighColor(amode == 0 ? ColLane() : Rgb(90, 130, 90));
    FillRect(ar);
    SetHighColor(ColGrid());  StrokeRect(ar);
    SetHighColor(ColText());
    const char* an = amode == 1 ? "Auto:Gain" : amode == 2 ? "Auto:Pan"
                                                           : "Auto: -";
    DrawString(an, BPoint(ar.left + 4, ar.bottom - 4));
}

// --- Automation editing --------------------------------------------------

float TimelineView::AutoValueToY(BRect lane, int mode, float v) const {
    const float top = lane.top + 4, bot = lane.bottom - 4;
    float t = (mode == 2) ? (v + 1.0f) * 0.5f    // pan -1..1
                          : v / kMaxGain;        // gain 0..max
    if (t < 0) t = 0; if (t > 1) t = 1;
    return bot - t * (bot - top);
}

float TimelineView::AutoYToValue(BRect lane, int mode, float y) const {
    const float top = lane.top + 4, bot = lane.bottom - 4;
    float t = (bot - y) / (bot - top);
    if (t < 0) t = 0; if (t > 1) t = 1;
    return (mode == 2) ? t * 2.0f - 1.0f : t * kMaxGain;
}

int TimelineView::AutoPointAt(const Track& t, BRect lane, int mode,
                              BPoint where) const {
    const AutomationLane& al = (mode == 2) ? t.panAuto : t.gainAuto;
    for (size_t i = 0; i < al.Count(); i++) {
        const float x = FrameToX(al.At(i).frame);
        const float y = AutoValueToY(lane, mode, al.At(i).value);
        if (std::fabs(x - where.x) <= 5.0f && std::fabs(y - where.y) <= 5.0f)
            return (int)i;
    }
    return -1;
}

void TimelineView::DrawAutomation(const Track& t, BRect lane, int mode) {
    const AutomationLane& al = (mode == 2) ? t.panAuto : t.gainAuto;
    const float staticV = (mode == 2) ? t.pan : t.gain;
    const float x0 = kHeaderWidth, x1 = lane.right;

    // Sampled polyline (uniform hold/lerp handling straight from ValueAt).
    SetHighColor(Rgb(230, 200, 90));
    float px = x0, py = AutoValueToY(lane, mode, al.ValueAt(XToFrame(x0), staticV));
    for (float x = x0 + 2.0f; x <= x1; x += 2.0f) {
        const float y = AutoValueToY(lane, mode, al.ValueAt(XToFrame(x), staticV));
        StrokeLine(BPoint(px, py), BPoint(x, y));
        px = x; py = y;
    }
    // Breakpoint handles.
    SetHighColor(Rgb(255, 232, 120));
    for (size_t i = 0; i < al.Count(); i++) {
        const float x = FrameToX(al.At(i).frame);
        if (x < x0 || x > x1) continue;
        const float y = AutoValueToY(lane, mode, al.At(i).value);
        FillRect(BRect(x - 3, y - 3, x + 3, y + 3));
    }
}

void TimelineView::HandleAutoMouseDown(const Track& t, BRect lane, int idx,
                                       BPoint where, bool rightClick) {
    const int mode = fAutoMode[t.id];                 // 1 gain / 2 pan
    const AutoLaneKind kind = (mode == 2) ? AutoLaneKind::Pan
                                          : AutoLaneKind::Gain;
    const AutomationLane& al = (mode == 2) ? t.panAuto : t.gainAuto;
    int hit = AutoPointAt(t, lane, mode, where);

    // Right-click a handle: delete it (one command).
    if (rightClick) {
        if (hit >= 0) {
            AutomationLane nl = al;
            nl.RemovePoint((size_t)hit);
            fStack->Execute(std::make_unique<SetAutoLaneCommand>(t.id, kind, nl),
                            *fProject);
            Invalidate(lane);
        }
        return;
    }

    // Snapshot the lane for undo, then begin a live-edit gesture on the model.
    fAutoOrig = al;
    Track* tm = fProject->FindTrack(t.id);
    if (!tm) return;
    AutomationLane& live = (mode == 2) ? tm->panAuto : tm->gainAuto;

    if (hit < 0) {                                    // empty: add a point
        Frame f = Snapped(XToFrame(where.x));
        if (f < 0) f = 0;
        const float v = AutoYToValue(lane, mode, where.y);
        live.AddPoint(f, v);
        fAutoDragFrame = f;
    } else {
        fAutoDragFrame = al.At((size_t)hit).frame;    // track by frame-key
    }

    fAutoDragging = true;
    fAutoTrack    = t.id;
    fAutoKind     = kind;
    fDragLane     = idx;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    Invalidate(lane);
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

    // Fade ramps: a diagonal from the block corner up to where the fade ends.
    SetHighColor(ColClipBorder());
    if (c.fadeInFrames > 0) {
        float fx = FrameToX(c.startFrame + c.fadeInFrames);
        if (fx > block.left)
            StrokeLine(BPoint(block.left, block.bottom),
                       BPoint(fx < block.right ? fx : block.right, block.top));
    }
    if (c.fadeOutFrames > 0) {
        float fx = FrameToX(c.startFrame + c.lengthFrames - c.fadeOutFrames);
        if (fx < block.right)
            StrokeLine(BPoint(fx > block.left ? fx : block.left, block.top),
                       BPoint(block.right, block.bottom));
    }

    // Per-clip gain: a horizontal line across the block at the gain level
    // (top = kMaxGain, bottom = 0), plus a dB label when not at unity.
    if (block.Width() > 24) {
        float gf = c.gain / kMaxGain; if (gf < 0) gf = 0; if (gf > 1) gf = 1;
        const float gy = block.bottom - gf * block.Height();
        SetHighColor(Rgb(255, 232, 120));
        StrokeLine(BPoint(block.left, gy), BPoint(block.right, gy));
        if (std::fabs(c.gain - 1.0f) > 0.01f) {
            char db[16];
            const float dB = c.gain > 0.0001f ? 20.0f * std::log10(c.gain)
                                              : -99.0f;
            std::snprintf(db, sizeof(db), "%+.1f dB", dB);
            DrawString(db, BPoint(block.left + 4, block.bottom - 4));
        }
    }

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
        // Brightness tracks velocity (Ctrl-drag a note to change it).
        const float s = 0.4f + 0.6f * (n.velocity / 127.0f);
        SetHighColor(Rgb((uint8)(120 * s), (uint8)(200 * s), (uint8)(140 * s)));
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
