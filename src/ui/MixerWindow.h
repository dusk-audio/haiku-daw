// MixerWindow — a horizontal rack of channel strips + a master strip.
//
// Fully custom-drawn (a single MixerStripsView) so it matches the dark timeline
// theme instead of falling back to light OS controls. Runs on its own looper
// thread; it holds a snapshot of the mix state, edits the local copy on mouse
// input, and posts changes to the main window (kMsgApplyMix / kMsgApplyMaster).
// The main window pushes live per-track peaks via kMsgMixPeaks.
#pragma once


#include "Theme.h"   // ThemeAware (T1)
#include <Messenger.h>
#include <View.h>
#include <Window.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace daw {

// One insert, as much of it as a mixer strip has to draw. The strip shows a
// label and a bypass dot, so the snapshot carries a label and a bypass flag --
// not an EffectDesc. Deliberate: the mixer runs on its own looper and must not
// hold model structures it could then be tempted to edit, and the label for an
// LV2 insert needs a host lookup that only the main thread does safely.
//
// The consequence is that the mixer cannot post a chain-replace the way the
// inspector does (it has no descriptors to replace it WITH), so every insert
// edit here posts an intent -- bypass this index, move this index -- and the
// main window turns it into a command against the real chain.
struct MixerInsertInfo {
    std::string name;
    bool        bypassed = false;
};

// One aux send, as much of it as the strip draws: where it goes and how loud
// it is. Same rule as MixerInsertInfo -- a label, not a model structure.
struct MixerSendInfo {
    std::string label;    // the destination's short name
    float       level = 1.0f;
};

struct MixerStripInfo {
    uint64      trackId;
    std::string name;
    float       gain;    // linear, 0..1.5 (1.0 == unity)
    float       pan;     // -1..+1 (0 == center)
    bool        muted;
    bool        soloed;
    bool        armed = false;
    bool        inputMonitor = false;
    int         colorIndex = 0;
    int         type = 0;        // 0 audio, 1 midi, 2 bus
    std::vector<MixerInsertInfo> fx;   // the insert chain, in order
    // Mixer strip extras (M3.4).
    float       trim = 1.0f;           // input trim (linear; kTrackTrimMax ceiling)
    bool        phaseInvert = false;   // polarity flip
    int         soloGroup = 0;         // >0 = the strip's solo group
    std::vector<MixerSendInfo> sends;  // the aux sends, in the model's order
    bool        hasInput = false;
    std::string outLabel;        // "Mst" or a bus name
};

// One strip's edited state -> the model. Fields: int64 "track", float "gain",
// float "pan", bool "mute", bool "solo", and (M3.4) float "trim", bool
// "phase", int32 "sgroup".
constexpr uint32 kMsgApplyMix    = 'mxap';
// One send's level (M3.4): int64 "track", int32 "index", float "level". A knob
// drag posts continuously and the window coalesces it into one undo step.
constexpr uint32 kMsgMixSend     = 'mxsl';
// The inline level field's commit, handled INSIDE the strips view (the field
// is its child): string "text" is the dB figure.
constexpr uint32 kMsgMixLevelCommit = 'mxlc';
// Master gain -> the model. Field: float "gain".
constexpr uint32 kMsgApplyMaster = 'mmst';
// Live peaks pushed from the main window. Per track: int64 "tid", float "pl",
// "pr"; plus master float "mpl","mpr".
constexpr uint32 kMsgMixPeaks    = 'mpks';
// Refreshed strip STATE pushed from the main window when the model changes
// elsewhere (mute/solo/arm/fx/routing). Per strip the same fields the mixer
// snapshot uses; master gain in float "mg".
//
// The insert chains travel as ONE flat run of int32 "fx" (this strip's insert
// count), string "fxn" and bool "fxb" across all strips, sliced by the counts in
// strip order -- the same shape kMsgApplyFx already uses for its parameters,
// because a BMessage cannot nest an array per strip.
constexpr uint32 kMsgMixStrips   = 'mstr';
// Strip section buttons -> the main window (which owns the model + editors).
// All carry int64 "track".
constexpr uint32 kMsgMixArm      = 'mxar';   // toggle record-enable
constexpr uint32 kMsgMixMon      = 'mxmn';   // toggle input monitor
constexpr uint32 kMsgMixFx       = 'mxfx';   // open the effects editor; optional
                                             // int32 "slot" opens on one insert
constexpr uint32 kMsgMixSends    = 'mxsn';   // open the sends editor
constexpr uint32 kMsgMixInst     = 'mxis';   // open the instrument editor
constexpr uint32 kMsgMixSelect   = 'mxse';   // select the track (inspector focus)
// Insert-slot edits. The mixer holds a snapshot, not descriptors, so these name
// what to do and the main window does it to the real chain (see MixerInsertInfo).
constexpr uint32 kMsgMixFxBypass = 'mxfb';   // + int32 "fx": toggle that insert
constexpr uint32 kMsgMixFxMove   = 'mxfm';   // + int32 "from","to": reorder
constexpr uint32 kMsgMixFxAdd    = 'mxfa';   // open the plugin browser for it

// Bring the (already open) mixer to the front. The main window sends this
// instead of opening a second rack (M1.4).
constexpr uint32 kMsgMixerActivate = 'mxac';

class MixerStripsView : public BView, public ThemeAware {
public:
    void ApplyTheme() override;   // T1: the cached lane colour
public:
    MixerStripsView(BRect frame, std::vector<MixerStripInfo> strips,
                    float masterGain, BMessenger apply);

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    void MouseUp(BPoint where) override;
    void MessageReceived(BMessage* msg) override;   // wheel: pan the rack

    void SetPeaks(const std::map<uint64, std::pair<float, float>>& peaks,
                  float masterL, float masterR);
    // Replace the strip state from a refresh (ignored mid-drag so it doesn't
    // disrupt a fader/pan gesture).
    void SetStrips(std::vector<MixerStripInfo> strips, float masterGain);

    // The strip state this rack is showing. Read by the functional test to
    // prove a drag elsewhere (the timeline's header fader) has ALREADY reached
    // it mid-gesture -- the rack's snapshot is the observable the inspector,
    // which draws straight from the model, cannot offer.
    const std::vector<MixerStripInfo>& Strips() const { return fStrips; }

private:
    float StripX(int i) const;       // left x of strip i (i == count -> master)
    void  DrawStrip(int i, const MixerStripInfo* info, float gain,
                    float peakL, float peakR, bool master);
    int   StripAt(BPoint where) const;   // -1 none, count == master
    void  ApplyStrip(int i);
    void  PostSendLevel(int i, int index, float level);   // one send (M3.4)
    void  Post(uint32 what, uint64 track);   // section-button message helper

    // Rows the insert block reserves. Every strip reserves the SAME number --
    // the largest any strip needs -- so the pan knobs, faders and meters stay on
    // one line across the rack, the way a console reads. A strip draws only its
    // own rows into that block; the master strip draws none.
    int   FxRows() const { return fFxRows; }
    void  RecomputeFxRows();
    // Rows strip `s` actually draws: one per insert plus a trailing empty slot,
    // or the cap with the last row standing in for the remainder.
    static int  FxRowsFor(const MixerStripInfo& s);
    // ... and how many aux-send rows it needs (M3.4), capped at kMaxSendRows.
    static int  SendRowsFor(const MixerStripInfo& s);
    int   SendRows() const { return fSendRows; }
    // The strip's trim as a knob value (kTrackTrimMax..unity..0 mapped to
    // -1..0..+1) and back. Unity at 12 o'clock, which is what a trim knob is.
    static float TrimToKnob(float trim);
    static float KnobToTrim(float knob);

    // The inline dB entry (M3.4): a real text field, placed over the strip's
    // value row on a double-click. Built once and hidden until then.
    void BeginLevelEntry(int strip);
    void CommitLevelEntry();
    void CancelLevelEntry();
    class DawTextField* fLevelField = nullptr;
    int   fLevelStrip = -1;

    float fScrollX = 0.0f;           // horizontal pan when the rack is wider
    float ContentWidth() const;      // all strips + master + margins

    std::vector<MixerStripInfo> fStrips;
    std::map<uint64, std::pair<float, float>> fPeaks;
    float      fMasterGain;
    float      fMasterL = 0.0f, fMasterR = 0.0f;
    BMessenger fApply;
    int        fFxRows = 1;
    int        fSendRows = 0;

    enum class Drag { None, Fader, Pan, FxSlot, Trim, Send };
    Drag  fDrag = Drag::None;
    int   fDragStrip = -1;   // count == master
    float fPanGrabY = 0.0f;  // pan knob drags vertically
    float fPanOrig  = 0.0f;
    float fTrimGrabY = 0.0f; // trim knob: vertical drag, like the pan knob
    float fTrimOrig  = 1.0f;
    float fSendGrabY = 0.0f; // send knob: the same gesture
    float fSendOrig  = 1.0f;
    int   fSendIndex = -1;
    // Insert reorder: the row grabbed, the row it would land on, and the track
    // it started on. The id is what the move is posted against, so a strip list
    // that has been rebuilt underneath cannot redirect the edit at another track.
    int     fDragFxFrom  = -1;
    int     fDragFxTo    = -1;
    uint64  fDragFxTrack = 0;
};

class MixerWindow : public BWindow {
public:
    MixerWindow(BRect frame, std::vector<MixerStripInfo> strips,
                float masterGain, BMessenger apply);
    void MessageReceived(BMessage* msg) override;
    void DispatchMessage(BMessage* msg, BHandler* h) override;  // spacebar -> transport
private:
    MixerStripsView* fView;
    BMessenger       fApply;
};

} // namespace daw
