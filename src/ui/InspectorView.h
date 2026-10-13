// InspectorView — a Logic-style track inspector column on the left of the
// window. Shows the full control set for the currently selected track (name,
// mute/solo/arm, input, monitor, output routing, sends, effects, instrument,
// pan knob, and a channel fader), so the track lane itself can stay slim.
//
// Custom-drawn (no BControls), like the timeline header. Edits go through the
// same CommandStack; it opens the effects/sends/instrument editors directly and
// posts kMsgUiRefresh so the timeline stays in sync. Haiku-only.
#pragma once


#include "Theme.h"   // ThemeAware (T1)
#include "../model/Project.h"
#include "../model/Command.h"

#include <View.h>

namespace daw {

class InspectorView : public BView, public ThemeAware {
public:
    void ApplyTheme() override;   // T1: the cached panel colour

    using Frame = daw::Frame;

    InspectorView(BRect frame, Project* project, CommandStack* stack);

    void SetTrack(TrackId id) { fTrack = id; Invalidate(); }
    TrackId SelectedTrack() const { return fTrack; }
    void SetMeter(float l, float r);   // peak-fed, with a hold + clip latch per channel

    // The input and output pickers, for the track named (which need not be the
    // selected one): `screenAt` is where the popup opens, in screen coordinates.
    // Public because the MIXER's strip buttons offer the same two choices
    // (kMsgMixInput / kMsgMixOutput route here): one picker, one list and one
    // command path, so a strip and the inspector cannot disagree about what a
    // track's input or output may be.
    void PickInput(TrackId id, BPoint screenAt);
    void PickOutput(TrackId id, BPoint screenAt);

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 code, const BMessage* drag) override;
    void MouseUp(BPoint where) override;

private:
    const Track* CurrentTrack() const;
    Track*       CurrentTrackMut() const;
    void Layout();                    // recompute control rects for the width
    void Refresh();                   // repaint + tell the window to refresh
    void RefreshFx();                 // ... and push the chain into the engine
    void MessageReceived(BMessage* msg) override;   // plugin browser's choice

    Project*      fProject;
    CommandStack* fStack;
    TrackId       fTrack = kInvalidTrackId;

    // Control rects (recomputed in Layout()).
    BRect fMuteR, fSoloR, fArmR, fMonR, fInputR, fOutR, fSendsR, fFxR, fInstR;
    BRect fGroupR;              // mute-group assignment
    // The insert-slot list block, and the row rect for one slot inside it.
    // Layout(), Draw() and MouseDown() all derive rows from FxRowRect so they
    // cannot disagree about where a row is.
    BRect fFxSlotsR;
    int   fFxRows = 0;          // rows drawn, including the trailing empty slot
    BRect FxRowRect(int i) const;
    BRect fAutoR, fPanR, fFaderR, fMeterR;
    // The meter block (package 52): a clip lamp over a stereo bar PAIR, so the
    // inspector's meter reads like the mixer strips'. fMeterR is the whole
    // block's hit box (a click there clears the latches).
    BRect fLampR, fMeterLR, fMeterRR;
    float fPeakL = 0.0f, fPeakR = 0.0f;   // selected track output level
    // Peak hold + clip latch, per channel: the same two state machines the
    // master meter runs (M3.3), so a hold falls at the same rate here.
    PeakHold  fHoldL, fHoldR;
    ClipLatch fClipL, fClipR;

    // Fader / pan drag (preview by writing the model, commit one command on up).
    enum class Drag { None, Fader, Pan, FxSlot };
    int   fDragFxFrom = -1;     // insert being dragged, and where it would land
    int   fDragFxTo   = -1;
    // The track the drag started on. Row indices mean nothing without it: if the
    // selection changed mid-drag they would address a DIFFERENT chain, and the
    // reorder would silently scramble the wrong track.
    TrackId fDragFxTrack = kInvalidTrackId;
    Drag  fDrag = Drag::None;
    float fDragOrig  = 0.0f;   // gain/pan at drag start (for undo)
    float fDragStartY = 0.0f;  // cursor y at drag start (pan knob = vertical)
};

} // namespace daw
