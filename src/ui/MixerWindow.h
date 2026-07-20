// MixerWindow — a horizontal rack of channel strips + a master strip.
//
// Fully custom-drawn (a single MixerStripsView) so it matches the dark timeline
// theme instead of falling back to light OS controls. Runs on its own looper
// thread; it holds a snapshot of the mix state, edits the local copy on mouse
// input, and posts changes to the main window (kMsgApplyMix / kMsgApplyMaster).
// The main window pushes live per-track peaks via kMsgMixPeaks.
#pragma once

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
    int         sendCount = 0;
    bool        hasInput = false;
    std::string outLabel;        // "Mst" or a bus name
};

// One strip's edited state -> the model. Fields: int64 "track", float "gain",
// float "pan", bool "mute", bool "solo".
constexpr uint32 kMsgApplyMix    = 'mxap';
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

class MixerStripsView : public BView {
public:
    MixerStripsView(BRect frame, std::vector<MixerStripInfo> strips,
                    float masterGain, BMessenger apply);

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    void MouseUp(BPoint where) override;

    void SetPeaks(const std::map<uint64, std::pair<float, float>>& peaks,
                  float masterL, float masterR);
    // Replace the strip state from a refresh (ignored mid-drag so it doesn't
    // disrupt a fader/pan gesture).
    void SetStrips(std::vector<MixerStripInfo> strips, float masterGain);

private:
    float StripX(int i) const;       // left x of strip i (i == count -> master)
    void  DrawStrip(int i, const MixerStripInfo* info, float gain,
                    float peakL, float peakR, bool master);
    int   StripAt(BPoint where) const;   // -1 none, count == master
    void  ApplyStrip(int i);
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

    std::vector<MixerStripInfo> fStrips;
    std::map<uint64, std::pair<float, float>> fPeaks;
    float      fMasterGain;
    float      fMasterL = 0.0f, fMasterR = 0.0f;
    BMessenger fApply;
    int        fFxRows = 1;

    enum class Drag { None, Fader, Pan, FxSlot };
    Drag  fDrag = Drag::None;
    int   fDragStrip = -1;   // count == master
    float fPanGrabY = 0.0f;  // pan knob drags vertically
    float fPanOrig  = 0.0f;
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
