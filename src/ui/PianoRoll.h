// PianoRoll — a windowed piano-roll editor for one MIDI track.
//
// Custom-drawn (piano keyboard column + pitch x time note grid), on its own
// looper thread. Edits a local snapshot of the track's notes and posts the full
// updated list to the main window (kMsgApplyNotes), which owns model mutation.
// A tool palette across the top picks the click behaviour: Pointer selects and
// moves/resizes, Pencil/Brush create, Eraser/Scissors/Glue/Velocity act on the
// note under the cursor. Right-click deletes and Ctrl-drag sets velocity with
// any tool. Tempo-map-aware grid + snap (the grid the toolbar's field selects,
// model/SnapGrid.h). Multi-select: Alt-click (either Alt bit — a PC's Alt and a
// Mac's Command/Option share them) toggles a note, drag on empty space
// marquee-selects (Pointer, Alt from empty space keeps the prior selection),
// and move/resize/velocity/delete act on the whole selection. Cmd-X/C/V/D cut,
// copy, paste and duplicate; Alt-DRAG duplicates under the pointer (the same
// key, told apart by whether the pointer moves).
//
// The snapshot is SYNCHRONISED (package 51): the main window pushes the
// region's current state after every model change (kMsgRollRegion), and every
// edit this view posts carries the stack serial it was based on. A commit whose
// base is stale is refused by the main window rather than written on top of a
// state the view never read — the bug was a stale list silently replacing an
// undo, and a region that moved under the open roll. The rules themselves are
// kit-free in model/RollEdit.h.
#pragma once


#include "Theme.h"   // ThemeAware (T1)
#include "../model/Project.h"
#include "../model/MidiOps.h"
#include "../model/RollEdit.h"
#include "../model/SnapGrid.h"
#include "../model/TempoMap.h"
#include "../model/types.h"

#include <Messenger.h>
#include <View.h>
#include <Window.h>

#include <vector>

namespace daw {

// Applied to the model: int64 "track" + int64 "clip"; per note int32 "np","nv"
// + int64 "ns","nl" (note frames are clip-relative), plus int64 "base" — the
// CommandStack serial the view's snapshot was based on (RollEdit.h:
// RollCommitCurrent). The main window applies the edit only while "base" is
// still the stack's serial, and otherwise pushes a fresh region instead.
constexpr uint32 kMsgApplyNotes = 'ntap';

// The CC lane's edited controller list: int64 "track" + int64 "clip"; per event
// int32 "et" (type), int32 "ed" (CC number), int32 "ev" (value) + int64 "es"
// (clip-relative frame). Separate from kMsgApplyNotes so a controller edit never
// rewrites notes and vice versa. Same int64 "base".
constexpr uint32 kMsgApplyEvents = 'evap';

// Applied to the model: one region's note list after a named transform
// (quantize / humanize / legato / transpose / velocity) ran in the roll --
// int64 "track" + int64 "clip", int32 "op" (MidiOp), then the same per-note
// shape as kMsgApplyNotes. The RESULT travels, not the parameters: MidiOps.h
// says why. Same int64 "base".
constexpr uint32 kMsgApplyMidiOp = 'mopz';

// MainWindow -> piano roll: current playhead (int64 "ph", absolute frames; a
// negative value hides it).
constexpr uint32 kMsgRollPlayhead = 'rlph';

// MainWindow -> piano roll: the region's current state, so the view stops
// showing a snapshot the model no longer has (package 51). Fields: int64
// "track", "clip", "start", "len", "serial", bool "gone" (the region is gone),
// and the note/event arrays in the same shape the apply messages use. Sent to
// the docked view and the popped-out window; the view decides what to do with
// it through RollEdit.h's DecideRollSync.
constexpr uint32 kMsgRollRegion = 'rlrg';

// MainWindow -> piano roll: the follow-playhead policy (bool "on"), so the View
// menu's one mark drives the timeline and every open roll (package 51).
constexpr uint32 kMsgRollFollow = 'rlfw';

// PianoRoll -> MainWindow: audition a note through the region track's
// instrument (package 51). int64 "track", int32 "pitch", "vel", bool "on". The
// window owns the engine side (and the preference that mutes it).
constexpr uint32 kMsgRollAudition = 'rlau';

// TimelineView -> MainWindow: a piano roll opened; carries BMessenger "m" so the
// main window can push the playhead to it during transport.
constexpr uint32 kMsgRollOpened = 'rlop';

// The keys the EDITOR handles itself: its tool palette (1-7), quantize (q),
// the grid menu (g), Fit (f), the zoom pair, the nudge arrows and Del. They are
// deliberately not in the arrange's one key map (model/KeyMap.h says why -- the
// roll draws its own keyboard), but the DOCKED view is a view of the main
// window, so the window has to know which keys to hand it while it holds the
// focus: without this the arrange's own bindings (the palette, the grid menu,
// the pan arrows, Delete) would take them, and the editor's tools would be
// dead while docked.
inline bool RollEditorKey(char c) {
    switch (c) {
        case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        case 'q': case 'Q':
        case 'g': case 'G':
        case 'f': case 'F':
        case '+': case '=': case '-': case '_':
        case B_LEFT_ARROW: case B_RIGHT_ARROW:
        case B_UP_ARROW: case B_DOWN_ARROW:
        case B_DELETE: case B_BACKSPACE:
            return true;
        default:
            return false;
    }
}

// --- the payload codecs ----------------------------------------------------
//
// MainWindow and the roll both speak the note/event array shapes above, so the
// encoders and decoders live here rather than on either side: one definition of
// the wire shape, next to the message ids it belongs to.

inline void AddRollNotes(BMessage* m, const std::vector<MidiNote>& notes) {
    for (const MidiNote& n : notes) {
        m->AddInt32("np", n.pitch);
        m->AddInt32("nv", n.velocity);
        m->AddInt64("ns", (int64)n.startFrame);
        m->AddInt64("nl", (int64)n.lengthFrames);
    }
}

inline std::vector<MidiNote> ParseRollNotes(const BMessage* msg) {
    std::vector<MidiNote> notes;
    int32 pitch = 0;
    for (int32 i = 0; msg->FindInt32("np", i, &pitch) == B_OK; i++) {
        MidiNote n;
        int32 vel = 100; int64 st = 0, len = 1;
        msg->FindInt32("nv", i, &vel);
        msg->FindInt64("ns", i, &st);
        msg->FindInt64("nl", i, &len);
        n.pitch = pitch; n.velocity = vel;
        n.startFrame = (Frame)st; n.lengthFrames = (Frame)len;
        notes.push_back(n);
    }
    return notes;
}

inline void AddRollEvents(BMessage* m, const std::vector<MidiClipEvent>& events) {
    for (const MidiClipEvent& e : events) {
        m->AddInt32("et", (int32)e.type);
        m->AddInt32("ed", (int32)e.data);
        m->AddInt32("ev", (int32)e.value);
        m->AddInt64("es", (int64)e.startFrame);
    }
}

// The same validation the apply path uses: every field present and in range, or
// the event is dropped — a partial event would land as a CC0 at frame 0, which
// no lane draws yet the project would save.
inline std::vector<MidiClipEvent> ParseRollEvents(const BMessage* msg) {
    std::vector<MidiClipEvent> events;
    int32 type = 0;
    for (int32 i = 0; msg->FindInt32("et", i, &type) == B_OK; i++) {
        int32 data = 0, val = 0; int64 st = 0;
        if (msg->FindInt32("ed", i, &data) != B_OK) continue;
        if (msg->FindInt32("ev", i, &val)  != B_OK) continue;
        if (msg->FindInt64("es", i, &st)   != B_OK) continue;
        if (type < MidiClipEvent::CC || type > MidiClipEvent::ChannelPressure)
            continue;
        if (data < 0 || data > 127) continue;
        const int32 vmax = (type == MidiClipEvent::PitchBend) ? 16383 : 127;
        if (val < 0 || val > vmax) continue;
        if (st < 0) continue;      // clip-relative; negative is junk
        MidiClipEvent e;
        e.type = type; e.data = data; e.value = val; e.startFrame = (Frame)st;
        events.push_back(e);
    }
    return events;
}

// The audition switch (see kMsgRollAudition).
inline void AddRollAudition(BMessage* m, TrackId track, int pitch, int velocity,
                            bool on) {
    m->AddInt64("track", (int64)track);
    m->AddInt32("pitch", pitch);
    m->AddInt32("vel", velocity);
    m->AddBool("on", on);
}

class PianoRollView : public BView, public ThemeAware {
public:
    void ApplyTheme() override;   // T1: the cached panel colour
public:
    using Frame = daw::Frame;
    // `playhead` is the transport position when the editor opened (absolute
    // frames, <0 = none): the view opens scrolled to it when it falls inside the
    // region, so double-clicking a region under the tapehead lands you there.
    PianoRollView(BRect frame, TrackId track, ClipId clip, Frame clipStart,
                  Frame clipLength, std::vector<MidiNote> notes,
                  std::vector<MidiClipEvent> events,
                  TempoMap tempo, double sampleRate, Frame playhead,
                  BMessenger apply);

    void SetPlayhead(Frame absFrame);   // M2.2: follows it when fFollow
    // The follow policy (package 51, item C). It used to be a hard-coded true
    // with no setter, so the View menu's toggle could not turn it off here.
    void SetFollow(bool on) { fFollow = on; }
    bool Follow() const { return fFollow; }
    // Visible-span introspection for the functional tests, and the follow
    // policy's own state (region-relative scroll in frames).
    Frame  ScrollFrame() const { return fScrollFrame; }
    Frame  VisibleSpan() const;   // frames the grid shows, right of the keyboard
    double FramesPerPixel() const { return fFramesPerPixel; }

    // The model ⇄ snapshot sync (package 51, items A/B). The main window calls
    // this with the region as the model has it NOW; the view re-reads, re-points
    // or (with `gone`) reports that it must close. Returns what it did
    // (RollEdit.h's RollSync) so a test can assert the decision, and `serial` is
    // the base every later commit carries.
    RollSync SyncRegion(TrackId track, ClipId clip, Frame start, Frame length,
                        std::vector<MidiNote> notes,
                        std::vector<MidiClipEvent> events,
                        uint64_t serial, bool gone);
    uint64_t SyncSerial() const { return fSerial; }
    // Whether a region push has ever reached this view. A view the main window
    // hosts is pushed the moment it opens, so it always is; a view built by
    // something else (a test harness) keeps committing the way it always did —
    // with no base, and nothing to gate against.
    bool Synced() const { return fSynced; }
    Frame RegionStart() const { return fClipStart; }
    Frame RegionLength() const { return fClipLen; }

    // Which lane the bottom strip shows (package 51, item G): -1 = note
    // velocity, -2 = pitch bend, else a CC number 0..127. The lane's own popup
    // is where a person picks it, and this is what each menu item calls —
    // public so a test can drive it without opening a popup, exactly like
    // RunMidiOp below.
    void SetLane(int lane);
    void SetLaneCc(int cc) { SetLane(cc); }   // the old CC-only spelling
    int  Lane() const { return fLane; }
    int  LaneCc() const { return fLane; }
    const char* LaneLabel() const { return fLaneLabel; }
    // Clear the lane's stream as one undoable step (the menu's "Clear lane").
    // Public for the same reason SetLane is. Returns how many points went.
    int  ClearLane();

    // The snap grid (package 51, item E): the same SnapGrid the arrangement
    // uses, defaulting to the 1/16 the roll was welded to. Public so a test can
    // set it without opening the toolbar's popup.
    void     SetSnapGrid(SnapGrid g) { fSnap = g; Invalidate(); }
    SnapGrid Snap() const { return fSnap; }
    void     OpenGridMenu();

    // --- editing feel (package 51, item H) ---------------------------------
    // Cmd-C/Cmd-V/Cmd-D and Alt-drag. The clipboard is the view's own (the
    // arrangement's clipboard is a different kind of thing: clips, not notes).
    // Public because the main window routes its Edit-menu shortcuts here while
    // this view holds the focus (the docked roll is a view of that window, so
    // the menu bar claims the keys before any view sees them).
    void CopySelection();
    void PasteClipboard();          // at the playhead, else at the region head
    void DuplicateSelection();      // in place, offset one grid step
    void DeleteSelection();         // Edit > Cut's second half, and Del
    void SelectAll();
    void ZoomToFitRegion();         // 'f': the whole region in view
    bool ClipboardEmpty() const { return fClipboard.empty(); }
    int  SelectedCount() const;
    bool HasSelection() const { return SelectedCount() > 0; }

    // --- audition (package 51, item D) -------------------------------------
    // Tell the main window to sound (or stop) a note through this region's
    // track instrument. The window decides whether anything comes out (a
    // preference can mute it, and an idle transport raises the monitor-only
    // engine for the clicks). Public because the window HOST calls AuditionEnd
    // to release a key the pointer may still be holding when the editor goes.
    void Audition(int pitch, int velocity, bool on);
    void AuditionEnd();   // release whatever this view is holding
    void AuditionBlip(int pitch, int velocity);   // a short one (on + off)

    // --- MIDI transforms ---------------------------------------------------
    // The transform runs here, on the snapshot, and the result is posted as one
    // undoable step (kMsgApplyMidiOp). `param` is the semitone count for
    // Transpose and the velocity delta for Velocity; the others ignore it.
    //
    // Public because it is the entry point each menu item calls, and the menu
    // is a popup -- a test (tests/ui_functional_tests.cpp) cannot open one, so
    // this is how each transform's commit path gets driven on the target.
    void  RunMidiOp(MidiOp op, int param = 0);

    // Snapshot introspection for the functional tests (package 51): the
    // stale-list bug is invisible from the model alone, so a test has to be able
    // to read what the ROLL shows.
    int      NoteCount() const { return (int)fNotes.size(); }
    MidiNote NoteForTest(int i) const { return fNotes[(size_t)i]; }
    int      EventCount() const { return (int)fEvents.size(); }
    int      SelectedCountForTest() const { return SelectedCount(); }
    // The geometry a test needs to aim a synthetic click: the frame under an x,
    // the x a frame is drawn at, and the y a lane value sits at. Same functions
    // the drawing and the hit tests use, so a click lands where a person's
    // would.
    Frame FrameAtXForTest(float x) const { return XToFrame(x); }
    float XForFrameForTest(Frame f) const { return FrameToX(f); }
    float LaneValueYForTest(int value) const { return CcValueToY(value); }

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    void MouseUp(BPoint where) override;
    void MessageReceived(BMessage* msg) override;   // wheel + the window's pushes
    void KeyDown(const char* bytes, int32 numBytes) override;

private:
    float FrameToX(Frame f) const;
    Frame XToFrame(float x) const;
    float PitchToY(int pitch) const;
    int   YToPitch(float y) const;
    Frame Snapped(Frame f) const;
    int   NoteAt(BPoint where) const;   // -1 none
    float VelLaneTop() const;           // y where the bottom lane begins
    void  DrawKeyboard(float velTop);   // the piano column
    int   VelNoteAtX(float x) const;    // nearest note to a velocity-lane click
    void  SetVelocityFromLane(float y); // set dragged/selected note velocity
    void  Apply();
    // Which end a note hit grabbed: the left edge resizes the start, the right
    // edge the end, the middle moves (package 51, item H).
    enum class Zone { Middle, LeftEdge, RightEdge };
    Zone  ZoneOf(const MidiNote& n, BPoint where) const;

    // --- theme + layout (package 51, item J) -------------------------------
    // Every metric and rect the view draws with, in device pixels: the design
    // sizes go through Themed() so the roll scales with the app like every
    // other window, and the toolbar is laid out from the view's own width so
    // the MIDI button stays reachable in a narrow dock.
    float KbdW() const;         // the piano column's width
    float RowH() const;         // pixels per semitone
    float LaneH() const;        // the bottom lane's height
    float ToolbarH() const;     // the tool strip's height
    BRect ToolRect(int i) const;
    BRect GridRect() const;
    BRect ZoomOutRect() const;
    BRect ZoomInRect() const;
    BRect MidiRect() const;
    BRect LanePickRect() const;
    void  LayoutToolbar(RollToolbarLayout* out) const;   // in device pixels
    int   ToolAt(BPoint where) const;   // -1 if not on a toolbar button
    // Toolbar hit test for the non-tool buttons: 0 grid, 1 zoom out, 2 zoom in,
    // 3 MIDI, -1 none. One switch for the click, the hover highlight and the
    // tooltip, so they cannot disagree about where a button is.
    int   ToolbarButtonAt(BPoint where) const;

    // --- the pointer's own feedback (item H) -------------------------------
    // The roll had no BCursor at all: a 5 px resize zone nobody can see. Same
    // ids the arrangement uses for the same gestures.
    enum { kCursorDefault = 0, kCursorDrag, kCursorTrim, kCursorDraw };
    void ApplyCursor(int which);

    // --- MIDI transforms ---------------------------------------------------

    // --- MIDI transforms (implementation) ----------------------------------
    void  ApplyMidiOp(MidiOp op);       // post the (already transformed) list
    void  MidiMenu();                   // the toolbar button's popup
    void  OpenQuantizeWindow();         // settings, remembered in fQuant

    // --- bottom lane ------------------------------------------------------
    // The strip under the grid shows note velocity, pitch bend, or ONE
    // controller. The controllers the voices actually act on today are CC1
    // (mod-wheel vibrato), CC7 x CC11 (channel gain), CC10 (pan) and CC64
    // (sustain) — model/MidiControl.h's CcVoiceEffect is the one table the
    // lane's menu labels come from, so the menu cannot advertise an effect the
    // engine does not have. The rest are recorded, drawn and saved (and
    // exported to SMF) without an audible effect of their own.
    int   fLane = kLaneVelocity;
    char  fLaneLabel[16] = "Vel";       // the lane button's short caption
    int   fHoverTool = -1;              // the tool button under the cursor
    int   fHoverBtn = -1;               // the toolbar button under the cursor
    // Fills `buf` with that caption: "Vel"/"Bend", or an abbreviation for a
    // controller people know by one ("Mod", "Vol", "Pan", "Expr", "Sus"), else
    // "CC n". Static + buffer-taking so it is safe to call while another label
    // lives.
    static const char* LaneLabelFor(int lane, char* buf, size_t n);
    void  LaneMenu();                   // the lane button's popup
    // CC-lane geometry + editing.
    float CcValueToY(int value) const;  // a lane value -> y inside the lane
    int   CcYToValue(float y) const;    // inverse, clamped to the lane's range
    int   CcEventAtX(float x) const;    // index into fEvents, -1 if none near
    void  SetCcAt(BPoint where);        // ramp path: a point at the click
    void  MoveCcDrag(BPoint where);     // drag the grabbed point (item F)
    void  EraseCcAt(BPoint where);      // right-click delete
    void  ApplyEvents();
    void  DrawCcLane(BRect lane);
    void  DrawRegionBounds(float velTop, float w);   // item I

    // Editing tools (a toolbar across the top selects the active one).
    enum class Tool { Pointer, Pencil, Brush, Eraser, Scissors, Glue, Velocity };
    Tool  fTool = Tool::Pointer;
    void  AddNoteAt(BPoint where, bool resizeDrag);   // pencil / brush
    void  EraseAt(BPoint where);        // eraser
    void  SplitNoteAt(int note, float x);   // scissors
    void  GlueNoteAt(int note);         // glue (merge next same-pitch)
    int   PaintBrush(BPoint where);     // brush: add along the drag path (count)

    // Selection helpers (fSel is index-aligned with fNotes).
    void  SelectOnly(int i);
    void  ClearSelection();
    void  DeleteSelected();
    void  CaptureDragOrigin();
    // Did the gesture in progress actually change anything? A bare click on a
    // note starts a Move drag that goes nowhere, and committing it pushed an
    // "Edit Notes" undo step that undid to the same notes (the stack has no
    // no-op detection).
    bool  DragChanged() const;

    std::vector<MidiNote> fNotes;
    std::vector<MidiClipEvent> fEvents;   // the region's controller events
    std::vector<MidiNote> fClipboard;     // the view's own note clipboard (H)
    std::vector<char>     fSel;   // 1 = selected, parallel to fNotes
    TrackId    fTrack;
    ClipId     fClip;
    Frame      fClipStart = 0;    // region start (notes are relative to it)
    Frame      fClipLen   = 0;    // region length (the window transforms respect)
    TempoMap   fTempo;
    double     fSampleRate;
    BMessenger fApply;
    // The CommandStack serial the snapshot was last synchronised with; every
    // commit carries it (RollEdit.h: RollCommitCurrent). `fSynced` says whether
    // a push has ever arrived (see Synced()).
    uint64_t   fSerial = 0;
    bool       fSynced = false;

    // Quantize settings, remembered for the session ("last used"): the MIDI
    // menu's plain Quantize and the 'q' key both run these.
    QuantizeOpts fQuant;

    // The grid the toolbar's field selects (item E). 1/16 by default: exactly
    // what the hard-coded kSnapDiv = 4 did.
    SnapGrid fSnap{ SnapKind::Sixteenth, false };

    double fFramesPerPixel = 128.0;
    Frame  fScrollFrame    = 0;
    bool   fFollow         = true;  // M2.2: scroll to keep the playhead in view
    int    fTopPitch       = 96;   // highest pitch row at the top
    Frame  fPlayhead       = -1;   // absolute frames; <0 = hidden ("tapehead")

    void ZoomBy(double factor);    // horizontal zoom about the view
    void ZoomAnchoredAt(double factor, float x);   // keeps x's frame in place
    BPoint fHoverPos{ -1.0f, -1.0f };   // last pointer position (wheel anchor)
    bool   fHoverSeen = false;

    enum class Drag { None, Move, Resize, ResizeLeft, Velocity, Marquee, Brush,
                      Erase, Cc, CcMove,
                      // Alt pressed on a note: it becomes a duplicate-and-drag
                      // only once the pointer MOVES; a click stays the additive
                      // selection toggle it has always been (the two gestures
                      // share one key on a PC keyboard, where Alt is
                      // B_COMMAND_KEY).
                      AltPending };
    Drag  fDrag = Drag::None;
    int   fDragNote = -1;
    Frame fGrabOffset = 0;
    int   fPitchOffset = 0;
    bool  fVelLaneDrag = false;    // velocity drag started in the bottom lane
    BPoint fDownPoint;             // where the current drag began
    int   fCcDragIndex = -1;       // the CC point a drag holds (its identity)
    // The note the keyboard column is holding down (its audition), or -1.
    int   fAuditionPitch = -1;
    int   fCursorShown = -1;       // the cursor currently set (skip a redundant one)

    // Per-note snapshot captured at drag start so a group move/resize applies
    // one consistent delta to every selected note (no cumulative drift).
    std::vector<NoteGeometry> fDragOrig;   // index-aligned with fNotes
    size_t fDragCount0 = 0;    // how many notes there were when the drag began
    bool   fDragMustCommit = false;   // a note was CREATED: commit even if the
                                      // gesture left its geometry alone

    // Marquee (rubber-band) selection.
    BPoint            fMarqueeCur;
    std::vector<char> fPreMarquee;   // selection before an additive marquee
};

class PianoRoll : public BWindow {
public:
    PianoRoll(BRect frame, TrackId track, ClipId clip, daw::Frame clipStart,
              daw::Frame clipLength, std::vector<MidiNote> notes,
              std::vector<MidiClipEvent> events,
              TempoMap tempo, double sampleRate, daw::Frame playhead,
              BMessenger apply, const char* trackName = nullptr);
    // A key held on the keyboard column when the window goes: its note-off
    // would never come, and the audition engine would keep waiting for it.
    ~PianoRoll() override { if (fView) fView->AuditionEnd(); }
    void MessageReceived(BMessage* msg) override;   // the pushes from MainWindow
    void DispatchMessage(BMessage* msg, BHandler* h) override;  // Cmd-Z, space
    PianoRollView* RollView() const { return fView; }
private:
    PianoRollView* fView;
    BMessenger     fMain;   // to the main window (transport toggle)
};

} // namespace daw
