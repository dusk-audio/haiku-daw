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

#include <string>
#include <vector>

class BMessageRunner;

namespace daw {

// What to call an insert in the UI: the plugin's own name for plugin-backed
// effects, the effect type otherwise. Shared so the channel strip's slot list
// and the editor's panel headers cannot drift apart -- and because getting it
// right for LV2 needs a host lookup (pluginName holds the URI, which is
// unreadable as a label), which is not worth writing twice.
std::string EffectDisplayName(const EffectDesc& d);

// A fresh insert descriptor for `type` (`pluginId` is the add-on name or LV2 URI
// for plugin-backed types, ignored otherwise). Shared with the channel strip so
// there is ONE place that knows a plugin must be seeded with its host's port
// defaults rather than zeros -- zeros switch off the `Enabled` port that LV2
// plugins routinely expose, and the plugin renders silence.
EffectDesc MakeInsertDesc(EffectType type, const std::string& pluginId);

// Fields: int64 "track"; per effect int32 "et" (type), int32 "ec" (param count),
// float[] "ep" (all params concatenated).
constexpr uint32 kMsgApplyFx = 'fxap';

// Live single-param preview during a knob/handle drag (int64 "track",
// int32 "fx","slot", float "val"): applied straight to the running engine so
// the effect responds while you drag; the undoable commit is kMsgApplyFx on
// mouse-up.
constexpr uint32 kMsgFxLive = 'fxlv';
// A native plugin editor's COMMITTED parameter writes: what kMsgFxLive previewed
// while the control moved, made permanent in the model once the gesture goes
// quiet (one undo step per gesture). Sent by Lv2UiWindow on a debounce; fields
// are int64 "track", int32 "fx", then one int32 "slot" and one float "val" per
// changed parameter, in matching order.
constexpr uint32 kMsgFxParamCommit = 'fxpc';
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
// Lv2UiWindow -> MainWindow: "this native editor is open on this insert, publish
// its control values to me". Fields: int64 "track", int32 "fx", messenger
// "msgr". The SAME message with no messenger means the editor closed and the
// engine must stop watching -- MainWindow also clears the watch by itself if the
// messenger dies, so a crash in an editor cannot leave the engine publishing to
// nothing.
constexpr uint32 kMsgFxWatch     = 'fxpw';
// MainWindow -> Lv2UiWindow: the watched insert's values, one float "v" per
// parameter in slot order. Absolute, not a delta, so a dropped frame costs
// nothing. Sent only while a watch is registered, on the same 60 Hz pulse that
// feeds the meters -- and only when the engine's change counter says something
// actually moved.
constexpr uint32 kMsgFxParams    = 'fxpv';

class EffectsView : public BView {
public:
    // `focusSlot` >= 0 shows ONLY that insert (the channel strip opens the
    // editor on the slot that was clicked). The view still holds the WHOLE
    // chain: Apply() posts every descriptor, so filtering the data instead of
    // the drawing would delete the other inserts on the first edit.
    EffectsView(BRect frame, std::vector<EffectDesc> chain, TrackId track,
                BMessenger apply, int focusSlot = -1);
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
    //  10 open the plugin's own (native) editor
    //  11 per-insert bypass toggle  12 per-insert wet/dry slider
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
    // Show only this chain index, or -1 for the whole chain. The chain itself is
    // always complete -- see the constructor comment.
    int        fFocus = -1;

    // Rebuilt each Draw so hit-testing matches exactly what was drawn.
    mutable std::vector<Hit> fHits;

    // Drag state.
    int   fDragEffect = -1;
    int   fDragSlot   = -1;
    int   fDragKind   = -1;   // 0 knob, 5 eq-handle, 9 param slider, 12 wet/dry
    float fDragMin = 0, fDragMax = 1;
    BPoint fDragStart;
    float fDragStartVal = 0;
    // The hit rect the drag started on. A knob drag is relative (delta from
    // where it began) so it needs no rect, but a parameter slider is absolute --
    // the value is wherever the pointer sits ALONG THE TRACK -- so the track has
    // to be remembered rather than recomputed from a layout that has since
    // scrolled.
    BRect fDragRect;
    // The dragged parameter's accepted domain, so the drag snaps to it instead
    // of writing a value the plugin does not define.
    bool  fDragInteger = false;
    bool  fDragToggled = false;
    std::vector<float> fDragScalePoints;   // empty unless an enumeration

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
    // focusSlot >= 0 opens on one insert alone; -1 shows the whole chain.
    EffectsWindow(BRect frame, std::vector<EffectDesc> chain, TrackId track,
                  BMessenger apply, int focusSlot = -1);
    void MessageReceived(BMessage* msg) override;
    void DispatchMessage(BMessage* msg, BHandler* h) override;  // spacebar -> transport
    bool QuitRequested() override;
private:
    EffectsView* fView;
    TrackId      fTrack;
    BMessenger   fApply;   // to MainWindow (open/close notices)
};

} // namespace daw
