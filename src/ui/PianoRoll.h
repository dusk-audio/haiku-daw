// PianoRoll — a windowed piano-roll editor for one MIDI track.
//
// Custom-drawn (piano keyboard column + pitch x time note grid), on its own
// looper thread. Edits a local snapshot of the track's notes and posts the full
// updated list to the main window (kMsgApplyNotes), which owns model mutation.
// Add (click empty), move/resize (drag), delete (right-click), velocity
// (Ctrl-drag). Tempo-map-aware grid + snap. Multi-select: Command-click toggles
// a note, drag on empty space marquee-selects, and move/resize/velocity/delete
// act on the whole selection.
#pragma once

#include "../model/Project.h"
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

// MainWindow -> piano roll: current playhead (int64 "ph", absolute frames; a
// negative value hides it).
constexpr uint32 kMsgRollPlayhead = 'rlph';

// TimelineView -> MainWindow: a piano roll opened; carries BMessenger "m" so the
// main window can push the playhead to it during transport.
constexpr uint32 kMsgRollOpened = 'rlop';

class PianoRollView : public BView {
public:
    using Frame = daw::Frame;
    PianoRollView(BRect frame, TrackId track, ClipId clip, Frame clipStart,
                  std::vector<MidiNote> notes,
                  TempoMap tempo, double sampleRate, BMessenger apply);

    void SetPlayhead(Frame absFrame) { fPlayhead = absFrame; Invalidate(); }

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
    float VelLaneTop() const;           // y where the velocity lane begins
    int   VelNoteAtX(float x) const;    // nearest note to a velocity-lane click
    void  SetVelocityFromLane(float y); // set dragged/selected note velocity
    void  Apply();

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
    std::vector<char>     fSel;   // 1 = selected, parallel to fNotes
    TrackId    fTrack;
    ClipId     fClip;
    Frame      fClipStart = 0;    // region start (notes are relative to it)
    TempoMap   fTempo;
    double     fSampleRate;
    BMessenger fApply;

    double fFramesPerPixel = 128.0;
    Frame  fScrollFrame    = 0;
    int    fTopPitch       = 96;   // highest pitch row at the top
    Frame  fPlayhead       = -1;   // absolute frames; <0 = hidden ("tapehead")

    void ZoomBy(double factor);    // horizontal zoom about the view

    enum class Drag { None, Move, Resize, Velocity, Marquee, Brush, Erase };
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
              std::vector<MidiNote> notes,
              TempoMap tempo, double sampleRate, BMessenger apply);
    void MessageReceived(BMessage* msg) override;   // forwards kMsgRollPlayhead
    void DispatchMessage(BMessage* msg, BHandler* h) override;  // spacebar -> transport
private:
    PianoRollView* fView;
    BMessenger     fMain;   // to the main window (transport toggle)
};

} // namespace daw
