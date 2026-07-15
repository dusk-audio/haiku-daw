// InspectorView — a Logic-style track inspector column on the left of the
// window. Shows the full control set for the currently selected track (name,
// mute/solo/arm, input, monitor, output routing, sends, effects, instrument,
// pan knob, and a channel fader), so the track lane itself can stay slim.
//
// Custom-drawn (no BControls), like the timeline header. Edits go through the
// same CommandStack; it opens the effects/sends/instrument editors directly and
// posts kMsgUiRefresh so the timeline stays in sync. Haiku-only.
#pragma once

#include "../model/Project.h"
#include "../model/Command.h"

#include <View.h>

namespace daw {

class InspectorView : public BView {
public:
    using Frame = daw::Frame;

    InspectorView(BRect frame, Project* project, CommandStack* stack);

    void SetTrack(TrackId id) { fTrack = id; Invalidate(); }
    TrackId SelectedTrack() const { return fTrack; }
    void SetMeter(float l, float r) { fPeakL = l; fPeakR = r; Invalidate(fMeterR); }

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 code, const BMessage* drag) override;
    void MouseUp(BPoint where) override;

private:
    const Track* CurrentTrack() const;
    Track*       CurrentTrackMut() const;
    void Layout();                    // recompute control rects for the width
    void Refresh();                   // repaint + tell the window to refresh

    Project*      fProject;
    CommandStack* fStack;
    TrackId       fTrack = kInvalidTrackId;

    // Control rects (recomputed in Layout()).
    BRect fMuteR, fSoloR, fArmR, fMonR, fInputR, fOutR, fSendsR, fFxR, fInstR;
    BRect fAutoR, fPanR, fFaderR, fMeterR;
    float fPeakL = 0.0f, fPeakR = 0.0f;   // selected track output level

    // Fader / pan drag (preview by writing the model, commit one command on up).
    enum class Drag { None, Fader, Pan };
    Drag  fDrag = Drag::None;
    float fDragOrig = 0.0f;   // gain/pan at drag start (for undo)
};

} // namespace daw
