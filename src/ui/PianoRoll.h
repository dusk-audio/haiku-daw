// PianoRoll — a windowed piano-roll editor for one MIDI track.
//
// Custom-drawn (piano keyboard column + pitch x time note grid), on its own
// looper thread. Edits a local snapshot of the track's notes and posts the full
// updated list to the main window (kMsgApplyNotes), which owns model mutation.
// A tool palette across the top picks the click behaviour: Pointer selects and
// moves/resizes, Pencil/Brush create, Eraser/Scissors/Glue/Velocity act on the
// note under the cursor. Right-click deletes and Ctrl-drag sets velocity with
// any tool. Tempo-map-aware grid + snap. Multi-select: Command-click toggles a
// note, drag on empty space marquee-selects (Pointer), and move/resize/velocity/
// delete act on the whole selection.
#pragma once

#include "../model/Project.h"
#include "../model/MidiOps.h"
#include "../model/TempoMap.h"
#include "../model/types.h"

#include <Messenger.h>
#include <View.h>
#include <Window.h>

#include <vector>

namespace daw {

// Applied to the model: int64 "track" + int64 "clip"; per note int32 "np","nv"
// + int64 "ns","nl" (note frames are clip-relative).
constexpr uint32 kMsgApplyNotes = 'ntap';

// The CC lane's edited controller list: int64 "track" + int64 "clip"; per event
// int32 "et" (type), int32 "ed" (CC number), int32 "ev" (value) + int64 "es"
// (clip-relative frame). Separate from kMsgApplyNotes so a controller edit never
// rewrites notes and vice versa.
constexpr uint32 kMsgApplyEvents = 'evap';

// Applied to the model: one region's note list after a named transform
// (quantize / humanize / legato / transpose / velocity) ran in the roll --
// int64 "track" + int64 "clip", int32 "op" (MidiOp), then the same per-note
// shape as kMsgApplyNotes (int32 "np","nv" + int64 "ns","nl"). The RESULT
// travels, not the parameters: MidiOps.h says why.
constexpr uint32 kMsgApplyMidiOp = 'mopz';

// MainWindow -> piano roll: current playhead (int64 "ph", absolute frames; a
// negative value hides it).
constexpr uint32 kMsgRollPlayhead = 'rlph';

// TimelineView -> MainWindow: a piano roll opened; carries BMessenger "m" so the
// main window can push the playhead to it during transport.
constexpr uint32 kMsgRollOpened = 'rlop';

class PianoRollView : public BView {
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

    void SetPlayhead(Frame absFrame) { fPlayhead = absFrame; Invalidate(); }

    // Which controller the bottom lane shows: -1 = note velocity, else a CC
    // number 0..127. The lane's own popup is where a person picks it (the menu
    // lists the controllers this region already has, then all 128), and this is
    // what each menu item calls — public so a test can drive it without opening
    // a popup, exactly like RunMidiOp below.
    void SetLaneCc(int cc);
    int  LaneCc() const { return fLaneCc; }

    // --- MIDI transforms ---------------------------------------------------
    // The transform runs here, on the snapshot, and the result is posted as one
    // undoable step (kMsgApplyMidiOp). `param` is the semitone count for
    // Transpose and the velocity delta for Velocity; the others ignore it.
    //
    // Public because it is the entry point each menu item calls, and the menu
    // is a popup -- a test (tests/ui_functional_tests.cpp) cannot open one, so
    // this is how each transform's commit path gets driven on the target.
    void  RunMidiOp(MidiOp op, int param = 0);

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    void MouseUp(BPoint where) override;
    void MessageReceived(BMessage* msg) override;   // wheel scroll
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

    // --- MIDI transforms ---------------------------------------------------

    // --- MIDI transforms (implementation) ----------------------------------
    void  ApplyMidiOp(MidiOp op);       // post the (already transformed) list
    void  MidiMenu();                   // the toolbar button's popup
    void  OpenQuantizeWindow();         // settings, remembered in fQuant

    // --- bottom lane ------------------------------------------------------
    // The strip under the grid shows either note velocity or ONE controller.
    // Any of the 128 can be shown (the lane button opens a menu of them), so a
    // recorded pedal or a synth's CC74 is editable here; the controllers the
    // voices actually act on today are CC1 (mod-wheel vibrato), CC7 x CC11
    // (channel gain), CC10 (pan) and CC64 (sustain) — see model/MidiControl.h,
    // model/MidiExpression.h and model/Sustain.h. The rest are recorded, drawn
    // and saved (and exported to SMF) without an audible effect of their own.
    int   fLaneCc = -1;                 // -1 = velocity
    char  fLaneLabel[16] = "Vel";       // the lane button's short caption
    int   fHoverTool = -1;              // the tool button under the cursor
    // Fills `buf` with that caption: "Vel", or an abbreviation for a controller
    // people know by one ("Mod", "Vol", "Pan", "Expr", "Sus"), else "CC n".
    // Static + buffer-taking so it is safe to call while another label lives.
    static const char* LaneLabelFor(int cc, char* buf, size_t n);
    void  LaneMenu();                   // the lane button's popup
    BRect LanePickRect() const;         // the lane selector button
    // CC-lane geometry + editing.
    float CcValueToY(int value) const;  // 0..127 -> y inside the lane
    int   CcYToValue(float y) const;    // inverse, clamped to 0..127
    int   CcEventAtX(float x) const;    // index into fEvents, -1 if none near
    void  SetCcAt(BPoint where);        // add/replace a point at the click
    void  EraseCcAt(BPoint where);      // right-click delete
    void  ApplyEvents();
    void  DrawCcLane(BRect lane);

    // Editing tools (a toolbar across the top selects the active one).
    enum class Tool { Pointer, Pencil, Brush, Eraser, Scissors, Glue, Velocity };
    Tool  fTool = Tool::Pointer;
    BRect ToolRect(int i) const;        // toolbar button rect
    int   ToolAt(BPoint where) const;   // -1 if not on a toolbar button
    void  AddNoteAt(BPoint where, bool resizeDrag);   // pencil / brush
    void  EraseAt(BPoint where);        // eraser
    void  SplitNoteAt(int note, float x);   // scissors
    void  GlueNoteAt(int note);         // glue (merge next same-pitch)
    void  PaintBrush(BPoint where);     // brush: add along the drag path

    // Selection helpers (fSel is index-aligned with fNotes).
    void  SelectOnly(int i);
    void  ClearSelection();
    int   SelectedCount() const;
    void  DeleteSelected();
    void  CaptureDragOrigin();

    std::vector<MidiNote> fNotes;
    std::vector<MidiClipEvent> fEvents;   // the region's controller events
    std::vector<char>     fSel;   // 1 = selected, parallel to fNotes
    TrackId    fTrack;
    ClipId     fClip;
    Frame      fClipStart = 0;    // region start (notes are relative to it)
    Frame      fClipLen   = 0;    // region length (the window transforms respect)
    TempoMap   fTempo;
    double     fSampleRate;
    BMessenger fApply;

    // Quantize settings, remembered for the session ("last used"): the MIDI
    // menu's plain Quantize and the 'q' key both run these.
    QuantizeOpts fQuant;

    double fFramesPerPixel = 128.0;
    Frame  fScrollFrame    = 0;
    int    fTopPitch       = 96;   // highest pitch row at the top
    Frame  fPlayhead       = -1;   // absolute frames; <0 = hidden ("tapehead")

    void ZoomBy(double factor);    // horizontal zoom about the view

    enum class Drag { None, Move, Resize, Velocity, Marquee, Brush, Erase, Cc };
    Drag  fDrag = Drag::None;
    int   fDragNote = -1;
    Frame fGrabOffset = 0;
    int   fPitchOffset = 0;
    bool  fVelLaneDrag = false;    // velocity drag started in the bottom lane
    BPoint fDownPoint;             // where the current drag began

    // Per-note snapshot captured at drag start so a group move/resize applies
    // one consistent delta to every selected note (no cumulative drift).
    struct Orig { Frame start; Frame len; int pitch; int velocity; };
    std::vector<Orig> fDragOrig;   // index-aligned with fNotes

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
              BMessenger apply);
    void MessageReceived(BMessage* msg) override;   // forwards kMsgRollPlayhead
    void DispatchMessage(BMessage* msg, BHandler* h) override;  // spacebar -> transport
private:
    PianoRollView* fView;
    BMessenger     fMain;   // to the main window (transport toggle)
};

} // namespace daw
