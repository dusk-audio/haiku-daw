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
    int         fxCount = 0;
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
constexpr uint32 kMsgMixStrips   = 'mstr';
// Strip section buttons -> the main window (which owns the model + editors).
// All carry int64 "track".
constexpr uint32 kMsgMixArm      = 'mxar';   // toggle record-enable
constexpr uint32 kMsgMixMon      = 'mxmn';   // toggle input monitor
constexpr uint32 kMsgMixFx       = 'mxfx';   // open the effects editor
constexpr uint32 kMsgMixSends    = 'mxsn';   // open the sends editor
constexpr uint32 kMsgMixInst     = 'mxis';   // open the instrument editor
constexpr uint32 kMsgMixSelect   = 'mxse';   // select the track (inspector focus)

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

    std::vector<MixerStripInfo> fStrips;
    std::map<uint64, std::pair<float, float>> fPeaks;
    float      fMasterGain;
    float      fMasterL = 0.0f, fMasterR = 0.0f;
    BMessenger fApply;

    enum class Drag { None, Fader, Pan };
    Drag  fDrag = Drag::None;
    int   fDragStrip = -1;   // count == master
    float fPanGrabY = 0.0f;  // pan knob drags vertically
    float fPanOrig  = 0.0f;
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
