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

class PianoRollView : public BView {
public:
    using Frame = daw::Frame;
    PianoRollView(BRect frame, TrackId track, ClipId clip,
                  std::vector<MidiNote> notes,
                  TempoMap tempo, double sampleRate, BMessenger apply);

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
    void  Apply();

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
    TempoMap   fTempo;
    double     fSampleRate;
    BMessenger fApply;

    double fFramesPerPixel = 128.0;
    Frame  fScrollFrame    = 0;
    int    fTopPitch       = 96;   // highest pitch row at the top

    enum class Drag { None, Move, Resize, Velocity, Marquee };
    Drag  fDrag = Drag::None;
    int   fDragNote = -1;
    Frame fGrabOffset = 0;
    int   fPitchOffset = 0;
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
    PianoRoll(BRect frame, TrackId track, ClipId clip,
              std::vector<MidiNote> notes,
              TempoMap tempo, double sampleRate, BMessenger apply);
private:
    PianoRollView* fView;
};

} // namespace daw
