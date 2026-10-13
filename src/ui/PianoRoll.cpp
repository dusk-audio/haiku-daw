#include "PianoRoll.h"

#include "QuantizeWindow.h"
#include "UiMetrics.h"
#include "Widgets.h"
#include "widgets/DawIcons.h"   // the tool glyphs (M1.6)
#include "../model/MidiControl.h"   // CcDefault / CcVoiceEffect (the lane menu)

#include <Cursor.h>
#include <MenuItem.h>
#include <OS.h>   // system_time(): the humanize seed
#include <PopUpMenu.h>
#include <Window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace daw {

// Design metrics (package 51, item J): every one of these goes through
// Themed(), so the roll scales with the app like every other window. They are
// design pixels; the device values live in the KbdW()/RowH()/... accessors.
static constexpr float kKbdW      = 54.0f;   // piano keyboard column width
static constexpr float kRowH      = 12.0f;   // pixels per semitone
static constexpr float kEdge      = 5.0f;    // resize grab zone (both ends)
static constexpr float kVelLaneH  = 64.0f;   // bottom lane
static constexpr float kToolbarH  = 28.0f;   // top tool palette strip

// The tool palette is icon-only since M1.6, so these names are what the hover
// tooltip shows and what the buttons are called in the help.
static const char* kToolNames[7] = { "Pointer", "Pencil", "Brush", "Eraser",
                                     "Scissors", "Glue", "Velocity" };

// The toolbar's non-tool buttons (ToolbarButtonAt's answer).
enum { kBtnGrid = 0, kBtnZoomOut = 1, kBtnZoomIn = 2, kBtnMidi = 3 };

static const char* NoteName(int pitch, char* buf, size_t n) {
    static const char* names[12] = { "C","C#","D","D#","E","F","F#","G","G#",
                                     "A","A#","B" };
    const int oct = pitch / 12 - 1;
    std::snprintf(buf, n, "%s%d", names[((pitch % 12) + 12) % 12], oct);
    return buf;
}

static bool IsBlackKey(int pitch) {
    switch (((pitch % 12) + 12) % 12) {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}

// The name a controller is known by, or nullptr when it has no common one.
static const char* CcCommonName(int cc) {
    switch (cc) {
        case 0:   return "Bank";
        case 1:   return "Mod";
        case 2:   return "Breath";
        case 4:   return "Foot";
        case 5:   return "Porta";
        case 7:   return "Vol";
        case 10:  return "Pan";
        case 11:  return "Expr";
        case 64:  return "Sus";
        case 65:  return "Porta";
        case 66:  return "Sost";
        case 67:  return "Soft";
        case 68:  return "Legato";
        case 71:  return "Reso";
        case 74:  return "Cutoff";
        case 84:  return "Porta";
        case 91:  return "Reverb";
        case 93:  return "Chorus";
        case 95:  return "Phaser";
        case 123: return "AllOff";
        default:  return nullptr;
    }
}

// Bottom-lane label. Short, because the button is 42 px wide: an abbreviation
// for the lanes people know by one ("Mod", "Vol", "Pan", "Expr", "Sus"), "Bend"
// for the bend, "CC n" for the rest. It always WRITES what it returns into
// `buf`, because the button draws the buffer — the first cut returned the name
// without copying it and the lane said "CC1" for the mod wheel (a screenshot
// found it; the test now reads the caption back through LaneLabel()).
const char* PianoRollView::LaneLabelFor(int lane, char* buf, size_t n) {
    if (lane == kLanePitchBend) { std::snprintf(buf, n, "Bend"); return buf; }
    const char* label = "Vel";
    if (lane >= 0) {
        // On the button only the shortest names fit; the menu (CcItemLabel)
        // spells out the rest ("CC64 Sus — sustain pedal").
        switch (lane) {
            case 1:  label = "Mod";  break;
            case 7:  label = "Vol";  break;
            case 10: label = "Pan";  break;
            case 11: label = "Expr"; break;
            case 64: label = "Sus";  break;
            default: label = nullptr; break;
        }
        if (label == nullptr) {
            std::snprintf(buf, n, "CC%d", lane);
            return buf;
        }
    }
    std::snprintf(buf, n, "%s", label);
    return buf;
}

// --- geometry (item J: every metric through Themed()) ----------------------

float PianoRollView::KbdW() const    { return Themed(kKbdW); }
float PianoRollView::RowH() const    { return Themed(kRowH); }
float PianoRollView::LaneH() const   { return Themed(kVelLaneH); }
float PianoRollView::ToolbarH() const { return Themed(kToolbarH); }

// The toolbar's layout, in device pixels, from the view's own width: the tree
// of rects the drawing, the hit test and the hover all read. A hard-coded
// offset is how the MIDI button ended up off a narrow dock's right edge, and
// why the whole strip failed to grow at 150 %.
void PianoRollView::LayoutToolbar(RollToolbarLayout* out) const {
    const float scale = ThemeScale();
    RollToolbarLayout l =
        RollToolbarFor(scale > 0 ? Bounds().Width() / scale : Bounds().Width(), 7);
    l.gridX    *= scale;
    l.zoomOutX *= scale;
    l.zoomInX  *= scale;
    l.midiX    *= scale;
    l.midiW    *= scale;
    if (out) *out = l;
}

BRect PianoRollView::ToolRect(int i) const {
    const float x = Themed(kRollToolX) + i * Themed(kRollToolPitch);
    return BRect(x, Themed(kRollToolTop), x + Themed(kRollToolW),
                 Themed(kRollToolBot));
}
BRect PianoRollView::GridRect() const {
    RollToolbarLayout l; LayoutToolbar(&l);
    return BRect(l.gridX, Themed(kRollToolTop), l.gridX + Themed(kRollGridW),
                 Themed(kRollToolBot));
}
BRect PianoRollView::ZoomOutRect() const {
    RollToolbarLayout l; LayoutToolbar(&l);
    return BRect(l.zoomOutX, Themed(kRollToolTop),
                 l.zoomOutX + Themed(kRollZoomW), Themed(kRollToolBot));
}
BRect PianoRollView::ZoomInRect() const {
    RollToolbarLayout l; LayoutToolbar(&l);
    return BRect(l.zoomInX, Themed(kRollToolTop),
                 l.zoomInX + Themed(kRollZoomW), Themed(kRollToolBot));
}
BRect PianoRollView::MidiRect() const {
    RollToolbarLayout l; LayoutToolbar(&l);
    if (l.midiW <= 0.0f) return BRect();
    return BRect(l.midiX, Themed(kRollToolTop), l.midiX + l.midiW,
                 Themed(kRollToolBot));
}

int PianoRollView::ToolAt(BPoint where) const {
    for (int i = 0; i < 7; i++)
        if (ToolRect(i).Contains(where)) return i;
    return -1;
}

int PianoRollView::ToolbarButtonAt(BPoint where) const {
    if (GridRect().Contains(where))    return kBtnGrid;
    if (ZoomOutRect().Contains(where)) return kBtnZoomOut;
    if (ZoomInRect().Contains(where))  return kBtnZoomIn;
    const BRect midi = MidiRect();
    if (midi.Width() > 0.0f && midi.Contains(where)) return kBtnMidi;
    return -1;
}

// --- construction ----------------------------------------------------------

PianoRollView::PianoRollView(BRect frame, TrackId track, ClipId clip,
                             Frame clipStart, Frame clipLength,
                             std::vector<MidiNote> notes,
                             std::vector<MidiClipEvent> events,
                             TempoMap tempo, double sampleRate, Frame playhead,
                             BMessenger apply)
    // Full update on resize: the bottom lane hangs off the bottom edge, and
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
    // item J: the roll's text scales with the app, like every other window's.
    BFont scaleFont(ThemeFont());
    SetFont(&scaleFont);

    // Open focused on the tapehead. The main window only pushes the playhead
    // during transport, so seed it here to draw it right away, and centre the
    // view on it -- but only while it is inside this region. Outside it there is
    // nothing to edit there, so fall back to the region's head (scroll 0).
    const Frame rel = playhead - fClipStart;
    const float visible = (float)((double)(frame.Width() - KbdW())
                                  * fFramesPerPixel);
    if (playhead >= 0 && rel >= 0 && (clipLength <= 0 || rel <= clipLength)) {
        fPlayhead = playhead;
        if (visible > 0) {
            fScrollFrame = rel - (Frame)(visible * 0.5);
            if (fScrollFrame < 0) fScrollFrame = 0;
        }
    }
}

void PianoRollView::ApplyTheme() {
    SetViewColor(ColBackground());
}

// The transport's playhead, pushed by the main window's pulse. M2.2: the roll
// FOLLOWS it -- when it leaves the visible span the view scrolls so it stays on
// screen (the same 10 % margin policy the timeline's chase uses), which is what
// makes a docked or popped-out roll usable while the song plays. Since package
// 51 the follow is the View menu's policy (SetFollow), not a hard-coded true.
Frame PianoRollView::VisibleSpan() const {
    const float w = Bounds().Width() - KbdW();
    if (w < 1.0f) return 0;
    return (Frame)((double)w * fFramesPerPixel);
}

void PianoRollView::SetPlayhead(Frame absFrame) {
    if (absFrame == fPlayhead) return;
    const Frame rel = absFrame - fClipStart;
    if (absFrame >= 0 && fFollow) {
        const Frame visible = VisibleSpan();
        if (visible > 0) {
            const Frame right = fScrollFrame + visible;
            if (rel < fScrollFrame || rel > right - visible / 10) {
                fScrollFrame = rel - visible / 10;
                if (fScrollFrame < 0) fScrollFrame = 0;
            }
        }
    }
    fPlayhead = absFrame;
    Invalidate();
}

// --- the model ⇄ snapshot sync (items A/B) ---------------------------------

RollSync PianoRollView::SyncRegion(TrackId track, ClipId clip, Frame start,
                                   Frame length, std::vector<MidiNote> notes,
                                   std::vector<MidiClipEvent> events,
                                   uint64_t serial, bool gone) {
    RollRegion model;
    if (!gone) {
        model.track  = track;
        model.clip   = clip;
        model.start  = start;
        model.length = length;
        model.notes  = std::move(notes);    // by value: take them, do not copy
        model.events = std::move(events);
    }
    RollRegion mine;
    mine.track  = fTrack;
    mine.clip   = fClip;
    mine.start  = fClipStart;
    mine.length = fClipLen;
    mine.notes  = fNotes;
    mine.events = fEvents;

    const RollSync action = DecideRollSync(model, mine);
    // The base for every later commit is the model's state NOW, whatever the
    // action: a Keep push still means "this is the serial your snapshot
    // corresponds to" (an unrelated edit elsewhere bumped it).
    fSerial = serial;
    fSynced = true;

    switch (action) {
        case RollSync::Close:
            // The caller decides what closing means (the dock empty-state, the
            // window quitting); the view only stops holding a snapshot whose
            // region is gone.
            AuditionEnd();
            fDrag = Drag::None;
            fDragNote = -1;
            fCcDragIndex = -1;
            break;
        case RollSync::Reload:
            // The model changed under the snapshot (an undo, another view's
            // edit, a split that trimmed the window). The in-flight gesture
            // loses: it was built on a state that no longer exists, and
            // committing it is exactly the stale-list bug.
            AuditionEnd();
            fDrag = Drag::None;
            fDragNote = -1;
            fCcDragIndex = -1;
            fNotes  = std::move(model.notes);
            fEvents = std::move(model.events);
            fSel.assign(fNotes.size(), 0);
            fDragOrig.clear();
            fMarqueeCur = fDownPoint = BPoint(-1.0f, -1.0f);
            fClipStart = model.start;
            fClipLen   = model.length;
            if (fScrollFrame < 0) fScrollFrame = 0;
            Invalidate();
            break;
        case RollSync::Repoint:
            // Only the region's start moved: the notes are still the right
            // notes (they are clip-relative), so the selection survives -- the
            // user moved the region in the arrangement, not here.
            fClipStart = model.start;
            fClipLen   = model.length;
            Invalidate();
            break;
        case RollSync::Keep:
        default:
            break;
    }
    return action;
}

// --- selection helpers ------------------------------------------------------

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

// --- editing feel (item H): the clipboard, paste, duplicate ---------------

void PianoRollView::DeleteSelection() { DeleteSelected(); }

void PianoRollView::SelectAll() {
    fSel.assign(fNotes.size(), 1);
    Invalidate();
}

void PianoRollView::CopySelection() {
    // Qualified: this member shares the free helper's name (RollEdit.h).
    fClipboard = daw::CopySelection(fNotes, fSel, 0);
}

// Paste at the playhead when it is inside the region (that is what "paste"
// means while the tapehead is parked somewhere), else at the region head; the
// whole run keeps its internal shape, and the first pasted note lands on the
// target.
void PianoRollView::PasteClipboard() {
    if (fClipboard.empty()) return;
    const Frame target = (fPlayhead >= 0 && fPlayhead >= fClipStart)
                       ? fPlayhead - fClipStart : 0;
    Frame first = fClipboard.front().startFrame;
    for (const MidiNote& n : fClipboard) first = std::min(first, n.startFrame);
    std::vector<MidiNote> copies =
        daw::CopySelection(fClipboard,
                           std::vector<char>(fClipboard.size(), 1),
                           target - first);
    if (AppendSelecting(fNotes, fSel, std::move(copies)) == 0) return;
    fTool = Tool::Pointer;
    Apply();
    Invalidate();
}

void PianoRollView::DuplicateSelection() {
    if (SelectedCount() == 0) return;
    // One grid step from the first selected note's own line, so a triplet note
    // duplicates by a triplet step.
    Frame anchor = -1;
    for (size_t i = 0; i < fNotes.size() && i < fSel.size(); i++)
        if (fSel[i]) { anchor = fNotes[i].startFrame; break; }
    if (anchor < 0) return;
    const Frame step = SnapStep(fTempo, anchor + fClipStart, fSnap);
    std::vector<MidiNote> copies = daw::CopySelection(fNotes, fSel, step);
    if (AppendSelecting(fNotes, fSel, std::move(copies)) == 0) return;
    Apply();
    Invalidate();
}

// The whole region in view ('f'), with a little air on both sides. A region
// with no length (or no notes) fits a bar so the view does not zoom to
// infinity.
void PianoRollView::ZoomToFitRegion() {
    Frame span = fClipLen;
    if (span <= 0) {
        for (const MidiNote& n : fNotes)
            span = std::max(span, n.startFrame + n.lengthFrames);
    }
    if (span <= 0) span = (Frame)fTempo.FramesPerBeatAt(0) * 4;
    const float w = Bounds().Width() - KbdW();
    if (w < 1.0f) return;
    double fpp = (double)span * 1.08 / (double)w;   // 8 % of air at the right
    if (fpp < 8.0)    fpp = 8.0;
    if (fpp > 8192.0) fpp = 8192.0;
    fFramesPerPixel = fpp;
    fScrollFrame = 0;   // the region's head at the keyboard edge
    Invalidate();
}

// --- geometry ---------------------------------------------------------------

float PianoRollView::FrameToX(Frame f) const {
    return KbdW() + (float)((double)(f - fScrollFrame) / fFramesPerPixel);
}
Frame PianoRollView::XToFrame(float x) const {
    return fScrollFrame + (Frame)((double)(x - KbdW()) * fFramesPerPixel);
}
float PianoRollView::PitchToY(int pitch) const {
    return ToolbarH() + (float)(fTopPitch - pitch) * RowH();
}
int PianoRollView::YToPitch(float y) const {
    return fTopPitch - (int)std::floor((y - ToolbarH()) / RowH());
}

// The zone a note's own body splits into: the two edges resize, the middle
// moves (item H).
PianoRollView::Zone PianoRollView::ZoneOf(const MidiNote& n, BPoint where) const {
    const float x0 = FrameToX(n.startFrame);
    const float x1 = FrameToX(n.startFrame + n.lengthFrames);
    const float edge = Themed(kEdge);
    // A note drawn narrower than two grab zones keeps a middle: the edges may
    // not overlap past the note's own centre.
    const float grab = std::min(edge, (x1 - x0) * 0.5f);
    if (where.x <= x0 + grab) return Zone::LeftEdge;
    if (where.x >= x1 - grab) return Zone::RightEdge;
    return Zone::Middle;
}

void PianoRollView::ZoomBy(double factor) {
    // The keys and the strip's buttons anchor on the roll's playhead when it is
    // in view (so the bar being worked on stays put), else on the centre.
    float x = (KbdW() + Bounds().right) * 0.5f;
    const float px = FrameToX(fPlayhead - fClipStart);
    if (fPlayhead >= 0 && px >= KbdW() && px <= Bounds().right) x = px;
    ZoomAnchoredAt(factor, x);
}

// Zoom keeping the frame under `x` where it is (M2.2's pointer-anchored zoom).
void PianoRollView::ZoomAnchoredAt(double factor, float x) {
    const Frame anchor = XToFrame(x);
    fFramesPerPixel *= factor;
    if (fFramesPerPixel < 8)    fFramesPerPixel = 8;
    if (fFramesPerPixel > 8192) fFramesPerPixel = 8192;
    fScrollFrame = anchor - (Frame)((double)(x - KbdW()) * fFramesPerPixel);
    if (fScrollFrame < 0) fScrollFrame = 0;
    Invalidate();
}

// Snap in the song's grid, in the region's own space (RollEdit.h): the grid
// the toolbar's field selects (item E), not a welded 1/16.
Frame PianoRollView::Snapped(Frame f) const {   // f is clip-relative
    if (modifiers() & B_SHIFT_KEY) return f < 0 ? 0 : f;
    return SnapClipRelative(fTempo, f, fClipStart, fSnap);
}

float PianoRollView::VelLaneTop() const { return Bounds().Height() - LaneH(); }

// --- drawing ----------------------------------------------------------------

// The keyboard column, drawn as a piano rather than as one stripe per row: the
// white keys are one surface whose seams fall where a real keyboard's do (on
// the row edge between B|C and E|F, under the middle of the black key
// everywhere else), and the black keys are shorter, rounded, and on top.
//
// The key colours are the instrument's own, like the velocity heat map: a piano
// keyboard is white and black in every DAW's dark mode, so these are not
// derived from the panel (item J's rule is about the panel's chrome; the keys
// are the content the column exists to show).
void PianoRollView::DrawKeyboard(float velTop) {
    const float right = KbdW() - Themed(2.0f);
    const float blackW = std::floor(KbdW() * 0.6f);
    PushState();
    ClipToRect(BRect(0, ToolbarH(), right, velTop));

    SetHighColor(Rgb(222, 225, 230));                         // the white keys
    FillRect(BRect(0, ToolbarH(), right, velTop));

    SetHighColor(Rgb(150, 154, 162));                         // their seams
    for (float y = ToolbarH(); y < velTop; y += RowH()) {
        const int pitch = YToPitch(y + 1);
        if (IsBlackKey(pitch)) continue;
        // The seam below this white key: on the row edge when the next key
        // down is white too, else through the black key's middle.
        const float seam = IsBlackKey(pitch - 1) ? y + RowH() * 1.5f
                                                 : y + RowH();
        StrokeLine(BPoint(0, seam), BPoint(right, seam));
    }

    for (float y = ToolbarH(); y < velTop; y += RowH()) {       // the black keys
        const int pitch = YToPitch(y + 1);
        if (!IsBlackKey(pitch)) continue;
        const BRect key(0, y + 1, blackW, y + RowH() - 1);
        SetHighColor(Rgb(30, 32, 37));
        FillRoundRect(key, 2.0f, 2.0f);
        SetHighColor(Rgb(70, 74, 82));                         // a lit edge
        StrokeLine(BPoint(key.left, key.top), BPoint(key.right - 2, key.top));
    }

    for (float y = ToolbarH(); y < velTop; y += RowH()) {       // C labels
        const int pitch = YToPitch(y + 1);
        if (((pitch % 12) + 12) % 12 != 0) continue;
        char nb[8]; NoteName(pitch, nb, sizeof(nb));
        SetHighColor(Rgb(70, 74, 82));
        DrawString(nb, BPoint(right - 3 - StringWidth(nb), y + RowH() - 2));
    }
    PopState();

    SetHighColor(ColGrid());                                  // keyboard edge
    StrokeLine(BPoint(right + 1, ToolbarH()), BPoint(right + 1, velTop));
}

// Item I: where the region starts and ends, and which notes it does NOT play.
// A note outside [0, fClipLen) is kept in the model but silent (MidiOps' window
// rule), so it is drawn dimmed -- "make this two bars longer" stopped being
// guesswork. Drawn after the notes, before the lane.
void PianoRollView::DrawRegionBounds(float velTop, float w) {
    const float x0 = FrameToX(0);
    const float x1 = FrameToX(fClipLen);
    SetHighColor(ColTextDim());
    SetPenSize(2.0f);
    if (x0 >= KbdW() && x0 <= w)
        StrokeLine(BPoint(x0, ToolbarH()), BPoint(x0, velTop));
    if (x1 >= KbdW() && x1 <= w)
        StrokeLine(BPoint(x1, ToolbarH()), BPoint(x1, velTop));
    SetPenSize(1.0f);
}

void PianoRollView::Draw(BRect) {
    const float w = Bounds().Width(), h = Bounds().Height();
    const float velTop = VelLaneTop();

    // Rows (pitch lanes) with octave shading + keyboard column (note area only).
    for (float y = ToolbarH(); y < velTop; y += RowH()) {
        const int pitch = YToPitch(y + 1);
        SetHighColor(IsBlackKey(pitch) ? ColLaneAlt() : ColLane());
        FillRect(BRect(KbdW(), y, w, y + RowH()));
        if (((pitch % 12) + 12) % 12 == 0) {   // octave boundary (C)
            SetHighColor(ColGrid());
            StrokeLine(BPoint(KbdW(), y + RowH()), BPoint(w, y + RowH()));
        }
    }
    DrawKeyboard(velTop);
    SetHighColor(ColGrid());
    StrokeLine(BPoint(KbdW(), ToolbarH()), BPoint(KbdW(), velTop));

    // Vertical bar/beat gridlines, computed in absolute-timeline space so the
    // bar numbering + snap reflect where the region actually sits in the song.
    const Frame left = XToFrame(KbdW()), right = XToFrame(w);   // clip-relative
    if (right > left) {
        const Frame aLeft = left + fClipStart, aRight = right + fClipStart;
        long beat = (long)std::floor(fTempo.BeatAt(aLeft < 0 ? 0 : aLeft));
        if (beat < 0) beat = 0;
        for (;; beat++) {
            const Frame af = fTempo.FrameAt((double)beat);   // absolute
            if (af > aRight) break;
            const float x = FrameToX(af - fClipStart);        // -> relative X
            if (x < KbdW()) continue;
            int bar = 1, bb = 1; fTempo.BarBeat(af, &bar, &bb);
            SetHighColor(bb == 1 ? ColGrid() : ColLaneAlt());
            StrokeLine(BPoint(x, ToolbarH()), BPoint(x, h));
            if (bb == 1) {   // absolute bar number at each bar line
                char bl[12]; std::snprintf(bl, sizeof(bl), "%d", bar);
                SetHighColor(ColTextDim());
                DrawString(bl, BPoint(x + 3, ToolbarH() + Themed(11.0f)));
            }
        }
    }

    // The region's own window, drawn UNDER the notes so the edges do not cut
    // through a note that crosses them.
    DrawRegionBounds(velTop, w);

    // Notes (velocity-heat colored, clipped to the note area). A note the
    // region does not play is dimmed (item I) but still editable: it is kept in
    // the model, and widening the region brings it back.
    for (size_t i = 0; i < fNotes.size(); i++) {
        const MidiNote& n = fNotes[i];
        const float x0 = FrameToX(n.startFrame);
        const float x1 = FrameToX(n.startFrame + n.lengthFrames);
        const float y  = PitchToY(n.pitch);
        if (x1 < KbdW() || x0 > w || y + RowH() < ToolbarH() || y > velTop) continue;
        const bool sel = i < fSel.size() && fSel[i];
        BRect nr(std::max(x0, KbdW()), std::max(y + 1, ToolbarH()),
                 x1, std::min(y + RowH() - 1, velTop));
        const rgb_color fill = VelHeat(n.velocity);
        SetHighColor(NoteInRegion(n, fClipLen)
                         ? fill
                         : MixColor(fill, ColLane(), 0.55f));
        FillRect(nr);
        // A selected note's outline is whichever of black/white reads on its
        // own fill (the theme's rule), two pixels wide; an unselected one keeps
        // the grid-coloured hairline it always had.
        if (sel) {
            SetHighColor(LabelOn(fill));
            SetPenSize(2.0f);
            StrokeRect(nr);
            SetPenSize(1.0f);
        } else {
            SetHighColor(ColGrid());
            StrokeRect(nr);
        }
    }

    // Playhead ("tapehead"): a bright vertical line at the transport position.
    if (fPlayhead >= 0) {
        const float px = FrameToX(fPlayhead - fClipStart);
        if (px >= KbdW() && px <= w) {
            SetHighColor(ColPlayhead());
            StrokeLine(BPoint(px, ToolbarH()), BPoint(px, h));
        }
    }

    // Bottom lane: note velocity, pitch bend, or one continuous controller.
    // The well takes the theme's own (item J): the literal dark band was
    // invisible-as-a-bug in System mode and did not follow a mode switch.
    SetHighColor(ColLcd());
    FillRect(BRect(0, velTop, w, h));
    SetHighColor(ColGrid());
    StrokeLine(BPoint(0, velTop), BPoint(w, velTop));
    if (fLane != kLaneVelocity) DrawCcLane(BRect(0, velTop, w, h));
    // Lane selector: names the lane it is showing, and opens the menu of them.
    DrawButton(this, LanePickRect(), fLaneLabel, fLane != kLaneVelocity,
               fLane != kLaneVelocity ? ColMidiAccent() : ColAccent());
    if (fLane == kLaneVelocity) {
        const float base = h - Themed(5.0f);
        const float span = LaneH() - Themed(12.0f);
        for (size_t i = 0; i < fNotes.size(); i++) {
            const MidiNote& n = fNotes[i];
            const float x0 = FrameToX(n.startFrame);
            const float x1 = FrameToX(n.startFrame + n.lengthFrames);
            const float ny = PitchToY(n.pitch);
            // Draw a lollipop iff its note is visible in the grid above — the
            // SAME cull the note loop uses (start/end x AND pitch row).
            // Otherwise a note moved/split/glued to an off-screen pitch leaves
            // an orphan lollipop, and a note whose body scrolls in from the
            // left shows with none: the lollipop appears "separated" from its
            // note.
            if (x1 < KbdW() || x0 > w || ny + RowH() < ToolbarH() || ny > velTop)
                continue;
            const float x = std::max(x0, KbdW());   // align to the note's drawn left edge
            const bool sel = i < fSel.size() && fSel[i];
            const float top = base - LaneValueFraction(fLane, n.velocity) * span;
            SetHighColor(sel ? ColText() : ColAccent());
            StrokeLine(BPoint(x, base), BPoint(x, top));           // stem
            FillEllipse(BRect(x - 3, top - 3, x + 3, top + 3));    // lollipop head
        }
    }

    // Marquee rectangle (rubber-band selection in progress).
    if (fDrag == Drag::Marquee) {
        BRect m(std::min(fDownPoint.x, fMarqueeCur.x),
                std::min(fDownPoint.y, fMarqueeCur.y),
                std::max(fDownPoint.x, fMarqueeCur.x),
                std::max(fDownPoint.y, fMarqueeCur.y));
        SetHighColor(ColText());
        SetPenSize(1.0f);
        StrokeRect(m, B_MIXED_COLORS);
    }

    // Tool palette (drawn last, on top). Buttons + the grid field, zoom and
    // MIDI, laid out from the view's own width (item J).
    SetHighColor(ColHeader());
    FillRect(BRect(0, 0, w, ToolbarH() - 1));
    SetHighColor(ColGrid());
    StrokeLine(BPoint(0, ToolbarH() - 1), BPoint(w, ToolbarH() - 1));
    for (int i = 0; i < 7; i++) {
        DrawButton(this, ToolRect(i), "", i == (int)fTool, ColAccent());
        icons::DrawTool(this, ToolRect(i).InsetByCopy(Themed(4.0f), Themed(3.0f)),
                        i, i == (int)fTool ? Rgb(16, 18, 22) : ColText());
        if (i != (int)fTool && i == fHoverTool) {   // hover highlight
            SetHighColor(ColAccent());
            StrokeRoundRect(ToolRect(i), Themed(3.0f), Themed(3.0f));
        }
    }
    // The snap field: its LABEL is the active grid (so triplets and Off read at
    // a glance), and it is lit while snapping is on — the arrange's own field,
    // the same SnapGrid label table (item E).
    DrawButton(this, GridRect(), SnapGridLabel(fSnap), fSnap.On(), ColAccent());
    DrawButton(this, ZoomOutRect(), "\xE2\x88\x92", false);   // minus
    DrawButton(this, ZoomInRect(),  "+", false);
    const BRect midi = MidiRect();
    if (midi.Width() > 0.0f) {
        // A shrunk MIDI button (a narrow dock) truncates its label rather than
        // spilling over the button's own edge (UI_GUIDELINES §3).
        BString ml("MIDI");
        TruncateString(&ml, B_TRUNCATE_END, midi.Width() - Themed(8.0f));
        DrawButton(this, midi, ml.String(), false);
    }
    for (int b = kBtnGrid; b <= kBtnMidi; b++) {
        if (b != fHoverBtn) continue;
        BRect r;
        switch (b) {
            case kBtnGrid:    r = GridRect(); break;
            case kBtnZoomOut: r = ZoomOutRect(); break;
            case kBtnZoomIn:  r = ZoomInRect(); break;
            default:          r = midi; break;
        }
        if (r.Width() <= 0.0f) continue;
        SetHighColor(ColAccent());
        StrokeRoundRect(r, Themed(3.0f), Themed(3.0f));
    }
}

int PianoRollView::NoteAt(BPoint where) const {
    for (int i = (int)fNotes.size() - 1; i >= 0; i--) {
        const MidiNote& n = fNotes[(size_t)i];
        const float x0 = FrameToX(n.startFrame);
        const float x1 = FrameToX(n.startFrame + n.lengthFrames);
        const float y  = PitchToY(n.pitch);
        if (where.x >= x0 && where.x <= x1 && where.y >= y && where.y <= y + RowH())
            return i;
    }
    return -1;
}

// --- committing to the model (items A/B: every commit says its base) --------

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
    // Only a synchronised view names the state it commits against; one that has
    // never been pushed has no base to be stale against (Synced()).
    if (fSynced) m.AddInt64("base", (int64)fSerial);
    AddRollNotes(&m, fNotes);
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
    if (fSynced) m.AddInt64("base", (int64)fSerial);
    m.AddInt32("op", (int32)op);
    AddRollNotes(&m, fNotes);
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

    const BRect r = MidiRect();
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
    const BRect r = MidiRect();
    const BPoint p = ConvertToScreen(BPoint(r.left, r.bottom));
    (new QuantizeWindow(BRect(p.x, p.y, p.x + 300, p.y + 232), fQuant,
                        BMessenger(this)))->Show();
}

// --- snapping (item E) ------------------------------------------------------

// The grid popup: the plan's values (bar, 1/2 … 1/32), the triplet modifier and
// Off — the arrange's own menu, from the same kit-free SnapGrid header, so the
// field's label and the entry that set it cannot disagree.
void PianoRollView::OpenGridMenu() {
    BPopUpMenu* menu = new BPopUpMenu("snap", false, false);
    int n = 0;
    const SnapDivision* div = SnapDivisions(&n);
    for (int i = 0; i < n; i++) {
        BMenuItem* it = new ThemedMenuItem(div[i].label, NULL);
        it->SetMarked(fSnap.kind == div[i].kind);
        menu->AddItem(it);
    }
    menu->AddSeparatorItem();
    BMenuItem* trip = new ThemedMenuItem("Triplets", NULL);
    trip->SetMarked(fSnap.triplet);
    menu->AddItem(trip);
    BMenuItem* off = new ThemedMenuItem("Off", NULL);
    off->SetMarked(!fSnap.On());
    menu->AddItem(off);

    const BRect r = GridRect();
    BMenuItem* sel = menu->Go(ConvertToScreen(BPoint(r.left, r.bottom)), false,
                              true);
    if (sel) {
        const int idx = menu->IndexOf(sel);
        if (sel == trip)          fSnap.triplet = !fSnap.triplet;
        else if (sel == off)      fSnap.kind = SnapKind::Off;
        else if (idx >= 0 && idx < n) fSnap.kind = div[idx].kind;
        Invalidate();
    }
    delete menu;
}

// --- audition (item D) ------------------------------------------------------

// Everything the roll sounds goes through here: the main window owns the
// engine and the preference that mutes it.
void PianoRollView::Audition(int pitch, int velocity, bool on) {
    BMessage m(kMsgRollAudition);
    AddRollAudition(&m, fTrack, pitch, velocity, on);
    fApply.SendMessage(&m);
}

void PianoRollView::AuditionEnd() {
    if (fAuditionPitch < 0) return;
    Audition(fAuditionPitch, 100, false);
    fAuditionPitch = -1;
}

// A short note: on, then off. The synth rings out its release tail, so this
// reads as a click rather than a blip.
void PianoRollView::AuditionBlip(int pitch, int velocity) {
    Audition(pitch, velocity, true);
    Audition(pitch, velocity, false);
}

// --- note editing -----------------------------------------------------------

// Snapshot every note's geometry so a group move/resize applies one delta.
void PianoRollView::CaptureDragOrigin() {
    fDragOrig = CaptureGeometry(fNotes);   // index-aligned with fNotes
    fDragCount0 = fNotes.size();
}

// Whether the gesture changed anything at all (or created a note, which has to
// be committed even when its geometry ended where it started).
bool PianoRollView::DragChanged() const {
    if (fDragMustCommit) return true;
    if (fNotes.size() != fDragCount0) return true;
    if (fDragOrig.size() != fNotes.size()) return true;   // no snapshot: assume
    for (size_t i = 0; i < fNotes.size(); i++) {
        const MidiNote& n = fNotes[i];
        const NoteGeometry& o = fDragOrig[i];
        if (n.startFrame != o.start || n.lengthFrames != o.len
            || n.pitch != o.pitch || n.velocity != o.velocity)
            return true;
    }
    return false;
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
    AuditionBlip(n.pitch, n.velocity);   // item D: a drawn note sounds
    if (resizeDrag) {   // pencil: drag right to size the new note
        fDragNote = (int)fNotes.size() - 1;
        CaptureDragOrigin();
        fDragMustCommit = true;   // the note is new even if the drag sizes nothing
        fDrag = Drag::Resize;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    } else {
        Apply();
    }
    Invalidate();
}

int PianoRollView::PaintBrush(BPoint where) {
    if (where.x < KbdW() || where.y < ToolbarH() || where.y >= VelLaneTop()) return 0;
    Frame start = Snapped(XToFrame(where.x));
    if (start < 0) start = 0;
    const int pitch = std::clamp(YToPitch(where.y), 0, 127);
    for (const MidiNote& n : fNotes)               // skip if a note is already here
        if (n.pitch == pitch && n.startFrame == start) return 0;
    MidiNote n;
    n.startFrame   = start;
    n.pitch        = pitch;
    n.velocity     = 100;
    n.lengthFrames = SnapStep(fTempo, start + fClipStart, fSnap);
    if (n.lengthFrames < 1) n.lengthFrames = 1;    // one grid step
    fNotes.push_back(n);
    fSel.assign(fNotes.size(), 0);
    AuditionBlip(n.pitch, n.velocity);   // item D
    Invalidate();   // committed on mouse-up
    return 1;
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
        if (x1 < KbdW() || x0 > w || ny + RowH() < ToolbarH() || ny > velTop)
            continue;
        const float d = std::fabs(std::max(x0, KbdW()) - x);
        if (d < bestd) { bestd = d; best = (int)i; }
    }
    return best;
}

// --- bottom lane (velocity, pitch bend, or one controller) -----------------

// Pick the lane. Clamps instead of failing, and re-labels the button, so the
// view always says which lane it is showing (UI_GUIDELINES §3).
void PianoRollView::SetLane(int lane) {
    if (!LaneIsValid(lane)) return;
    if (lane == fLane) return;
    AuditionEnd();            // a bend/CC lane change mid-note must not stick
    fLane = lane;
    LaneLabelFor(lane, fLaneLabel, sizeof(fLaneLabel));
    Invalidate();
}

// Clear the lane's stream as one undoable step (the menu's "Clear lane"). The
// count comes back so the caller (and the tests) can tell an empty lane from a
// real edit -- and so no empty undo step is pushed.
int PianoRollView::ClearLane() {
    if (fLane == kLaneVelocity) return 0;   // velocity is a note field, not a lane
    const int removed = CcLaneClear(fEvents, fLane);
    if (removed > 0) { ApplyEvents(); Invalidate(); }
    return removed;
}

// The lane menu's label for a controller: always the number (the message the
// picker reads back), the common name where there is one, and — from the
// model's own table (MidiControl.h: CcVoiceEffect) — what it does to a voice,
// or that it does nothing yet. One source, so the menu cannot advertise an
// effect the engine does not have (item G).
static BString CcItemLabel(int cc) {
    BString label;
    label << "CC" << cc;
    if (const char* name = CcCommonName(cc)) label << " " << name;
    label << " \xE2\x80\x94 ";                     // an em dash
    if (const char* effect = CcVoiceEffect(cc)) label << effect;
    else                                       label << "no effect yet";
    return label;
}

// The lane button's menu. Lists velocity and pitch bend, then the controllers
// THIS region already carries (so a recorded pedal or wheel lane is one click
// away), then all 128 in four groups, then Clear lane. The item's label carries
// the number ("CC64 Sus — sustain pedal"), and the pick below parses it — the
// same dispatch-by-label the MIDI menu uses.
void PianoRollView::LaneMenu() {
    BPopUpMenu* m = new BPopUpMenu("lane", false, false);
    m->AddItem(new BMenuItem("Velocity", NULL));
    m->AddItem(new BMenuItem("Pitch Bend", NULL));

    // What is actually in the region, in controller order, deduplicated.
    bool have[128] = {};
    for (const MidiClipEvent& e : fEvents)
        if (e.type == MidiClipEvent::CC && e.data >= 0 && e.data < 128)
            have[e.data] = true;
    bool any = false;
    for (int cc = 0; cc < 128; cc++) {
        if (!have[cc]) continue;
        if (!any) { m->AddSeparatorItem(); any = true; }
        m->AddItem(new BMenuItem(CcItemLabel(cc).String(), NULL));
    }

    m->AddSeparatorItem();
    for (int group = 0; group < 4; group++) {
        char gname[40];
        std::snprintf(gname, sizeof(gname), "Controllers %d-%d",
                      group * 32, group * 32 + 31);
        BPopUpMenu* sub = new BPopUpMenu(gname);
        for (int cc = group * 32; cc < group * 32 + 32; cc++)
            sub->AddItem(new BMenuItem(CcItemLabel(cc).String(), NULL));
        m->AddItem(new BMenuItem(sub));
    }
    if (fLane != kLaneVelocity) {   // item F: a way to empty the lane again
        m->AddSeparatorItem();
        const int n = LanePointCount(fEvents, fLane);
        BString label("Clear lane");
        if (n > 0) label << " (" << n << ")";
        BMenuItem* clear = new BMenuItem(label.String(), NULL);
        clear->SetEnabled(n > 0);
        m->AddItem(clear);
    }

    const BRect r = LanePickRect();
    BMenuItem* sel = m->Go(ConvertToScreen(BPoint(r.left, r.top)), false, true);
    const std::string label = sel ? std::string(sel->Label()) : std::string();
    delete m;
    if (label.empty()) return;
    if (label == "Velocity")   { SetLane(kLaneVelocity); return; }
    if (label == "Pitch Bend") { SetLane(kLanePitchBend); return; }
    if (label.rfind("Clear lane", 0) == 0) { ClearLane(); return; }
    // Only a "CC<n> ..." item names a lane: a submenu's parent item ("Controllers
    // 0-31") can come back selected too, and must not be read as a controller.
    int cc = -1;
    if (label.rfind("CC", 0) == 0 && std::sscanf(label.c_str(), "CC%d", &cc) == 1)
        SetLane(cc);
}

BRect PianoRollView::LanePickRect() const {
    const float top = VelLaneTop();
    return BRect(Themed(4.0f), top + Themed(3.0f), Themed(46.0f),
                 top + Themed(19.0f));
}

// The lane plots its own value range between the same baseline/span the
// velocity lollipops use, so switching lanes doesn't shift the drawing around,
// and a 14-bit bend gets the same full-height travel a 7-bit CC does.
float PianoRollView::CcValueToY(int value) const {
    const float base = Bounds().Height() - Themed(5.0f);
    const float span = LaneH() - Themed(12.0f);
    return base - (float)LaneValueFraction(fLane, value) * span;
}

int PianoRollView::CcYToValue(float y) const {
    const float base = Bounds().Height() - Themed(5.0f);
    const float span = LaneH() - Themed(12.0f);
    if (span <= 0) return LaneValueDefault(fLane);
    return LaneValueAtFraction(fLane, (double)(base - y) / (double)span);
}

// Nearest point of the active lane within a few pixels of x, else -1. The
// radius is the same 7 px, converted to frames at the current zoom, so the
// grab zone is a constant on screen whatever the zoom.
int PianoRollView::CcEventAtX(float x) const {
    if (fLane == kLaneVelocity) return -1;
    const Frame tol = (Frame)std::ceil(7.0 * fFramesPerPixel);
    return CcPointNearFrame(fEvents, fLane, XToFrame(x), tol);
}

// The ramp path (empty space): add a point at the click, snapped in time so
// drawing a ramp lands on the grid like note entry does.
void PianoRollView::SetCcAt(BPoint where) {
    if (fLane == kLaneVelocity) return;
    Frame f = Snapped(XToFrame(where.x));
    if (f < 0) f = 0;
    CcPointSet(fEvents, fLane, f, CcYToValue(where.y));
}

// Item F: a drag that STARTED on a point moves THAT point (RollEdit.h's
// CcPointMove takes the index as the drag's identity), instead of writing a new
// point at every new x and leaving the old one plus a staircase behind.
void PianoRollView::MoveCcDrag(BPoint where) {
    if (fLane == kLaneVelocity || fCcDragIndex < 0) return;
    Frame f = Snapped(XToFrame(where.x));
    if (f < 0) f = 0;
    // The index is the drag's identity, and it MOVES when the point absorbs a
    // neighbour below it: CcPointMove hands the new one back, so the next
    // mouse-move still holds the point the user grabbed.
    if (!CcPointMove(fEvents, fLane, &fCcDragIndex, f, CcYToValue(where.y)))
        fCcDragIndex = -1;   // the point is gone: stop dragging, do not paint
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
    if (fSynced) m.AddInt64("base", (int64)fSerial);
    AddRollEvents(&m, fEvents);
    fApply.SendMessage(&m);
}

// A controller is a STEP function (CcValueAt takes the latest event at or
// before the frame), so draw it as a staircase rather than joining the dots —
// the shape on screen is then exactly what the engine renders. The same code
// draws the bend lane: the lane (not the CC number) is the address, and the
// value range comes from LaneValue* (RollEdit.h).
void PianoRollView::DrawCcLane(BRect lane) {
    const float w = lane.right;
    const rgb_color line = ColMidiAccent();

    // Gather this lane's points in time order.
    std::vector<const MidiClipEvent*> pts;
    for (const MidiClipEvent& e : fEvents)
        if (EventInLane(e, fLane)) pts.push_back(&e);
    std::sort(pts.begin(), pts.end(),
              [](const MidiClipEvent* a, const MidiClipEvent* b) {
                  return a->startFrame < b->startFrame;
              });

    // Value in force before the first point: the same fallback the engine and
    // Exporter use where they act on this lane (MidiControl.h), so the
    // staircase drawn here is the level that sounds. The faint line makes the
    // fallback visible, which is what an empty lane otherwise hides.
    const int def = LaneValueDefault(fLane);
    SetHighColor(ColGrid());
    const float defY = CcValueToY(def);
    StrokeLine(BPoint(KbdW(), defY), BPoint(w, defY));

    SetHighColor(line);

    // Walk left to right holding each value until the next point, then stepping
    // to it. Everything is clipped to the grid area, so points scrolled off to
    // the left still contribute the value they hold at the left edge.
    float heldY = defY;
    float x = KbdW();
    for (const MidiClipEvent* e : pts) {
        const float ex = std::min(FrameToX(e->startFrame), w);
        const float ey = CcValueToY(e->value);
        if (ex > x) StrokeLine(BPoint(x, heldY), BPoint(ex, heldY));   // hold
        if (ex >= KbdW()) StrokeLine(BPoint(ex, heldY), BPoint(ex, ey)); // step
        x = std::max(ex, KbdW());
        heldY = ey;
    }
    if (x < w) StrokeLine(BPoint(x, heldY), BPoint(w, heldY));   // hold to the end

    // Handles on top, so a point stays grabbable even where steps overlap.
    for (const MidiClipEvent* e : pts) {
        const float x = FrameToX(e->startFrame);
        if (x < KbdW() || x > w) continue;
        const float y = CcValueToY(e->value);
        SetHighColor(line);
        FillEllipse(BRect(x - 3, y - 3, x + 3, y + 3));
        SetHighColor(LabelOn(line));
        StrokeEllipse(BRect(x - 3, y - 3, x + 3, y + 3));
    }
}

// Map a y in the velocity lane to a velocity and apply it to the dragged note
// (and, if it's part of the selection, every selected note).
void PianoRollView::SetVelocityFromLane(float y) {
    if (fDragNote < 0 || (size_t)fDragNote >= fNotes.size()) return;
    const float base = Bounds().Height() - Themed(5.0f);
    const float span = LaneH() - Themed(12.0f);
    int vel = (int)std::lround((base - y) / span * 127.0f);
    if (vel < 1) vel = 1; if (vel > 127) vel = 127;
    if (fDragNote < (int)fSel.size() && fSel[(size_t)fDragNote]) {
        for (size_t i = 0; i < fNotes.size(); i++)
            if (fSel[i]) fNotes[i].velocity = vel;
    } else {
        fNotes[(size_t)fDragNote].velocity = vel;
    }
}

// --- the pointer ------------------------------------------------------------

// The pointer's own feedback (item H). The roll had no cursor at all: a 5 px
// resize zone nobody can see and an Alt-drag nobody can discover. Same ids the
// arrangement uses for the same gestures.
//
// The four cursors are held for the life of the process: a view holds the
// cursor's app-server TOKEN, which dies with the BCursor object, so deleting
// one would leave the view pointing at nothing (TimelineView keeps its own
// table for the same reason).
void PianoRollView::ApplyCursor(int which) {
    if (which == fCursorShown) return;
    fCursorShown = which;
    static const BCursor* const kDefault =
        new BCursor(B_CURSOR_ID_SYSTEM_DEFAULT);
    static const BCursor* const kGrab    = new BCursor(B_CURSOR_ID_GRAB);
    static const BCursor* const kTrim    = new BCursor(B_CURSOR_ID_RESIZE_EAST_WEST);
    static const BCursor* const kDraw    = new BCursor(B_CURSOR_ID_CROSS_HAIR);
    const BCursor* c = kDefault;
    switch (which) {
        case kCursorDrag: c = kGrab; break;
        case kCursorTrim: c = kTrim; break;
        case kCursorDraw: c = kDraw; break;
        default: break;
    }
    SetViewCursor(c, true);
}

void PianoRollView::MouseDown(BPoint where) {
    MakeFocus(true);   // a click here owns the keyboard (the keys are editor-local)
    fDragMustCommit = false;

    // Tool palette strip (top): pick a tool or one of the buttons.
    if (where.y < ToolbarH()) {
        const int t = ToolAt(where);
        if (t >= 0) { fTool = (Tool)t; Invalidate(); return; }
        switch (ToolbarButtonAt(where)) {
            case kBtnGrid:    OpenGridMenu(); break;
            case kBtnZoomOut: ZoomBy(2.0); break;
            case kBtnZoomIn:  ZoomBy(0.5); break;
            case kBtnMidi:    MidiMenu(); break;
            default: break;
        }
        return;
    }

    // Bottom lane. The selector names the lane it is showing and opens the menu
    // of them; below that it is either the velocity lollipops or the active
    // lane's envelope.
    if (where.y >= VelLaneTop()) {
        if (LanePickRect().Contains(where)) {
            LaneMenu();
            return;
        }
        int32 lb = 0;
        if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
            m->FindInt32("buttons", &lb);
        if (fLane != kLaneVelocity) {   // CC/bend lane: drag, right-click deletes
            if (where.x < KbdW()) return;
            if (lb & B_SECONDARY_MOUSE_BUTTON) { EraseCcAt(where); return; }
            const int hit = CcEventAtX(where.x);
            // The point has to be under the pointer in BOTH axes: a click far
            // above or below it is a new point, not a grab (and the old code's
            // grab-anywhere is how a "move" became a paint).
            const bool onPoint =
                hit >= 0
                && std::fabs(CcValueToY(fEvents[(size_t)hit].value) - where.y)
                       <= Themed(6.0f);
            if (onPoint) {               // item F: grab the point, do not paint
                fDrag = Drag::CcMove;
                fCcDragIndex = hit;
                MoveCcDrag(where);       // the drag starts where the pointer is
            } else {                     // empty space: paint a new point
                fDrag = Drag::Cc;
                fCcDragIndex = -1;
                SetCcAt(where);
            }
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
            AuditionBlip(fNotes[(size_t)vn].pitch, fNotes[(size_t)vn].velocity);
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            Invalidate();
        }
        return;
    }

    // The keyboard column: audition the key under the pointer (item D). It used
    // to be a no-op; every DAW sounds the note here.
    if (where.x < KbdW()) {
        const int pitch = std::clamp(YToPitch(where.y), 0, 127);
        fAuditionPitch = pitch;
        Audition(pitch, 100, true);
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        return;
    }

    int32 buttons = 0;
    if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
        m->FindInt32("buttons", &buttons);
    const bool rightClick = (buttons & B_SECONDARY_MOUSE_BUTTON) != 0;
    // Alt means "duplicate under the pointer" (item H). BOTH bits: Haiku maps
    // the PC's Alt to B_COMMAND_KEY and the Option/Windows key to B_OPTION_KEY
    // (the arrangement's own Alt gestures accept both, TimelineView.cpp:2340).
    const bool altDup = (modifiers() & (B_OPTION_KEY | B_COMMAND_KEY)) != 0;

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
                CaptureDragOrigin();   // so a drag that adds nothing commits nothing
                fDrag = Drag::Brush;
                if (PaintBrush(where) > 0) fDragMustCommit = true;
                SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
                return;
            case Tool::Eraser:
                CaptureDragOrigin();
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
        // Alt on a note: which gesture it is depends on what the pointer does
        // next — a drag duplicates, a click toggles the selection. Deciding
        // here instead would take one of the two away, and they share a key on
        // a PC keyboard.
        if (altDup) {
            fDrag = Drag::AltPending;
            fDragNote = hit;
            fDownPoint = where;
            CaptureDragOrigin();
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            return;
        }
        // Plain click: if the note isn't already selected, make it the sole
        // selection; a click on an already-selected note keeps the group.
        if ((size_t)hit >= fSel.size() || !fSel[(size_t)hit])
            SelectOnly(hit);

        int grab = hit;
        const MidiNote& n = fNotes[(size_t)grab];
        fDragNote = grab;
        CaptureDragOrigin();
        const Zone zone = ZoneOf(n, where);
        if (modifiers() & B_CONTROL_KEY) { fDrag = Drag::Velocity; fVelLaneDrag = false; }
        else if (zone == Zone::LeftEdge)   fDrag = Drag::ResizeLeft;
        else if (zone == Zone::RightEdge)  fDrag = Drag::Resize;
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
    // prior selection (the Alt bits, as they always were here — the note-hit
    // case above turns them into the click-vs-drag pair instead). Creating
    // notes is the Pencil/Brush tools' job.
    fDrag = Drag::Marquee;
    fMarqueeCur = where;
    fPreMarquee = altDup ? fSel : std::vector<char>(fNotes.size(), 0);
    if (!altDup) ClearSelection();
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    Invalidate();
}

void PianoRollView::MouseMoved(BPoint where, uint32 transit, const BMessage*) {
    // The pointer's last position: the Ctrl+wheel zoom anchors on it (M2.2),
    // and a wheel message carries no position of its own.
    fHoverPos = where;
    fHoverSeen = true;

    // A held keyboard key: release it when the pointer leaves the column (a
    // note-off has to arrive even when the mouse-up lands somewhere else).
    if (fAuditionPitch >= 0
        && (transit == B_EXITED_VIEW || where.x >= KbdW())) {
        Audition(fAuditionPitch, 100, false);
        fAuditionPitch = -1;
    }

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
        const int btn = ToolbarButtonAt(where);
        if (btn != fHoverBtn) {
            fHoverBtn = btn;
            if (btn >= 0) {
                static const char* const kTips[4] = {
                    "Snap grid (g)", "Zoom out (-)", "Zoom in (+)",
                    "MIDI transforms (q quantizes)" };
                SetToolTip(kTips[btn]);
                ShowToolTip();
            } else if (hover < 0) {
                HideToolTip();
            }
            Invalidate(BRect(0, 0, Bounds().right, ToolbarH()));
        }
        if (transit == B_EXITED_VIEW) {
            fHoverTool = -1;
            fHoverBtn = -1;
            HideToolTip();
        }

        // Cursor feedback (item H): an edge resizes, a note body drags, the
        // pencil and brush draw. Everything else is the system arrow.
        int want = kCursorDefault;
        if (where.y < ToolbarH()) {
            want = kCursorDefault;
        } else if (where.y >= VelLaneTop()) {
            want = (fLane != kLaneVelocity && CcEventAtX(where.x) >= 0)
                       ? kCursorDrag : kCursorDefault;
        } else if (where.x < KbdW()) {
            want = kCursorDraw;
        } else if (fTool == Tool::Pencil || fTool == Tool::Brush) {
            want = kCursorDraw;
        } else if (fTool == Tool::Pointer) {
            const int h = NoteAt(where);
            if (h >= 0) {
                const Zone z = ZoneOf(fNotes[(size_t)h], where);
                want = (z == Zone::Middle) ? kCursorDrag : kCursorTrim;
            }
        }
        ApplyCursor(want);
        return;
    }

    // Alt on a note: the pointer MOVED, so this is the duplicate-and-drag the
    // gesture promises. The copies are made now, selected, and the drag below
    // carries them.
    if (fDrag == Drag::AltPending) {
        const float dx = where.x - fDownPoint.x, dy = where.y - fDownPoint.y;
        if (dx * dx + dy * dy < 9.0f) return;    // still a click, not a drag
        const int hit = fDragNote;
        if (hit < 0 || (size_t)hit >= fNotes.size()) { fDrag = Drag::None; return; }
        if ((size_t)hit >= fSel.size() || !fSel[(size_t)hit]) SelectOnly(hit);
        int rank = 0;
        for (int i = 0; i < hit && i < (int)fSel.size(); i++)
            if (fSel[i]) rank++;
        const int before = (int)fNotes.size();
        std::vector<MidiNote> copies = daw::CopySelection(fNotes, fSel, 0);
        if (AppendSelecting(fNotes, fSel, std::move(copies)) == 0) {
            fDrag = Drag::None;
            return;
        }
        const int grab = before + rank;
        fDragNote = grab;
        CaptureDragOrigin();
        fDragMustCommit = true;      // the copies are new
        AuditionBlip(fNotes[(size_t)grab].pitch, fNotes[(size_t)grab].velocity);
        fDrag = Drag::Move;
        fGrabOffset  = XToFrame(fDownPoint.x) - fNotes[(size_t)grab].startFrame;
        fPitchOffset = fNotes[(size_t)grab].pitch - YToPitch(fDownPoint.y);
        // fall through: this same move already carries the copies
    }

    if (fDrag == Drag::Brush) { PaintBrush(where); return; }
    if (fDrag == Drag::Erase) { EraseAt(where);   return; }
    // The lane's two drags: painting a ramp drops points along the way; moving
    // holds ONE point (item F).
    if (fDrag == Drag::Cc)     { SetCcAt(where); Invalidate(); return; }
    if (fDrag == Drag::CcMove) { MoveCcDrag(where); Invalidate(); return; }

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
                                && y + RowH() >= m.top && y <= m.bottom;
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
        const NoteGeometry& g = fDragOrig[(size_t)fDragNote];
        Frame newStart = Snapped(XToFrame(where.x) - fGrabOffset);
        if (newStart < 0) newStart = 0;
        const Frame dFrame = newStart - g.start;
        const int   dPitch = std::clamp(YToPitch(where.y) + fPitchOffset, 0, 127)
                             - g.pitch;
        daw::NudgeSelection(fNotes, fSel, fDragOrig, dFrame, dPitch);
    } else if (fDrag == Drag::Resize) {
        const NoteGeometry& g = fDragOrig[(size_t)fDragNote];
        Frame newLen = Snapped(XToFrame(where.x)) - g.start;
        if (newLen < 1) newLen = 1;
        const Frame dLen = newLen - g.len;
        for (size_t i = 0; i < fNotes.size(); i++) {
            if (!fSel[i]) continue;
            Frame len = fDragOrig[i].len + dLen;
            if (len < 1) len = 1;
            fNotes[i].lengthFrames = len;
        }
    } else if (fDrag == Drag::ResizeLeft) {   // item H: the left edge
        const NoteGeometry& g = fDragOrig[(size_t)fDragNote];
        Frame newStart = Snapped(XToFrame(where.x));
        if (newStart < 0) newStart = 0;
        // Never past the note's own end (a zero-length note is invisible and
        // ungrabbable); the note's END stays put, and the start moves.
        const Frame end = g.start + g.len;
        if (newStart > end - 1) newStart = end - 1;
        const Frame dStart = newStart - g.start;
        for (size_t i = 0; i < fNotes.size(); i++) {
            if (!fSel[i]) continue;
            Frame s = fDragOrig[i].start + dStart;
            if (s < 0) s = 0;
            Frame e = fDragOrig[i].start + fDragOrig[i].len;
            // A note dragged past its own end keeps one frame of length.
            if (e - s < 1) e = s + 1;
            fNotes[i].startFrame   = s;
            fNotes[i].lengthFrames = e - s;
        }
    } else if (fDrag == Drag::Velocity) {   // note-area Ctrl-drag, or the lane
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
    fCcDragIndex = -1;
    if (was == Drag::AltPending) {
        // Alt without a drag: the additive selection toggle (the gesture the
        // Alt bits mean in the arrangement too).
        if (fDragNote >= 0 && (size_t)fDragNote < fSel.size()) {
            fSel[(size_t)fDragNote] = fSel[(size_t)fDragNote] ? 0 : 1;
            fDragNote = -1;
            Invalidate();
        }
        return;
    }
    if (fAuditionPitch >= 0) {          // the keyboard column's key goes up
        Audition(fAuditionPitch, 100, false);
        fAuditionPitch = -1;
    }

    if (was == Drag::Marquee) {
        // A click on empty space that never became a drag just clears the
        // selection (MouseDown already did that). It must NOT add a note: only
        // the Pencil and Brush tools create, and the marquee path is reachable
        // only with the Pointer tool.
        Invalidate();
        return;
    }
    if (was == Drag::Cc || was == Drag::CcMove) {   // controllers, their own command
        ApplyEvents();
        Invalidate();
        return;
    }
    if (was == Drag::None) return;
    // A gesture that changed nothing is not an edit: committing it pushed an
    // "Edit Notes" undo step whose undo does nothing (a bare click to select a
    // note did exactly that).
    if (DragChanged()) Apply();
    Invalidate();
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
    if (msg->what == kMsgRollFollow) {
        // The View menu's one mark drives the timeline and every roll (item C).
        bool on = true;
        msg->FindBool("on", &on);
        SetFollow(on);
        return;
    }
    if (msg->what == kMsgRollRegion) {
        // The main window's push (items A/B). The docked view is hosted here;
        // a popped-out window gets the same message and closes itself when the
        // region is gone (the window handles that below).
        int64 track = 0, clip = 0, start = 0, len = 0, serial = 0;
        bool gone = false;
        msg->FindInt64("track", &track);
        msg->FindInt64("clip", &clip);
        msg->FindInt64("start", &start);
        msg->FindInt64("len", &len);
        msg->FindInt64("serial", &serial);
        msg->FindBool("gone", &gone);
        if (gone) {
            SyncRegion(kInvalidTrackId, kInvalidClipId, 0, 0, {}, {}, (uint64_t)serial,
                       true);
            return;
        }
        SyncRegion((TrackId)track, (ClipId)clip, (daw::Frame)start,
                   (daw::Frame)len, ParseRollNotes(msg), ParseRollEvents(msg),
                   (uint64_t)serial, false);
        return;
    }
    if (msg->what == B_MOUSE_WHEEL_CHANGED) {
        float dy = 0.0f;
        if (msg->FindFloat("be:wheel_delta_y", &dy) == B_OK && dy != 0.0f) {
            const uint32 mods = EventModifiers(msg);
            if (mods & B_SHIFT_KEY) {                 // Shift+wheel: scroll time
                fScrollFrame += (Frame)((double)dy * 8.0 * fFramesPerPixel);
                if (fScrollFrame < 0) fScrollFrame = 0;
            } else if (mods & B_CONTROL_KEY) {        // Ctrl+wheel: zoom time,
                float x = fHoverPos.x;                // anchored on the pointer
                if (x < KbdW()) x = KbdW();
                if (x > Bounds().right) x = Bounds().right;
                if (!fHoverSeen) x = (KbdW() + Bounds().right) * 0.5f;
                ZoomAnchoredAt(dy > 0 ? 1.2 : 1.0 / 1.2, x);
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
    const Frame page = (Frame)((Bounds().Width() - KbdW()) * fFramesPerPixel);
    const bool cmd = (modifiers() & B_COMMAND_KEY) != 0;
    // The nudge step is the ACTIVE grid's own line spacing (item E): with
    // triplets selected, one press moves a triplet step.
    auto gridStep = [&]() -> Frame {
        Frame anchor = fScrollFrame;
        for (size_t i = 0; i < fNotes.size() && i < fSel.size(); i++)
            if (fSel[i]) { anchor = fNotes[i].startFrame; break; }
        return SnapStep(fTempo, anchor + fClipStart, fSnap);
    };
    // Nudge-or-scroll: with a selection the arrows move it (item H), without
    // one they pan as they always did.
    auto nudge = [&](Frame frames, int pitches) {
        if (SelectedCount() == 0) return false;
        const std::vector<NoteGeometry> orig = CaptureGeometry(fNotes);
        if (daw::NudgeSelection(fNotes, fSel, orig, frames, pitches) == 0)
            return true;
        Apply();
        Invalidate();
        return true;
    };
    switch (bytes[0]) {
        case B_LEFT_ARROW:
            if (nudge(-gridStep(), 0)) break;
            fScrollFrame -= page / 4; if (fScrollFrame < 0) fScrollFrame = 0;
            Invalidate(); break;
        case B_RIGHT_ARROW:
            if (nudge(gridStep(), 0)) break;
            fScrollFrame += page / 4; Invalidate(); break;
        case B_UP_ARROW:
            if (nudge(0, +1)) break;
            BView::KeyDown(bytes, numBytes); break;
        case B_DOWN_ARROW:
            if (nudge(0, -1)) break;
            BView::KeyDown(bytes, numBytes); break;
        case '+': case '=': ZoomBy(0.5); break;   // anchored (item H)
        case '-': case '_': ZoomBy(2.0); break;
        case 'f': case 'F': ZoomToFitRegion(); break;
        case 'g': case 'G': OpenGridMenu(); break;
        case B_DELETE: case B_BACKSPACE: DeleteSelected(); break;
        case '1': fTool = Tool::Pointer;  Invalidate(); break;
        case '2': fTool = Tool::Pencil;   Invalidate(); break;
        case '3': fTool = Tool::Brush;    Invalidate(); break;
        case '4': fTool = Tool::Eraser;   Invalidate(); break;
        case '5': fTool = Tool::Scissors; Invalidate(); break;
        case '6': fTool = Tool::Glue;     Invalidate(); break;
        case '7': fTool = Tool::Velocity; Invalidate(); break;
        case 'c': case 'C': case 3:       // Cmd-C: copy (item H)
            if (cmd) CopySelection();
            else BView::KeyDown(bytes, numBytes);
            break;
        case 'x': case 'X': case 24:      // Cmd-X: cut
            if (cmd) { CopySelection(); DeleteSelected(); }
            else BView::KeyDown(bytes, numBytes);
            break;
        case 'v': case 'V': case 22:      // Cmd-V: paste
            if (cmd) PasteClipboard();
            else BView::KeyDown(bytes, numBytes);
            break;
        case 'd': case 'D':               // Cmd-D: duplicate
            if (cmd) DuplicateSelection();
            else BView::KeyDown(bytes, numBytes);
            break;
        case 'q': case 'Q': {
            // Plain q runs the last-used quantize; an auto-repeat is ignored, or
            // holding the key would stack one undo step per repeat (at strength
            // < 1 each one moves the notes further, so nothing collapses them).
            // Command-Q falls through as before -- the roll has no menu bar, so
            // quitting from here has never worked and this does not change it.
            int32 repeat = 0;
            if (BMessage* cur = Window() ? Window()->CurrentMessage() : nullptr)
                cur->FindInt32("be:key_repeat", &repeat);
            if (cmd || repeat > 1)
                BView::KeyDown(bytes, numBytes);
            else RunMidiOp(MidiOp::Quantize);
            break;
        }
        case 1: case 'a': case 'A':   // Command-A: select all
            if (cmd) {
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
                     BMessenger apply, const char* trackName)
    // Zoomable (item J): the editor is the one window whose whole point is
    // more room, and B_NOT_ZOOMABLE made it the one window that could not take
    // it. The title names the region's track -- the dock strip does, so the
    // pop-out must too ("Piano Roll" alone named nothing).
    : BWindow(frame, "Piano Roll", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS) {
    fMain = apply;
    if (trackName != nullptr && trackName[0] != '\0') {
        BString title("Piano Roll \xE2\x80\x94 ");
        title << trackName;
        SetTitle(title.String());
    }
    fView = new PianoRollView(Bounds(), track, clip, clipStart, clipLength,
                              std::move(notes), std::move(events),
                              tempo, sampleRate, playhead, apply);
    AddChild(fView);
    fView->MakeFocus(true);
}

void PianoRoll::DispatchMessage(BMessage* m, BHandler* h) {
    if (ForwardUndoRedo(m, fMain)) return;           // Cmd-Z from a child window
    if (ForwardSpaceToTransport(m, fMain)) return;
    // The clipboard keys have to be intercepted HERE, not left to the view's
    // KeyDown: BWindow::_HandleKeyDown ends with "we always eat the event if the
    // command key was pressed", so a Command-modified key-down never reaches the
    // focus view at all (Cmd-Z worked only because the forwarding above runs
    // first). The view's own Cmd branches stay as belt and braces; this is the
    // path that runs.
    if (m != nullptr && m->what == B_KEY_DOWN && fView != nullptr) {
        int32 mods = 0;
        const char* bytes = nullptr;
        if (m->FindInt32("modifiers", &mods) == B_OK
            && (mods & B_COMMAND_KEY) != 0
            && m->FindString("bytes", &bytes) == B_OK && bytes != nullptr) {
            if (bytes[0] == 'c' || bytes[0] == 'C') { fView->CopySelection(); return; }
            if (bytes[0] == 'x' || bytes[0] == 'X') {
                fView->CopySelection(); fView->DeleteSelection(); return;
            }
            if (bytes[0] == 'v' || bytes[0] == 'V') { fView->PasteClipboard(); return; }
            if (bytes[0] == 'd' || bytes[0] == 'D') { fView->DuplicateSelection(); return; }
            if (bytes[0] == 'a' || bytes[0] == 'A') { fView->SelectAll(); return; }
        }
    }
    BWindow::DispatchMessage(m, h);
}

void PianoRoll::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgRollPlayhead) {
        int64 ph = -1;
        msg->FindInt64("ph", &ph);
        if (fView) fView->SetPlayhead((daw::Frame)ph);
        return;
    }
    if (msg->what == kMsgRollFollow) {
        // The View menu's one follow policy (item C), pushed here the same way
        // the docked view is told directly.
        bool on = true;
        msg->FindBool("on", &on);
        if (fView) fView->SetFollow(on);
        return;
    }
    if (msg->what == kMsgRollRegion) {
        // The same push the docked view gets (items A/B). A region that is gone
        // closes the window: it was opened on something that no longer exists,
        // and every edit it could post would address a dead clip id.
        int64 track = 0, clip = 0, start = 0, len = 0, serial = 0;
        bool gone = false;
        msg->FindInt64("track", &track);
        msg->FindInt64("clip", &clip);
        msg->FindInt64("start", &start);
        msg->FindInt64("len", &len);
        msg->FindInt64("serial", &serial);
        msg->FindBool("gone", &gone);
        const RollSync action =
            gone ? fView->SyncRegion(kInvalidTrackId, kInvalidClipId, 0, 0, {},
                                     {}, (uint64_t)serial, true)
                 : fView->SyncRegion((TrackId)track, (ClipId)clip,
                                     (daw::Frame)start, (daw::Frame)len,
                                     ParseRollNotes(msg), ParseRollEvents(msg),
                                     (uint64_t)serial, false);
        if (action == RollSync::Close) PostMessage(B_QUIT_REQUESTED);
        return;
    }
    BWindow::MessageReceived(msg);
}

} // namespace daw
