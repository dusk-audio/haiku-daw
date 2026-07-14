// PianoRoll — a windowed piano-roll editor for one MIDI track.
//
// Custom-drawn (piano keyboard column + pitch x time note grid), on its own
// looper thread. Edits a local snapshot of the track's notes and posts the full
// updated list to the main window (kMsgApplyNotes), which owns model mutation.
// Add (click empty), move/resize (drag), delete (right-click), velocity
// (Ctrl-drag). Tempo-map-aware grid + snap.
#pragma once

#include "../model/Project.h"
#include "../model/TempoMap.h"
#include "../model/types.h"

#include <Messenger.h>
#include <View.h>
#include <Window.h>

#include <vector>

namespace daw {

// Applied to the model: int64 "track"; per note int32 "np","nv" + int64 "ns","nl".
constexpr uint32 kMsgApplyNotes = 'ntap';

class PianoRollView : public BView {
public:
    using Frame = daw::Frame;
    PianoRollView(BRect frame, TrackId track, std::vector<MidiNote> notes,
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

    std::vector<MidiNote> fNotes;
    TrackId    fTrack;
    TempoMap   fTempo;
    double     fSampleRate;
    BMessenger fApply;

    double fFramesPerPixel = 128.0;
    Frame  fScrollFrame    = 0;
    int    fTopPitch       = 96;   // highest pitch row at the top

    enum class Drag { None, Move, Resize, Velocity };
    Drag  fDrag = Drag::None;
    int   fDragNote = -1;
    Frame fGrabOffset = 0;
    int   fPitchOffset = 0;
};

class PianoRoll : public BWindow {
public:
    PianoRoll(BRect frame, TrackId track, std::vector<MidiNote> notes,
              TempoMap tempo, double sampleRate, BMessenger apply);
private:
    PianoRollView* fView;
};

} // namespace daw
