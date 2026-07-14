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
    int         colorIndex = 0;
};

// One strip's edited state -> the model. Fields: int64 "track", float "gain",
// float "pan", bool "mute", bool "solo".
constexpr uint32 kMsgApplyMix    = 'mxap';
// Master gain -> the model. Field: float "gain".
constexpr uint32 kMsgApplyMaster = 'mmst';
// Live peaks pushed from the main window. Per track: int64 "tid", float "pl",
// "pr"; plus master float "mpl","mpr".
constexpr uint32 kMsgMixPeaks    = 'mpks';

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

private:
    float StripX(int i) const;       // left x of strip i (i == count -> master)
    void  DrawStrip(int i, const char* name, float gain, float pan,
                    bool muted, bool soloed, int colorIndex,
                    float peakL, float peakR, bool master);
    int   StripAt(BPoint where) const;   // -1 none, count == master
    void  ApplyStrip(int i);

    std::vector<MixerStripInfo> fStrips;
    std::map<uint64, std::pair<float, float>> fPeaks;
    float      fMasterGain;
    float      fMasterL = 0.0f, fMasterR = 0.0f;
    BMessenger fApply;

    enum class Drag { None, Fader, Pan };
    Drag fDrag = Drag::None;
    int  fDragStrip = -1;   // count == master
};

class MixerWindow : public BWindow {
public:
    MixerWindow(BRect frame, std::vector<MixerStripInfo> strips,
                float masterGain, BMessenger apply);
    void MessageReceived(BMessage* msg) override;
private:
    MixerStripsView* fView;
};

} // namespace daw
