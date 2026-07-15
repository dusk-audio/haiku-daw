#include "PianoRoll.h"

#include "UiMetrics.h"

#include <PopUpMenu.h>
#include <MenuItem.h>
#include <Window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace daw {

static constexpr float kKbdW = 54.0f;   // piano keyboard column width
static constexpr float kRowH = 12.0f;   // pixels per semitone
static constexpr int   kSnapDiv = 4;    // 16th-note snap
static constexpr float kEdge = 5.0f;    // resize grab zone

static bool IsBlackKey(int pitch) {
    switch (((pitch % 12) + 12) % 12) {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}
static const char* NoteName(int pitch, char* buf, size_t n) {
    static const char* names[12] = { "C","C#","D","D#","E","F","F#","G","G#",
                                     "A","A#","B" };
    const int oct = pitch / 12 - 1;
    std::snprintf(buf, n, "%s%d", names[((pitch % 12) + 12) % 12], oct);
    return buf;
}

PianoRollView::PianoRollView(BRect frame, TrackId track, ClipId clip,
                             std::vector<MidiNote> notes, TempoMap tempo,
                             double sampleRate, BMessenger apply)
    : BView(frame, "roll", B_FOLLOW_ALL_SIDES, B_WILL_DRAW),
      fNotes(std::move(notes)), fTrack(track), fClip(clip), fTempo(tempo),
      fSampleRate(sampleRate), fApply(apply) {
    SetViewColor(ColBackground());
    fTempo.sampleRate = sampleRate;
    fSel.assign(fNotes.size(), 0);
}

void PianoRollView::SelectOnly(int i) {
    fSel.assign(fNotes.size(), 0);
    if (i >= 0 && (size_t)i < fSel.size()) fSel[(size_t)i] = 1;
}
void PianoRollView::ClearSelection() { fSel.assign(fNotes.size(), 0); }
int PianoRollView::SelectedCount() const {
    int c = 0; for (char s : fSel) c += s ? 1 : 0; return c;
}
void PianoRollView::DeleteSelected() {
    std::vector<MidiNote> keep;
    for (size_t i = 0; i < fNotes.size(); i++)
        if (!fSel[i]) keep.push_back(fNotes[i]);
    if (keep.size() == fNotes.size()) return;   // nothing selected
    fNotes.swap(keep);
    fSel.assign(fNotes.size(), 0);
    Apply(); Invalidate();
}

float PianoRollView::FrameToX(Frame f) const {
    return kKbdW + (float)((double)(f - fScrollFrame) / fFramesPerPixel);
}
Frame PianoRollView::XToFrame(float x) const {
    return fScrollFrame + (Frame)((double)(x - kKbdW) * fFramesPerPixel);
}
float PianoRollView::PitchToY(int pitch) const {
    return (float)(fTopPitch - pitch) * kRowH;
}
int PianoRollView::YToPitch(float y) const {
    return fTopPitch - (int)std::floor(y / kRowH);
}
Frame PianoRollView::Snapped(Frame f) const {
    if (modifiers() & B_SHIFT_KEY) return f < 0 ? 0 : f;
    const double beat = fTempo.BeatAt(f < 0 ? 0 : f);
    const double snapped = std::llround(beat * kSnapDiv) / (double)kSnapDiv;
    const Frame out = fTempo.FrameAt(snapped);
    return out < 0 ? 0 : out;
}

void PianoRollView::Draw(BRect) {
    const float w = Bounds().Width(), h = Bounds().Height();

    // Rows (pitch lanes) with octave shading + keyboard column.
    for (float y = 0; y < h; y += kRowH) {
        const int pitch = YToPitch(y + 1);
        SetHighColor(IsBlackKey(pitch) ? ColLaneAlt() : ColLane());
        FillRect(BRect(kKbdW, y, w, y + kRowH));
        if (((pitch % 12) + 12) % 12 == 0) {   // octave boundary (C)
            SetHighColor(ColGrid());
            StrokeLine(BPoint(kKbdW, y + kRowH), BPoint(w, y + kRowH));
        }
        // Keyboard key.
        SetHighColor(IsBlackKey(pitch) ? Rgb(28, 30, 35) : Rgb(210, 214, 220));
        FillRect(BRect(0, y, kKbdW - 2, y + kRowH));
        if (((pitch % 12) + 12) % 12 == 0) {   // label C notes
            char nb[8]; NoteName(pitch, nb, sizeof(nb));
            SetHighColor(Rgb(40, 44, 50));
            DrawString(nb, BPoint(4, y + kRowH - 2));
        }
    }
    SetHighColor(ColGrid());
    StrokeLine(BPoint(kKbdW, 0), BPoint(kKbdW, h));

    // Vertical bar/beat gridlines.
    const Frame left = XToFrame(kKbdW), right = XToFrame(w);
    if (right > left) {
        long beat = (long)std::floor(fTempo.BeatAt(left < 0 ? 0 : left));
        if (beat < 0) beat = 0;
        for (;; beat++) {
            const Frame f = fTempo.FrameAt((double)beat);
            if (f > right) break;
            const float x = FrameToX(f);
            if (x < kKbdW) continue;
            int bar = 1, bb = 1; fTempo.BarBeat(f, &bar, &bb);
            SetHighColor(bb == 1 ? ColGrid() : ColLaneAlt());
            StrokeLine(BPoint(x, 0), BPoint(x, h));
        }
    }

    // Notes.
    for (size_t i = 0; i < fNotes.size(); i++) {
        const MidiNote& n = fNotes[i];
        const float x0 = FrameToX(n.startFrame);
        const float x1 = FrameToX(n.startFrame + n.lengthFrames);
        const float y  = PitchToY(n.pitch);
        if (x1 < kKbdW || x0 > w || y + kRowH < 0 || y > h) continue;
        const bool sel = i < fSel.size() && fSel[i];
        const float s = 0.45f + 0.55f * (n.velocity / 127.0f);
        BRect nr(std::max(x0, kKbdW), y + 1, x1, y + kRowH - 1);
        if (sel) SetHighColor(Rgb((uint8)(120 * s), (uint8)(190 * s), (uint8)(255 * s)));
        else     SetHighColor(Rgb((uint8)(70 * s),  (uint8)(150 * s), (uint8)(220 * s)));
        FillRect(nr);
        SetHighColor(sel ? Rgb(255, 240, 140) : ColClipBorder());
        StrokeRect(nr);
    }

    // Marquee rectangle (rubber-band selection in progress).
    if (fDrag == Drag::Marquee) {
        BRect m(std::min(fDownPoint.x, fMarqueeCur.x),
                std::min(fDownPoint.y, fMarqueeCur.y),
                std::max(fDownPoint.x, fMarqueeCur.x),
                std::max(fDownPoint.y, fMarqueeCur.y));
        SetHighColor(255, 240, 140);
        SetPenSize(1.0f);
        StrokeRect(m, B_MIXED_COLORS);
    }
}

int PianoRollView::NoteAt(BPoint where) const {
    for (int i = (int)fNotes.size() - 1; i >= 0; i--) {
        const MidiNote& n = fNotes[(size_t)i];
        const float x0 = FrameToX(n.startFrame);
        const float x1 = FrameToX(n.startFrame + n.lengthFrames);
        const float y  = PitchToY(n.pitch);
        if (where.x >= x0 && where.x <= x1 && where.y >= y && where.y <= y + kRowH)
            return i;
    }
    return -1;
}

void PianoRollView::Apply() {
    BMessage m(kMsgApplyNotes);
    m.AddInt64("track", (int64)fTrack);
    m.AddInt64("clip", (int64)fClip);
    for (const MidiNote& n : fNotes) {
        m.AddInt32("np", n.pitch);
        m.AddInt32("nv", n.velocity);
        m.AddInt64("ns", (int64)n.startFrame);
        m.AddInt64("nl", (int64)n.lengthFrames);
    }
    fApply.SendMessage(&m);
}

// Snapshot every note's geometry so a group move/resize applies one delta.
void PianoRollView::CaptureDragOrigin() {
    fDragOrig.resize(fNotes.size());
    for (size_t i = 0; i < fNotes.size(); i++)
        fDragOrig[i] = { fNotes[i].startFrame, fNotes[i].lengthFrames,
                         fNotes[i].pitch, fNotes[i].velocity };
}

void PianoRollView::MouseDown(BPoint where) {
    if (where.x < kKbdW) return;
    int32 buttons = 0;
    if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
        m->FindInt32("buttons", &buttons);
    const bool rightClick = (buttons & B_SECONDARY_MOUSE_BUTTON) != 0;
    const bool additive   = (modifiers() & B_COMMAND_KEY) != 0;

    const int hit = NoteAt(where);
    fDownPoint = where;

    if (rightClick) {
        if (hit >= 0) {
            // Delete the whole selection if the clicked note is part of it,
            // otherwise just the clicked note.
            if ((size_t)hit < fSel.size() && fSel[(size_t)hit]
                && SelectedCount() > 1) {
                DeleteSelected();
            } else {
                fNotes.erase(fNotes.begin() + hit);
                if ((size_t)hit < fSel.size()) fSel.erase(fSel.begin() + hit);
                Apply(); Invalidate();
            }
        }
        return;
    }

    if (hit >= 0) {
        if (additive) {   // toggle this note in/out of the selection, no drag
            fSel[(size_t)hit] = fSel[(size_t)hit] ? 0 : 1;
            Invalidate();
            return;
        }
        // Plain click: if the note isn't already selected, make it the sole
        // selection; a click on an already-selected note keeps the group.
        if ((size_t)hit >= fSel.size() || !fSel[(size_t)hit])
            SelectOnly(hit);

        const MidiNote& n = fNotes[(size_t)hit];
        const float x1 = FrameToX(n.startFrame + n.lengthFrames);
        fDragNote = hit;
        CaptureDragOrigin();
        if (modifiers() & B_CONTROL_KEY)      fDrag = Drag::Velocity;
        else if (where.x >= x1 - kEdge)       fDrag = Drag::Resize;
        else {
            fDrag = Drag::Move;
            fGrabOffset  = XToFrame(where.x) - n.startFrame;
            fPitchOffset = n.pitch - YToPitch(where.y);
        }
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        Invalidate();
        return;
    }

    // Empty space: begin a marquee. If the pointer doesn't move, MouseUp
    // treats it as a click and adds a note. Additive keeps prior selection.
    fDrag = Drag::Marquee;
    fMarqueeCur = where;
    fPreMarquee = additive ? fSel : std::vector<char>(fNotes.size(), 0);
    if (!additive) ClearSelection();
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    Invalidate();
}

void PianoRollView::MouseMoved(BPoint where, uint32, const BMessage*) {
    if (fDrag == Drag::None) return;

    if (fDrag == Drag::Marquee) {
        fMarqueeCur = where;
        BRect m(std::min(fDownPoint.x, where.x), std::min(fDownPoint.y, where.y),
                std::max(fDownPoint.x, where.x), std::max(fDownPoint.y, where.y));
        for (size_t i = 0; i < fNotes.size(); i++) {
            const MidiNote& n = fNotes[i];
            const float x0 = FrameToX(n.startFrame);
            const float x1 = FrameToX(n.startFrame + n.lengthFrames);
            const float y  = PitchToY(n.pitch);
            const bool inside = x1 >= m.left && x0 <= m.right
                                && y + kRowH >= m.top && y <= m.bottom;
            const char prev = i < fPreMarquee.size() ? fPreMarquee[i] : 0;
            fSel[i] = (inside || prev) ? 1 : 0;   // union with prior selection
        }
        Invalidate();
        return;
    }

    if (fDragNote < 0 || (size_t)fDragNote >= fNotes.size()
        || fDragOrig.size() != fNotes.size()) return;

    if (fDrag == Drag::Move) {
        // One snapped delta (frames + pitch) from the grabbed note, applied to
        // every selected note from its captured origin.
        const Orig& g = fDragOrig[(size_t)fDragNote];
        Frame newStart = Snapped(XToFrame(where.x) - fGrabOffset);
        if (newStart < 0) newStart = 0;
        const Frame dFrame = newStart - g.start;
        const int   dPitch = std::clamp(YToPitch(where.y) + fPitchOffset, 0, 127)
                             - g.pitch;
        for (size_t i = 0; i < fNotes.size(); i++) {
            if (!fSel[i]) continue;
            Frame s = fDragOrig[i].start + dFrame;
            if (s < 0) s = 0;
            fNotes[i].startFrame = s;
            fNotes[i].pitch = std::clamp(fDragOrig[i].pitch + dPitch, 0, 127);
        }
    } else if (fDrag == Drag::Resize) {
        const Orig& g = fDragOrig[(size_t)fDragNote];
        Frame newLen = Snapped(XToFrame(where.x)) - g.start;
        if (newLen < 1) newLen = 1;
        const Frame dLen = newLen - g.len;
        for (size_t i = 0; i < fNotes.size(); i++) {
            if (!fSel[i]) continue;
            Frame len = fDragOrig[i].len + dLen;
            if (len < 1) len = 1;
            fNotes[i].lengthFrames = len;
        }
    } else {   // Velocity: same absolute value across the selection
        const float t = 1.0f - where.y / Bounds().Height();
        const int vel = std::clamp((int)(t * 127.0f + 0.5f), 1, 127);
        for (size_t i = 0; i < fNotes.size(); i++)
            if (fSel[i]) fNotes[i].velocity = vel;
    }
    Invalidate();
}

void PianoRollView::MouseUp(BPoint where) {
    const Drag was = fDrag;
    fDrag = Drag::None;
    fDragNote = -1;

    if (was == Drag::Marquee) {
        const float moved = std::fabs(where.x - fDownPoint.x)
                          + std::fabs(where.y - fDownPoint.y);
        if (moved < 4.0f) {
            // A click on empty space (no marquee drag): add a note, selected.
            MidiNote n;
            Frame start = Snapped(XToFrame(fDownPoint.x));
            if (start < 0) start = 0;
            n.startFrame   = start;
            n.pitch        = std::clamp(YToPitch(fDownPoint.y), 0, 127);
            n.velocity     = 100;
            n.lengthFrames = (Frame)fTempo.FramesPerBeatAt(start);
            fNotes.push_back(n);
            fSel.assign(fNotes.size(), 0);
            fSel.back() = 1;
            Apply();
        }
        Invalidate();
        return;
    }
    if (was != Drag::None) { Apply(); Invalidate(); }
}

void PianoRollView::MessageReceived(BMessage* msg) {
    if (msg->what == B_MOUSE_WHEEL_CHANGED) {
        float dy = 0.0f;
        if (msg->FindFloat("be:wheel_delta_y", &dy) == B_OK && dy != 0.0f) {
            fTopPitch -= (int)dy * 3;
            if (fTopPitch > 127) fTopPitch = 127;
            if (fTopPitch < 24)  fTopPitch = 24;
            Invalidate();
            return;
        }
    }
    BView::MessageReceived(msg);
}

void PianoRollView::KeyDown(const char* bytes, int32 numBytes) {
    if (numBytes < 1) { BView::KeyDown(bytes, numBytes); return; }
    const Frame page = (Frame)((Bounds().Width() - kKbdW) * fFramesPerPixel);
    switch (bytes[0]) {
        case B_LEFT_ARROW:  fScrollFrame -= page / 4; if (fScrollFrame < 0) fScrollFrame = 0; Invalidate(); break;
        case B_RIGHT_ARROW: fScrollFrame += page / 4; Invalidate(); break;
        case '+': case '=': fFramesPerPixel *= 0.5; if (fFramesPerPixel < 8) fFramesPerPixel = 8; Invalidate(); break;
        case '-': case '_': fFramesPerPixel *= 2.0; if (fFramesPerPixel > 8192) fFramesPerPixel = 8192; Invalidate(); break;
        case B_DELETE: case B_BACKSPACE: DeleteSelected(); break;
        case 1: case 'a': case 'A':   // Command-A: select all
            if (modifiers() & B_COMMAND_KEY) {
                fSel.assign(fNotes.size(), 1); Invalidate();
            } else BView::KeyDown(bytes, numBytes);
            break;
        default: BView::KeyDown(bytes, numBytes);
    }
}

// --- window ---------------------------------------------------------------

PianoRoll::PianoRoll(BRect frame, TrackId track, ClipId clip,
                     std::vector<MidiNote> notes,
                     TempoMap tempo, double sampleRate, BMessenger apply)
    : BWindow(frame, "Piano Roll", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS) {
    fView = new PianoRollView(Bounds(), track, clip, std::move(notes), tempo,
                              sampleRate, apply);
    AddChild(fView);
    fView->MakeFocus(true);
}

} // namespace daw
