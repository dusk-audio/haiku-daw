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

PianoRollView::PianoRollView(BRect frame, TrackId track,
                             std::vector<MidiNote> notes, TempoMap tempo,
                             double sampleRate, BMessenger apply)
    : BView(frame, "roll", B_FOLLOW_ALL_SIDES, B_WILL_DRAW),
      fNotes(std::move(notes)), fTrack(track), fTempo(tempo),
      fSampleRate(sampleRate), fApply(apply) {
    SetViewColor(ColBackground());
    fTempo.sampleRate = sampleRate;
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
    for (const MidiNote& n : fNotes) {
        const float x0 = FrameToX(n.startFrame);
        const float x1 = FrameToX(n.startFrame + n.lengthFrames);
        const float y  = PitchToY(n.pitch);
        if (x1 < kKbdW || x0 > w || y + kRowH < 0 || y > h) continue;
        const float s = 0.45f + 0.55f * (n.velocity / 127.0f);
        BRect nr(std::max(x0, kKbdW), y + 1, x1, y + kRowH - 1);
        SetHighColor(Rgb((uint8)(70 * s), (uint8)(150 * s), (uint8)(220 * s)));
        FillRect(nr);
        SetHighColor(ColClipBorder());
        StrokeRect(nr);
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
    for (const MidiNote& n : fNotes) {
        m.AddInt32("np", n.pitch);
        m.AddInt32("nv", n.velocity);
        m.AddInt64("ns", (int64)n.startFrame);
        m.AddInt64("nl", (int64)n.lengthFrames);
    }
    fApply.SendMessage(&m);
}

void PianoRollView::MouseDown(BPoint where) {
    if (where.x < kKbdW) return;
    int32 buttons = 0;
    if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
        m->FindInt32("buttons", &buttons);
    const bool rightClick = (buttons & B_SECONDARY_MOUSE_BUTTON) != 0;

    const int hit = NoteAt(where);
    if (rightClick) {
        if (hit >= 0) { fNotes.erase(fNotes.begin() + hit); Apply(); Invalidate(); }
        return;
    }
    if (hit >= 0) {
        const MidiNote& n = fNotes[(size_t)hit];
        const float x1 = FrameToX(n.startFrame + n.lengthFrames);
        fDragNote = hit;
        if (modifiers() & B_CONTROL_KEY)      fDrag = Drag::Velocity;
        else if (where.x >= x1 - kEdge)       fDrag = Drag::Resize;
        else {
            fDrag = Drag::Move;
            fGrabOffset = XToFrame(where.x) - n.startFrame;
            fPitchOffset = n.pitch - YToPitch(where.y);
        }
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        return;
    }
    // Empty: add a note (1 beat long, snapped).
    MidiNote n;
    Frame start = Snapped(XToFrame(where.x));
    if (start < 0) start = 0;
    n.startFrame = start;
    n.pitch = std::clamp(YToPitch(where.y), 0, 127);
    n.velocity = 100;
    n.lengthFrames = (Frame)fTempo.FramesPerBeatAt(start);
    fNotes.push_back(n);
    Apply();
    Invalidate();
}

void PianoRollView::MouseMoved(BPoint where, uint32, const BMessage*) {
    if (fDrag == Drag::None || fDragNote < 0
        || (size_t)fDragNote >= fNotes.size()) return;
    MidiNote& n = fNotes[(size_t)fDragNote];
    if (fDrag == Drag::Move) {
        Frame start = Snapped(XToFrame(where.x) - fGrabOffset);
        if (start < 0) start = 0;
        n.startFrame = start;
        n.pitch = std::clamp(YToPitch(where.y) + fPitchOffset, 0, 127);
    } else if (fDrag == Drag::Resize) {
        Frame len = Snapped(XToFrame(where.x)) - n.startFrame;
        if (len < 1) len = 1;
        n.lengthFrames = len;
    } else {   // Velocity: map vertical pointer position over the view height
        const float t = 1.0f - where.y / Bounds().Height();
        n.velocity = std::clamp((int)(t * 127.0f + 0.5f), 1, 127);
    }
    Invalidate();
}

void PianoRollView::MouseUp(BPoint) {
    if (fDrag != Drag::None) { Apply(); Invalidate(); }
    fDrag = Drag::None;
    fDragNote = -1;
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
        default: BView::KeyDown(bytes, numBytes);
    }
}

// --- window ---------------------------------------------------------------

PianoRoll::PianoRoll(BRect frame, TrackId track, std::vector<MidiNote> notes,
                     TempoMap tempo, double sampleRate, BMessenger apply)
    : BWindow(frame, "Piano Roll", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS) {
    fView = new PianoRollView(Bounds(), track, std::move(notes), tempo,
                              sampleRate, apply);
    AddChild(fView);
    fView->MakeFocus(true);
}

} // namespace daw
