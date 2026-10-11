#include "PianoRoll.h"

#include "QuantizeWindow.h"
#include "UiMetrics.h"
#include "Widgets.h"
#include "widgets/DawIcons.h"   // the tool glyphs (M1.6)

#include <OS.h>   // system_time(): the humanize seed
#include <PopUpMenu.h>
#include <MenuItem.h>
#include <Window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace daw {

static constexpr float kKbdW = 54.0f;   // piano keyboard column width
static constexpr float kRowH = 12.0f;   // pixels per semitone
static constexpr int   kSnapDiv = 4;    // 16th-note snap
static constexpr float kEdge = 5.0f;    // resize grab zone
static constexpr float kVelLaneH = 64.0f;   // bottom velocity (lollipop) lane
static constexpr float kToolbarH = 28.0f;   // top tool palette strip

// The tool palette is icon-only since M1.6, so these names are what the hover
// tooltip shows and what the buttons are called in the help.
static const char* kToolNames[7] = { "Pointer", "Pencil", "Brush", "Eraser",
                                     "Scissors", "Glue", "Velocity" };
static BRect ZoomOutRectR() { return BRect(360, 5, 384, 23); }
static BRect ZoomInRectR()  { return BRect(388, 5, 412, 23); }
static BRect MidiMenuRectR() { return BRect(420, 5, 476, 23); }

// The note-list half of the roll's edit messages. kMsgApplyNotes and
// kMsgApplyMidiOp carry the same shape, so MainWindow decodes them once.
static void AddNoteList(BMessage* m, const std::vector<MidiNote>& notes) {
    for (const MidiNote& n : notes) {
        m->AddInt32("np", n.pitch);
        m->AddInt32("nv", n.velocity);
        m->AddInt64("ns", (int64)n.startFrame);
        m->AddInt64("nl", (int64)n.lengthFrames);
    }
}

// Note fill color by velocity: cool (blue) when soft, hot (red) when loud.
static rgb_color VelHeat(int vel) {
    float t = vel / 127.0f;
    if (t < 0) t = 0; if (t > 1) t = 1;
    float r, g, b;
    if (t < 0.25f)      { float u = t / 0.25f;          r = 40;          g = 120 + 90 * u; b = 235; }
    else if (t < 0.5f)  { float u = (t - 0.25f) / 0.25f; r = 40;          g = 210;          b = 235 - 175 * u; }
    else if (t < 0.75f) { float u = (t - 0.5f) / 0.25f;  r = 40 + 215 * u; g = 210;          b = 60; }
    else                { float u = (t - 0.75f) / 0.25f; r = 255;         g = 210 - 155 * u; b = 60 - 20 * u; }
    return Rgb((uint8)r, (uint8)g, (uint8)b);
}

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

// Bottom-lane choices. Velocity plus the controllers the synth actually renders
// (CC7 x CC11 = channel gain, CC10 = pan), so editing one is always audible.
const PianoRollView::LaneDef PianoRollView::kLanes[4] = {
    { "Vel",  -1 },
    { "Vol",   7 },
    { "Expr", 11 },
    { "Pan",  10 },
};

PianoRollView::PianoRollView(BRect frame, TrackId track, ClipId clip,
                             Frame clipStart, Frame clipLength,
                             std::vector<MidiNote> notes,
                             std::vector<MidiClipEvent> events,
                             TempoMap tempo, double sampleRate, Frame playhead,
                             BMessenger apply)
    // Full update on resize: the velocity lane hangs off the bottom edge, and
    // the docked roll is resized by its split as well as by a window.
    : BView(frame, "roll", B_FOLLOW_ALL_SIDES,
            B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
      fNotes(std::move(notes)), fEvents(std::move(events)),
      fTrack(track), fClip(clip),
      fClipStart(clipStart), fClipLen(clipLength), fTempo(tempo),
      fSampleRate(sampleRate), fApply(apply) {
    ApplyTheme();
    fTempo.sampleRate = sampleRate;
    fSel.assign(fNotes.size(), 0);

    // Open focused on the tapehead. The main window only pushes the playhead
    // during transport, so seed it here to draw it right away, and centre the
    // view on it -- but only while it is inside this region. Outside it there is
    // nothing to edit there, so fall back to the region's head (scroll 0).
    const Frame rel = playhead - fClipStart;
    if (playhead >= 0 && rel >= 0 && (clipLength <= 0 || rel <= clipLength)) {
        fPlayhead = playhead;
        const double visible = ((double)frame.Width() - kKbdW) * fFramesPerPixel;
        if (visible > 0) {
            fScrollFrame = rel - (Frame)(visible * 0.5);
            if (fScrollFrame < 0) fScrollFrame = 0;
        }
    }
}

void PianoRollView::ApplyTheme() {
    SetViewColor(ColBackground());
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
    return kToolbarH + (float)(fTopPitch - pitch) * kRowH;
}
int PianoRollView::YToPitch(float y) const {
    return fTopPitch - (int)std::floor((y - kToolbarH) / kRowH);
}

// Icon-only buttons since M1.6: a 22x18 glyph with the tool's name in its
// tooltip reads faster than seven short words, and it is what the icon set is
// for. (The name is still the label, so nothing else had to change.)
BRect PianoRollView::ToolRect(int i) const {
    return BRect(4 + i * 26, 5, 4 + i * 26 + 22, 23);
}
int PianoRollView::ToolAt(BPoint where) const {
    for (int i = 0; i < 7; i++)
        if (ToolRect(i).Contains(where)) return i;
    return -1;
}
void PianoRollView::ZoomBy(double factor) {
    // Zoom about the view's left edge (keeps the left-visible frame anchored).
    fFramesPerPixel *= factor;
    if (fFramesPerPixel < 8)    fFramesPerPixel = 8;
    if (fFramesPerPixel > 8192) fFramesPerPixel = 8192;
    Invalidate();
}
Frame PianoRollView::Snapped(Frame f) const {   // f is clip-relative
    if (modifiers() & B_SHIFT_KEY) return f < 0 ? 0 : f;
    // Snap in absolute-timeline space so notes align to the song grid even when
    // the region doesn't start on a bar; convert back to clip-relative.
    Frame af = f + fClipStart; if (af < 0) af = 0;
    const double beat = fTempo.BeatAt(af);
    const double snapped = std::llround(beat * kSnapDiv) / (double)kSnapDiv;
    const Frame out = fTempo.FrameAt(snapped) - fClipStart;
    return out < 0 ? 0 : out;
}

float PianoRollView::VelLaneTop() const { return Bounds().Height() - kVelLaneH; }

// The keyboard column, drawn as a piano rather than as one stripe per row: the
// white keys are one surface whose seams fall where a real keyboard's do (on
// the row edge between B|C and E|F, under the middle of the black key
// everywhere else), and the black keys are shorter, rounded, and on top.
void PianoRollView::DrawKeyboard(float velTop) {
    const float right = kKbdW - 2.0f;
    const float blackW = std::floor(kKbdW * 0.6f);
    PushState();
    ClipToRect(BRect(0, kToolbarH, right, velTop));

    SetHighColor(Rgb(222, 225, 230));                         // the white keys
    FillRect(BRect(0, kToolbarH, right, velTop));

    SetHighColor(Rgb(150, 154, 162));                         // their seams
    for (float y = kToolbarH; y < velTop; y += kRowH) {
        const int pitch = YToPitch(y + 1);
        if (IsBlackKey(pitch)) continue;
        // The seam below this white key: on the row edge when the next key
        // down is white too, else through the black key's middle.
        const float seam = IsBlackKey(pitch - 1) ? y + kRowH * 1.5f
                                                 : y + kRowH;
        StrokeLine(BPoint(0, seam), BPoint(right, seam));
    }

    for (float y = kToolbarH; y < velTop; y += kRowH) {       // the black keys
        const int pitch = YToPitch(y + 1);
        if (!IsBlackKey(pitch)) continue;
        const BRect key(0, y + 1, blackW, y + kRowH - 1);
        SetHighColor(Rgb(30, 32, 37));
        FillRoundRect(key, 2.0f, 2.0f);
        SetHighColor(Rgb(70, 74, 82));                         // a lit edge
        StrokeLine(BPoint(key.left, key.top), BPoint(key.right - 2, key.top));
    }

    for (float y = kToolbarH; y < velTop; y += kRowH) {       // C labels
        const int pitch = YToPitch(y + 1);
        if (((pitch % 12) + 12) % 12 != 0) continue;
        char nb[8]; NoteName(pitch, nb, sizeof(nb));
        SetHighColor(Rgb(70, 74, 82));
        DrawString(nb, BPoint(right - 3 - StringWidth(nb), y + kRowH - 2));
    }
    PopState();

    SetHighColor(Rgb(20, 20, 24));                            // keyboard edge
    StrokeLine(BPoint(right + 1, kToolbarH), BPoint(right + 1, velTop));
}

void PianoRollView::Draw(BRect) {
    const float w = Bounds().Width(), h = Bounds().Height();
    const float velTop = VelLaneTop();

    // Rows (pitch lanes) with octave shading + keyboard column (note area only).
    for (float y = kToolbarH; y < velTop; y += kRowH) {
        const int pitch = YToPitch(y + 1);
        SetHighColor(IsBlackKey(pitch) ? ColLaneAlt() : ColLane());
        FillRect(BRect(kKbdW, y, w, y + kRowH));
        if (((pitch % 12) + 12) % 12 == 0) {   // octave boundary (C)
            SetHighColor(ColGrid());
            StrokeLine(BPoint(kKbdW, y + kRowH), BPoint(w, y + kRowH));
        }
    }
    DrawKeyboard(velTop);
    SetHighColor(ColGrid());
    StrokeLine(BPoint(kKbdW, kToolbarH), BPoint(kKbdW, velTop));

    // Vertical bar/beat gridlines, computed in absolute-timeline space so the
    // bar numbering + snap reflect where the region actually sits in the song.
    const Frame left = XToFrame(kKbdW), right = XToFrame(w);   // clip-relative
    if (right > left) {
        const Frame aLeft = left + fClipStart, aRight = right + fClipStart;
        long beat = (long)std::floor(fTempo.BeatAt(aLeft < 0 ? 0 : aLeft));
        if (beat < 0) beat = 0;
        for (;; beat++) {
            const Frame af = fTempo.FrameAt((double)beat);   // absolute
            if (af > aRight) break;
            const float x = FrameToX(af - fClipStart);        // -> relative X
            if (x < kKbdW) continue;
            int bar = 1, bb = 1; fTempo.BarBeat(af, &bar, &bb);
            SetHighColor(bb == 1 ? ColGrid() : ColLaneAlt());
            StrokeLine(BPoint(x, kToolbarH), BPoint(x, h));
            if (bb == 1) {   // absolute bar number at each bar line
                char bl[12]; std::snprintf(bl, sizeof(bl), "%d", bar);
                SetHighColor(ColTextDim());
                DrawString(bl, BPoint(x + 3, kToolbarH + 11));
            }
        }
    }

    // Notes (velocity-heat colored, clipped to the note area).
    for (size_t i = 0; i < fNotes.size(); i++) {
        const MidiNote& n = fNotes[i];
        const float x0 = FrameToX(n.startFrame);
        const float x1 = FrameToX(n.startFrame + n.lengthFrames);
        const float y  = PitchToY(n.pitch);
        if (x1 < kKbdW || x0 > w || y + kRowH < kToolbarH || y > velTop) continue;
        const bool sel = i < fSel.size() && fSel[i];
        BRect nr(std::max(x0, kKbdW), std::max(y + 1, kToolbarH),
                 x1, std::min(y + kRowH - 1, velTop));
        SetHighColor(VelHeat(n.velocity));
        FillRect(nr);
        SetHighColor(sel ? Rgb(255, 240, 140) : ColGrid());
        StrokeRect(nr);
    }

    // Playhead ("tapehead"): a bright vertical line at the transport position.
    if (fPlayhead >= 0) {
        const float px = FrameToX(fPlayhead - fClipStart);
        if (px >= kKbdW && px <= w) {
            SetHighColor(ColPlayhead());
            StrokeLine(BPoint(px, kToolbarH), BPoint(px, h));
        }
    }

    // Bottom lane: note velocity, or one continuous controller.
    SetHighColor(Rgb(18, 20, 25));
    FillRect(BRect(0, velTop, w, h));
    SetHighColor(ColGrid());
    StrokeLine(BPoint(0, velTop), BPoint(w, velTop));
    if (fLane > 0) DrawCcLane(BRect(0, velTop, w, h));
    // Lane selector (click to cycle Vel -> Vol -> Expr -> Pan).
    DrawButton(this, LanePickRect(), kLanes[fLane].label, fLane > 0,
               fLane > 0 ? Rgb(120, 200, 160) : ColAccent());
    const float base = h - 5.0f;
    const float span = kVelLaneH - 12.0f;
    for (size_t i = 0; fLane == 0 && i < fNotes.size(); i++) {
        const MidiNote& n = fNotes[i];
        const float x0 = FrameToX(n.startFrame);
        const float x1 = FrameToX(n.startFrame + n.lengthFrames);
        const float ny = PitchToY(n.pitch);
        // Draw a lollipop iff its note is visible in the grid above — the SAME
        // cull the note loop uses (start/end x AND pitch row). Otherwise a note
        // moved/split/glued to an off-screen pitch leaves an orphan lollipop, and
        // a note whose body scrolls in from the left shows with none: the
        // lollipop appears "separated" from its note.
        if (x1 < kKbdW || x0 > w || ny + kRowH < kToolbarH || ny > velTop)
            continue;
        const float x = std::max(x0, kKbdW);   // align to the note's drawn left edge
        const bool sel = i < fSel.size() && fSel[i];
        const float top = base - (n.velocity / 127.0f) * span;
        SetHighColor(sel ? Rgb(255, 240, 140) : Rgb(110, 180, 250));
        StrokeLine(BPoint(x, base), BPoint(x, top));           // stem
        FillEllipse(BRect(x - 3, top - 3, x + 3, top + 3));    // lollipop head
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

    // Tool palette (drawn last, on top). Buttons + zoom out/in.
    SetHighColor(ColHeader());
    FillRect(BRect(0, 0, w, kToolbarH - 1));
    SetHighColor(ColGrid());
    StrokeLine(BPoint(0, kToolbarH - 1), BPoint(w, kToolbarH - 1));
    for (int i = 0; i < 7; i++) {
        DrawButton(this, ToolRect(i), "", i == (int)fTool, ColAccent());
        icons::DrawTool(this, ToolRect(i).InsetByCopy(4, 3), i,
                        i == (int)fTool ? Rgb(16, 18, 22) : ColText());
    }
    DrawButton(this, ZoomOutRectR(), "\xE2\x88\x92", false);   // minus
    DrawButton(this, ZoomInRectR(),  "+", false);
    DrawButton(this, MidiMenuRectR(), "MIDI", false);
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
    // The model grows the region to cover a note drawn past its end
    // (SetMidiClipNotesCommand only ever grows), so the window a later
    // transform clamps to has to grow with it: otherwise the note the user just
    // drew past the end is the one a quantize skips, and a note near the old
    // edge loses the tail the region now has room for.
    for (const MidiNote& n : fNotes) {
        const Frame e = n.startFrame + (n.lengthFrames > 0 ? n.lengthFrames : 1);
        if (e > fClipLen) fClipLen = e;
    }
    BMessage m(kMsgApplyNotes);
    m.AddInt64("track", (int64)fTrack);
    m.AddInt64("clip", (int64)fClip);
    AddNoteList(&m, fNotes);
    fApply.SendMessage(&m);
}

// --- MIDI transforms -------------------------------------------------------

// Run a transform over the selection -- or over the whole region when nothing
// is selected, which is what a menu item has to do on its own (an edit tool can
// demand a selection; a command cannot). The transform runs HERE, on the view's
// snapshot, and only the result travels: the model command then has no
// selection indices to re-resolve against a note list that may have moved under
// it, and a transform that changed nothing never reaches the undo stack.
void PianoRollView::RunMidiOp(MidiOp op, int param) {
    // Never mid-gesture: a drag holds a pre-transform snapshot (fDragOrig) that
    // the next MouseMoved replays, so a transform landing inside one would be
    // silently undone for the dragged notes and kept for the rest -- a state no
    // single action produced.
    if (fDrag != Drag::None) return;
    const NoteSel sel = SelectedCount() > 0 ? NoteSel(fSel) : NoteSel();
    std::vector<MidiNote> out = fNotes;
    switch (op) {
        case MidiOp::Quantize:
            out = Quantize(fNotes, sel, fTempo, fClipStart, fClipLen, fQuant);
            break;
        case MidiOp::Humanize: {
            // 8 ms of timing and +-10 of velocity: enough movement to read as
            // "played" rather than "programmed", not enough to read as
            // "mistimed". The seed is the clock, so humanizing twice is two
            // takes rather than the same one again -- and the same again next
            // session. (The function is deterministic from its seed; the UI
            // just does not keep it.)
            const Frame jitter = (Frame)(0.008 * fSampleRate);
            out = Humanize(fNotes, sel, fClipLen, jitter, 10,
                           (uint64_t)system_time());
            break;
        }
        case MidiOp::Legato:
            out = Legato(fNotes, sel, fClipLen);
            break;
        case MidiOp::Transpose:
            out = TransposeSemitones(fNotes, sel, param);
            break;
        case MidiOp::Velocity:
            out = ScaleVelocity(fNotes, sel, 1.0f, (float)param);
            break;
    }
    if (NotesEqual(out, fNotes)) return;
    fNotes = std::move(out);
    ApplyMidiOp(op);
    Invalidate();
}

void PianoRollView::ApplyMidiOp(MidiOp op) {
    BMessage m(kMsgApplyMidiOp);
    m.AddInt64("track", (int64)fTrack);
    m.AddInt64("clip", (int64)fClip);
    m.AddInt32("op", (int32)op);
    AddNoteList(&m, fNotes);
    fApply.SendMessage(&m);
}

// This window has no menu bar (it is all drawn), so one toolbar button with a
// popup underneath is the whole discoverable surface for the transforms; 'q' is
// the fast path for the one that gets used constantly.
void PianoRollView::MidiMenu() {
    BPopUpMenu* m = new BPopUpMenu("midi", false, false);
    m->AddItem(new ThemedMenuItem("Quantize" B_UTF8_ELLIPSIS, NULL));
    m->AddItem(new ThemedMenuItem("Quantize (last settings)   q", NULL));
    m->AddSeparatorItem();
    m->AddItem(new ThemedMenuItem("Humanize", NULL));
    m->AddItem(new ThemedMenuItem("Legato", NULL));
    m->AddSeparatorItem();
    m->AddItem(new ThemedMenuItem("Transpose +1", NULL));
    m->AddItem(new ThemedMenuItem("Transpose -1", NULL));
    m->AddItem(new ThemedMenuItem("Transpose +12", NULL));
    m->AddItem(new ThemedMenuItem("Transpose -12", NULL));
    m->AddSeparatorItem();
    m->AddItem(new ThemedMenuItem("Velocity +10", NULL));
    m->AddItem(new ThemedMenuItem("Velocity -10", NULL));

    const BRect r = MidiMenuRectR();
    BMenuItem* sel = m->Go(ConvertToScreen(BPoint(r.left, r.bottom)), false, true);
    const std::string label = sel ? std::string(sel->Label()) : std::string();
    delete m;
    if (label.empty()) return;

    if (label.rfind("Quantize", 0) == 0) {
        if (label.find("last settings") != std::string::npos)
            RunMidiOp(MidiOp::Quantize);
        else
            OpenQuantizeWindow();
    } else if (label == "Humanize") {
        RunMidiOp(MidiOp::Humanize);
    } else if (label == "Legato") {
        RunMidiOp(MidiOp::Legato);
    } else if (label.rfind("Transpose", 0) == 0) {
        int semis = 0;
        std::sscanf(label.c_str(), "Transpose %d", &semis);
        RunMidiOp(MidiOp::Transpose, semis);
    } else if (label.rfind("Velocity", 0) == 0) {
        int delta = 0;
        std::sscanf(label.c_str(), "Velocity %d", &delta);
        RunMidiOp(MidiOp::Velocity, delta);
    }
}

void PianoRollView::OpenQuantizeWindow() {
    const BRect r = MidiMenuRectR();
    const BPoint p = ConvertToScreen(BPoint(r.left, r.bottom));
    (new QuantizeWindow(BRect(p.x, p.y, p.x + 300, p.y + 232), fQuant,
                        BMessenger(this)))->Show();
}

// Snapshot every note's geometry so a group move/resize applies one delta.
void PianoRollView::CaptureDragOrigin() {
    fDragOrig.resize(fNotes.size());
    for (size_t i = 0; i < fNotes.size(); i++)
        fDragOrig[i] = { fNotes[i].startFrame, fNotes[i].lengthFrames,
                         fNotes[i].pitch, fNotes[i].velocity };
}

void PianoRollView::AddNoteAt(BPoint where, bool resizeDrag) {
    MidiNote n;
    Frame start = Snapped(XToFrame(where.x));
    if (start < 0) start = 0;
    n.startFrame   = start;
    n.pitch        = std::clamp(YToPitch(where.y), 0, 127);
    n.velocity     = 100;
    n.lengthFrames = (Frame)fTempo.FramesPerBeatAt(start + fClipStart);
    if (n.lengthFrames < 1) n.lengthFrames = 1;
    fNotes.push_back(n);
    fSel.assign(fNotes.size(), 0);
    fSel.back() = 1;
    if (resizeDrag) {   // pencil: drag right to size the new note
        fDragNote = (int)fNotes.size() - 1;
        CaptureDragOrigin();
        fDrag = Drag::Resize;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    } else {
        Apply();
    }
    Invalidate();
}

void PianoRollView::PaintBrush(BPoint where) {
    if (where.x < kKbdW || where.y < kToolbarH || where.y >= VelLaneTop()) return;
    Frame start = Snapped(XToFrame(where.x));
    if (start < 0) start = 0;
    const int pitch = std::clamp(YToPitch(where.y), 0, 127);
    for (const MidiNote& n : fNotes)               // skip if a note is already here
        if (n.pitch == pitch && n.startFrame == start) return;
    MidiNote n;
    n.startFrame   = start;
    n.pitch        = pitch;
    n.velocity     = 100;
    n.lengthFrames = (Frame)fTempo.FramesPerBeatAt(start + fClipStart) / kSnapDiv;
    if (n.lengthFrames < 1) n.lengthFrames = 1;    // one grid step
    fNotes.push_back(n);
    fSel.assign(fNotes.size(), 0);
    Invalidate();   // committed on mouse-up
}

void PianoRollView::EraseAt(BPoint where) {
    const int h = NoteAt(where);
    if (h < 0) return;
    fNotes.erase(fNotes.begin() + h);
    if ((size_t)h < fSel.size()) fSel.erase(fSel.begin() + h);
    Invalidate();   // committed on mouse-up
}

void PianoRollView::SplitNoteAt(int i, float x) {
    if (i < 0 || (size_t)i >= fNotes.size()) return;
    const Frame cut = Snapped(XToFrame(x));   // clip-relative
    MidiNote& n = fNotes[(size_t)i];
    if (cut <= n.startFrame || cut >= n.startFrame + n.lengthFrames) return;
    MidiNote right = n;
    right.startFrame   = cut;
    right.lengthFrames = n.startFrame + n.lengthFrames - cut;
    n.lengthFrames     = cut - n.startFrame;
    fNotes.push_back(right);
    fSel.assign(fNotes.size(), 0);
    Apply(); Invalidate();
}

void PianoRollView::GlueNoteAt(int i) {
    if (i < 0 || (size_t)i >= fNotes.size()) return;
    const int   pitch = fNotes[(size_t)i].pitch;
    const Frame start = fNotes[(size_t)i].startFrame;
    // Nearest same-pitch note that starts at/after this one.
    int best = -1; Frame bestStart = 0;
    for (size_t j = 0; j < fNotes.size(); j++) {
        if ((int)j == i || fNotes[j].pitch != pitch) continue;
        if (fNotes[j].startFrame >= start
            && (best < 0 || fNotes[j].startFrame < bestStart)) {
            best = (int)j; bestStart = fNotes[j].startFrame;
        }
    }
    if (best < 0) return;
    const Frame end = std::max(start + fNotes[(size_t)i].lengthFrames,
                               fNotes[(size_t)best].startFrame
                               + fNotes[(size_t)best].lengthFrames);
    fNotes[(size_t)i].lengthFrames = end - start;   // extend the kept note
    fNotes.erase(fNotes.begin() + best);            // remove the merged one
    fSel.assign(fNotes.size(), 0);
    Apply(); Invalidate();
}

int PianoRollView::VelNoteAtX(float x) const {
    const float w      = Bounds().Width();
    const float velTop = VelLaneTop();
    int best = -1; float bestd = 8.0f;
    for (size_t i = 0; i < fNotes.size(); i++) {
        const MidiNote& n = fNotes[i];
        const float x0 = FrameToX(n.startFrame);
        const float x1 = FrameToX(n.startFrame + n.lengthFrames);
        const float ny = PitchToY(n.pitch);
        // Only grab a note whose lollipop is actually drawn (same visibility as
        // the velocity-lane draw), so a click can't seize an off-screen note.
        if (x1 < kKbdW || x0 > w || ny + kRowH < kToolbarH || ny > velTop)
            continue;
        const float d = std::fabs(std::max(x0, kKbdW) - x);
        if (d < bestd) { bestd = d; best = (int)i; }
    }
    return best;
}

// --- bottom lane (velocity or one controller) ------------------------------

BRect PianoRollView::LanePickRect() const {
    const float top = VelLaneTop();
    return BRect(4, top + 3, 46, top + 19);
}

// The lane plots 0..127 between the same baseline/span the velocity lollipops
// use, so switching lanes doesn't shift the drawing around.
float PianoRollView::CcValueToY(int value) const {
    const float base = Bounds().Height() - 5.0f;
    const float span = kVelLaneH - 12.0f;
    return base - ((float)std::clamp(value, 0, 127) / 127.0f) * span;
}

int PianoRollView::CcYToValue(float y) const {
    const float base = Bounds().Height() - 5.0f;
    const float span = kVelLaneH - 12.0f;
    if (span <= 0) return 0;
    return std::clamp((int)((base - y) / span * 127.0f + 0.5f), 0, 127);
}

// Nearest event of the active controller within a few pixels of x, else -1.
int PianoRollView::CcEventAtX(float x) const {
    if (fLane <= 0) return -1;
    const int cc = kLanes[fLane].cc;
    int   best = -1;
    float bestD = 7.0f;   // grab radius in pixels
    for (size_t i = 0; i < fEvents.size(); i++) {
        const MidiClipEvent& e = fEvents[i];
        if (e.type != MidiClipEvent::CC || e.data != cc) continue;
        const float d = std::fabs(FrameToX(e.startFrame) - x);
        if (d < bestD) { bestD = d; best = (int)i; }
    }
    return best;
}

// Add a controller point at the click, or move the one already at that frame.
// Snapped in time so drawing a ramp lands on the grid like note entry does.
void PianoRollView::SetCcAt(BPoint where) {
    if (fLane <= 0) return;
    const int cc = kLanes[fLane].cc;
    Frame f = Snapped(XToFrame(where.x));
    if (f < 0) f = 0;
    const int v = CcYToValue(where.y);

    for (MidiClipEvent& e : fEvents)
        if (e.type == MidiClipEvent::CC && e.data == cc && e.startFrame == f) {
            e.value = v;      // one point per controller per frame
            return;
        }
    MidiClipEvent e;
    e.type = MidiClipEvent::CC;
    e.data = cc;
    e.startFrame = f;
    e.value = v;
    fEvents.push_back(e);
}

void PianoRollView::EraseCcAt(BPoint where) {
    const int i = CcEventAtX(where.x);
    if (i < 0) return;
    fEvents.erase(fEvents.begin() + i);
    ApplyEvents();
    Invalidate();
}

void PianoRollView::ApplyEvents() {
    BMessage m(kMsgApplyEvents);
    m.AddInt64("track", (int64)fTrack);
    m.AddInt64("clip",  (int64)fClip);
    for (const MidiClipEvent& e : fEvents) {
        m.AddInt32("et", (int32)e.type);
        m.AddInt32("ed", (int32)e.data);
        m.AddInt32("ev", (int32)e.value);
        m.AddInt64("es", (int64)e.startFrame);
    }
    fApply.SendMessage(&m);
}

// A controller is a STEP function (CcValueAt takes the latest event at or before
// the frame), so draw it as a staircase rather than joining the dots — the shape
// on screen is then exactly what the engine renders.
void PianoRollView::DrawCcLane(BRect lane) {
    const int cc = kLanes[fLane].cc;
    const float w = lane.right;

    // Gather this controller's points in time order.
    std::vector<const MidiClipEvent*> pts;
    for (const MidiClipEvent& e : fEvents)
        if (e.type == MidiClipEvent::CC && e.data == cc) pts.push_back(&e);
    std::sort(pts.begin(), pts.end(),
              [](const MidiClipEvent* a, const MidiClipEvent* b) {
                  return a->startFrame < b->startFrame;
              });

    // Value in force before the first point: what MidiControl falls back to when
    // the controller is absent (unity for vol/expr, centre for pan).
    const int def = (cc == 10) ? 64 : 127;
    SetHighColor(Rgb(120, 200, 160));

    // Walk left to right holding each value until the next point, then stepping
    // to it. Everything is clipped to the grid area, so points scrolled off to
    // the left still contribute the value they hold at the left edge.
    float heldY = CcValueToY(def);
    float x = kKbdW;
    for (const MidiClipEvent* e : pts) {
        const float ex = std::min(FrameToX(e->startFrame), w);
        const float ey = CcValueToY(e->value);
        if (ex > x) StrokeLine(BPoint(x, heldY), BPoint(ex, heldY));   // hold
        if (ex >= kKbdW) StrokeLine(BPoint(ex, heldY), BPoint(ex, ey)); // step
        x = std::max(ex, (float)kKbdW);
        heldY = ey;
    }
    if (x < w) StrokeLine(BPoint(x, heldY), BPoint(w, heldY));   // hold to the end

    // Handles on top, so a point stays grabbable even where steps overlap.
    for (const MidiClipEvent* e : pts) {
        const float x = FrameToX(e->startFrame);
        if (x < kKbdW || x > w) continue;
        const float y = CcValueToY(e->value);
        SetHighColor(Rgb(190, 240, 210));
        FillEllipse(BRect(x - 3, y - 3, x + 3, y + 3));
        SetHighColor(Rgb(30, 60, 50));
        StrokeEllipse(BRect(x - 3, y - 3, x + 3, y + 3));
        SetHighColor(Rgb(120, 200, 160));
    }
}

// Map a y in the velocity lane to a velocity and apply it to the dragged note
// (and, if it's part of the selection, every selected note).
void PianoRollView::SetVelocityFromLane(float y) {
    if (fDragNote < 0 || (size_t)fDragNote >= fNotes.size()) return;
    const float base = Bounds().Height() - 5.0f;
    const float span = kVelLaneH - 12.0f;
    int vel = (int)std::lround((base - y) / span * 127.0f);
    if (vel < 1) vel = 1; if (vel > 127) vel = 127;
    if (fDragNote < (int)fSel.size() && fSel[(size_t)fDragNote]) {
        for (size_t i = 0; i < fNotes.size(); i++)
            if (fSel[i]) fNotes[i].velocity = vel;
    } else {
        fNotes[(size_t)fDragNote].velocity = vel;
    }
}

void PianoRollView::MouseDown(BPoint where) {
    // Tool palette strip (top): pick a tool or zoom.
    if (where.y < kToolbarH) {
        const int t = ToolAt(where);
        if (t >= 0)                              { fTool = (Tool)t; Invalidate(); }
        else if (ZoomOutRectR().Contains(where)) ZoomBy(2.0);
        else if (ZoomInRectR().Contains(where))  ZoomBy(0.5);
        else if (MidiMenuRectR().Contains(where)) MidiMenu();
        return;
    }

    // Bottom lane. The selector cycles which lane is shown; below that it is
    // either the velocity lollipops or the active controller's envelope.
    if (where.y >= VelLaneTop()) {
        if (LanePickRect().Contains(where)) {
            fLane = (fLane + 1) % (int)(sizeof(kLanes) / sizeof(kLanes[0]));
            Invalidate();
            return;
        }
        int32 lb = 0;
        if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
            m->FindInt32("buttons", &lb);
        if (fLane > 0) {           // CC lane: draw points, right-click deletes
            if (where.x < kKbdW) return;
            if (lb & B_SECONDARY_MOUSE_BUTTON) { EraseCcAt(where); return; }
            fDrag = Drag::Cc;
            SetCcAt(where);
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            Invalidate();
            return;
        }
        const int vn = VelNoteAtX(where.x);
        if (vn >= 0) {
            fDrag = Drag::Velocity;
            fDragNote = vn;
            fVelLaneDrag = true;
            CaptureDragOrigin();   // satisfy the MouseMoved drag guard
            SetVelocityFromLane(where.y);
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            Invalidate();
        }
        return;
    }
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

    // Non-pointer tools: each has its own click behavior.
    if (fTool != Tool::Pointer) {
        switch (fTool) {
            case Tool::Pencil:
                if (hit < 0) { AddNoteAt(where, true); return; }
                break;   // on a note: fall through to pointer move/resize below
            case Tool::Brush:
                fDrag = Drag::Brush;
                PaintBrush(where);
                SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
                return;
            case Tool::Eraser:
                if (hit >= 0) EraseAt(where);
                fDrag = Drag::Erase;
                SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
                return;
            case Tool::Scissors:
                if (hit >= 0) SplitNoteAt(hit, where.x);
                return;
            case Tool::Glue:
                if (hit >= 0) GlueNoteAt(hit);
                return;
            case Tool::Velocity:
                if (hit >= 0) {
                    if ((size_t)hit >= fSel.size() || !fSel[(size_t)hit]) SelectOnly(hit);
                    fDragNote = hit;
                    CaptureDragOrigin();
                    fDrag = Drag::Velocity; fVelLaneDrag = false;
                    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
                    Invalidate();
                }
                return;
            default: break;
        }
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
        if (modifiers() & B_CONTROL_KEY)    { fDrag = Drag::Velocity; fVelLaneDrag = false; }
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

    // Empty space (Pointer tool): begin a marquee selection. Additive keeps the
    // prior selection. Creating notes is the Pencil/Brush tools' job.
    fDrag = Drag::Marquee;
    fMarqueeCur = where;
    fPreMarquee = additive ? fSel : std::vector<char>(fNotes.size(), 0);
    if (!additive) ClearSelection();
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    Invalidate();
}

void PianoRollView::MouseMoved(BPoint where, uint32 transit, const BMessage*) {
    // The tool buttons carry no text since M1.6: hovering one names it. (The
    // view is custom-drawn, so the tooltip is driven here rather than by a
    // control per button.)
    if (fDrag == Drag::None) {
        const int hover = ToolAt(where);
        if (hover != fHoverTool) {
            fHoverTool = hover;
            if (hover >= 0) { SetToolTip(kToolNames[hover]); ShowToolTip(); }
            else            { HideToolTip(); }
        }
        if (transit == B_EXITED_VIEW) { fHoverTool = -1; HideToolTip(); }
        return;
    }

    if (fDrag == Drag::Brush) { PaintBrush(where); return; }
    if (fDrag == Drag::Erase) { EraseAt(where);   return; }
    // Painting a controller ramp: keep dropping points along the drag.
    if (fDrag == Drag::Cc)    { SetCcAt(where); Invalidate(); return; }

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
    } else {   // Velocity
        if (fVelLaneDrag) {
            SetVelocityFromLane(where.y);   // lane baseline mapping
        } else {   // note-area Ctrl-drag: whole-height mapping, across selection
            const float t = 1.0f - where.y / Bounds().Height();
            const int vel = std::clamp((int)(t * 127.0f + 0.5f), 1, 127);
            for (size_t i = 0; i < fNotes.size(); i++)
                if (fSel[i]) fNotes[i].velocity = vel;
        }
    }
    Invalidate();
}

void PianoRollView::MouseUp(BPoint) {
    const Drag was = fDrag;
    fDrag = Drag::None;
    fDragNote = -1;
    fVelLaneDrag = false;

    if (was == Drag::Marquee) {
        // A click on empty space that never became a drag just clears the
        // selection (MouseDown already did that). It must NOT add a note: only
        // the Pencil and Brush tools create, and the marquee path is reachable
        // only with the Pointer tool.
        Invalidate();
        return;
    }
    if (was == Drag::Cc) {   // controllers, not notes: their own command
        ApplyEvents();
        Invalidate();
        return;
    }
    if (was != Drag::None) { Apply(); Invalidate(); }
}

void PianoRollView::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgRollQuantize) {
        // The settings dialog. They are remembered here, so both it and 'q'
        // quantize with the same numbers.
        int32 grid = 0, strength = 100, swing = 0;
        bool lengths = false;
        msg->FindInt32("grid", &grid);
        msg->FindInt32("strength", &strength);
        msg->FindInt32("swing", &swing);
        msg->FindBool("lengths", &lengths);
        if (grid < 0 || grid > (int32)QuantGrid::SixteenthTriplet) grid = 0;
        fQuant.grid = (QuantGrid)grid;
        fQuant.strength = (float)std::clamp(strength, (int32)0, (int32)100)
                          / 100.0f;
        fQuant.swingPct = (float)std::clamp(swing, (int32)0, (int32)100);
        fQuant.quantizeLengths = lengths;
        RunMidiOp(MidiOp::Quantize);
        return;
    }
    if (msg->what == B_MOUSE_WHEEL_CHANGED) {
        float dy = 0.0f;
        if (msg->FindFloat("be:wheel_delta_y", &dy) == B_OK && dy != 0.0f) {
            if (modifiers() & B_SHIFT_KEY) {          // Shift+wheel: scroll time
                fScrollFrame += (Frame)((double)dy * 8.0 * fFramesPerPixel);
                if (fScrollFrame < 0) fScrollFrame = 0;
            } else if (modifiers() & B_CONTROL_KEY) { // Ctrl+wheel: zoom time
                ZoomBy(dy > 0 ? 1.2 : 1.0 / 1.2);
            } else {                                   // wheel: scroll pitch
                fTopPitch -= (int)dy * 3;
                if (fTopPitch > 127) fTopPitch = 127;
                if (fTopPitch < 24)  fTopPitch = 24;
            }
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
        case '1': fTool = Tool::Pointer;  Invalidate(); break;
        case '2': fTool = Tool::Pencil;   Invalidate(); break;
        case '3': fTool = Tool::Brush;    Invalidate(); break;
        case '4': fTool = Tool::Eraser;   Invalidate(); break;
        case '5': fTool = Tool::Scissors; Invalidate(); break;
        case '6': fTool = Tool::Glue;     Invalidate(); break;
        case '7': fTool = Tool::Velocity; Invalidate(); break;
        case 'q': case 'Q': {
            // Plain q runs the last-used quantize; an auto-repeat is ignored, or
            // holding the key would stack one undo step per repeat (at strength
            // < 1 each one moves the notes further, so nothing collapses them).
            // Command-Q falls through as before -- the roll has no menu bar, so
            // quitting from here has never worked and this does not change it.
            int32 repeat = 0;
            if (BMessage* cur = Window() ? Window()->CurrentMessage() : nullptr)
                cur->FindInt32("be:key_repeat", &repeat);
            if ((modifiers() & B_COMMAND_KEY) || repeat > 1)
                BView::KeyDown(bytes, numBytes);
            else RunMidiOp(MidiOp::Quantize);
            break;
        }
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
                     daw::Frame clipStart, daw::Frame clipLength,
                     std::vector<MidiNote> notes,
                     std::vector<MidiClipEvent> events,
                     TempoMap tempo, double sampleRate, daw::Frame playhead,
                     BMessenger apply)
    : BWindow(frame, "Piano Roll", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS) {
    fMain = apply;
    fView = new PianoRollView(Bounds(), track, clip, clipStart, clipLength,
                              std::move(notes), std::move(events),
                              tempo, sampleRate, playhead, apply);
    AddChild(fView);
    fView->MakeFocus(true);
}

void PianoRoll::DispatchMessage(BMessage* m, BHandler* h) {
    if (ForwardSpaceToTransport(m, fMain)) return;
    BWindow::DispatchMessage(m, h);
}

void PianoRoll::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgRollPlayhead) {
        int64 ph = -1;
        msg->FindInt64("ph", &ph);
        if (fView) fView->SetPlayhead((daw::Frame)ph);
        return;
    }
    BWindow::MessageReceived(msg);
}

} // namespace daw
