// EffectsWindow — custom-drawn editor for one track's effect chain.
//
// Separate looper thread (like the mixer): holds a local snapshot of the chain,
// edits it on mouse input, and posts the whole updated chain to the main window
// (kMsgApplyFx). Fully custom-drawn (dark knobs, EQ response graph, compressor
// transfer curve) so it matches the timeline/mixer theme instead of light OS
// sliders.
#pragma once

#include "../model/Effect.h"
#include "../model/types.h"

#include <Messenger.h>
#include <ScrollView.h>
#include <View.h>
#include <Window.h>

#include <vector>

namespace daw {

// Fields: int64 "track"; per effect int32 "et" (type), int32 "ec" (param count),
// float[] "ep" (all params concatenated).
constexpr uint32 kMsgApplyFx = 'fxap';
// Right-click a knob to toggle automation of that param. Fields: int64 "track",
// int32 "fx" (effect index), int32 "slot", float "val" (current value).
constexpr uint32 kMsgToggleFxAuto = 'fxat';

class EffectsView : public BView {
public:
    EffectsView(BRect frame, std::vector<EffectDesc> chain, TrackId track,
                BMessenger apply);

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    void MouseUp(BPoint where) override;

    float ContentHeight() const;    // total stacked height (for the scroll bar)

private:
    void  Apply();
    float PanelHeight(const EffectDesc& d) const;
    float PanelTop(size_t i) const;

    // A draggable knob/handle hit region discovered during a Draw pass.
    struct Hit { int effect; int kind; int slot; BRect rect;
                 float min; float max; };   // kind: 0 knob,1 up,2 dn,3 remove,
                                            // 4 add,5 eq-handle
    void  DrawKnob(BRect r, const char* label, float value, float mn, float mx);
    void  DrawEqGraph(BRect r, const EffectDesc& d, int effIdx);
    void  DrawCompCurve(BRect r, const EffectDesc& d);
    int   HitTest(BPoint where, Hit* out) const;

    std::vector<EffectDesc> fChain;
    TrackId    fTrack;
    BMessenger fApply;

    // Rebuilt each Draw so hit-testing matches exactly what was drawn.
    mutable std::vector<Hit> fHits;

    // Drag state.
    int   fDragEffect = -1;
    int   fDragSlot   = -1;
    int   fDragKind   = -1;   // 0 knob, 5 eq-handle
    float fDragMin = 0, fDragMax = 1;
    BPoint fDragStart;
    float fDragStartVal = 0;
};

class EffectsWindow : public BWindow {
public:
    EffectsWindow(BRect frame, std::vector<EffectDesc> chain, TrackId track,
                  BMessenger apply);
    void MessageReceived(BMessage* msg) override;
private:
    EffectsView* fView;
};

} // namespace daw
