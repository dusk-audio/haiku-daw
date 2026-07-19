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

class BMessageRunner;

namespace daw {

// Fields: int64 "track"; per effect int32 "et" (type), int32 "ec" (param count),
// float[] "ep" (all params concatenated).
constexpr uint32 kMsgApplyFx = 'fxap';

// Live single-param preview during a knob/handle drag (int64 "track",
// int32 "fx","slot", float "val"): applied straight to the running engine so
// the effect responds while you drag; the undoable commit is kMsgApplyFx on
// mouse-up.
constexpr uint32 kMsgFxLive = 'fxlv';
// Right-click a knob to toggle automation of that param. Fields: int64 "track",
// int32 "fx" (effect index), int32 "slot", float "val" (current value).
constexpr uint32 kMsgToggleFxAuto = 'fxat';
// EffectsWindow -> MainWindow: this editor opened/closed for a track. Fields:
// int64 "track", messenger "msgr" (open only). MainWindow sets the engine meter
// focus and pushes kMsgFxMeter here while playing.
constexpr uint32 kMsgFxWinOpen   = 'fxwo';
constexpr uint32 kMsgFxWinClosed = 'fxwc';
// MainWindow -> EffectsWindow: live effect meters. Fields: float[] "gr" (per-fx
// gain reduction dB), float[] "spec" (EQ spectrum dB), int32 "specfx" (which fx
// the spectrum belongs to), int32 "specn" (bin count).
constexpr uint32 kMsgFxMeter     = 'fxmt';

class EffectsView : public BView {
public:
    EffectsView(BRect frame, std::vector<EffectDesc> chain, TrackId track,
                BMessenger apply);
    ~EffectsView() override;

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    void MouseUp(BPoint where) override;
    void MessageReceived(BMessage* msg) override;   // wheel-adjust + its commit

    float ContentHeight() const;    // total stacked height (for the scroll bar)
    void  UpdateScrollRange();      // re-fit the enclosing scroll bar to content

    // Live meters from the engine (per-fx gain reduction + one EQ spectrum).
    void SetMeters(const float* gr, int grN,
                   const float* spec, int specN, int specFx);

    // Commit a wheel edit whose debounce timer has not fired yet. The window
    // calls this on close: the engine already heard the change (kMsgFxLive), so
    // dropping the commit would leave the model behind the audio.
    void FlushPendingEdit();

private:
    void  Apply();
    float PanelHeight(const EffectDesc& d) const;
    float PanelTop(size_t i) const;

    // A draggable knob/handle hit region discovered during a Draw pass.
    // kind identifies what was hit. Every value here is dispatched by number in
    // MouseDown/WheelAdjust, so a duplicate silently routes one control to
    // another's handler and still compiles -- keep this list complete.
    //   0 knob        1 move up      2 move down   3 remove     4 add
    //   5 eq handle   6 FFT toggle   7 reverb type 8 delay sync
    //   9 plugin parameter slider (vertical generic list)
    struct Hit { int effect; int kind; int slot; BRect rect;
                 float min; float max; };
    void  DrawKnob(BRect r, const char* label, float value, float mn, float mx);
    void  DrawEqGraph(BRect r, const EffectDesc& d, int effIdx);
    void  DrawCompCurve(BRect r, const EffectDesc& d, int effIdx);
    int   HitTest(BPoint where, Hit* out) const;
    void  OpenBrowser();      // "Add Effect..." -> searchable plugin browser

    // Mouse-wheel parameter edit. Returns false when the pointer is not over a
    // control, so the wheel keeps scrolling the panel list.
    bool  WheelAdjust(BPoint where, float dy);
    void  ScheduleCommit();   // debounce wheel notches into one undo step

    std::vector<EffectDesc> fChain;
    TrackId    fTrack;
    BMessenger fApply;

    // Rebuilt each Draw so hit-testing matches exactly what was drawn.
    mutable std::vector<Hit> fHits;

    // Drag state.
    int   fDragEffect = -1;
    int   fDragSlot   = -1;
    int   fDragKind   = -1;   // 0 knob, 5 eq-handle, 9 parameter slider
    float fDragMin = 0, fDragMax = 1;
    BPoint fDragStart;
    float fDragStartVal = 0;
    // The hit rect the drag started on. A knob drag is relative (delta from
    // where it began) so it needs no rect, but a parameter slider is absolute --
    // the value is wherever the pointer sits ALONG THE TRACK -- so the track has
    // to be remembered rather than recomputed from a layout that has since
    // scrolled.
    BRect fDragRect;
    // Set when the dragged parameter accepts whole numbers only (lv2:toggled and
    // friends), so the drag snaps instead of writing fractions into a toggle.
    bool  fDragInteger = false;

    // Pending debounced commit of a wheel gesture (null when idle).
    BMessageRunner* fCommit = nullptr;

    // Live meters (set from the engine via SetMeters).
    static constexpr int kSpecMax = 256;
    float fGr[16] = {};       // per-fx gain reduction (dB, <= 0)
    int   fGrN = 0;
    float fSpec[kSpecMax] = {};
    int   fSpecN = 0;
    int   fSpecFx = -1;       // which effect the spectrum belongs to
    bool  fFftOn = true;      // EQ analyzer overlay toggle
};

class EffectsWindow : public BWindow {
public:
    EffectsWindow(BRect frame, std::vector<EffectDesc> chain, TrackId track,
                  BMessenger apply);
    void MessageReceived(BMessage* msg) override;
    void DispatchMessage(BMessage* msg, BHandler* h) override;  // spacebar -> transport
    bool QuitRequested() override;
private:
    EffectsView* fView;
    TrackId      fTrack;
    BMessenger   fApply;   // to MainWindow (open/close notices)
};

} // namespace daw
