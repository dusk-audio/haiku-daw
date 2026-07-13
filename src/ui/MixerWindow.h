// MixerWindow — a horizontal rack of channel strips, one per track.
//
// Like EffectsWindow, this is a separate BWindow that runs on its own looper
// thread, so it must NOT touch the shared Project directly (that would race
// the main thread's paint/playback). It receives a *snapshot* of the mix
// state (one MixerStripInfo per track) in its constructor, edits only that
// local copy, and posts each changed strip's full state to the main window
// (via `apply`). The main window owns all model mutation and repaints.
#pragma once

#include <Button.h>
#include <Messenger.h>
#include <Slider.h>
#include <StringView.h>
#include <SupportDefs.h>
#include <Window.h>

#include <string>
#include <vector>

namespace daw {

// One channel strip's mixer state. POD; TrackId is uint64_t in the model
// (see ../model/types.h), stored here as uint64 for the Interface Kit edge.
struct MixerStripInfo {
    uint64      trackId;
    std::string name;
    float       gain;    // linear, 0..1.5 (1.0 == unity)
    float       pan;     // -1..+1 (0 == center)
    bool        muted;
    bool        soloed;
};

// Message the main window handles to apply one strip's edited state to the
// model. It carries that strip's FULL current state, so the handler can just
// overwrite the track's gain/pan/mute/solo. Fields:
//   int64 "track"  — the track id (MixerStripInfo::trackId)
//   float "gain"   — linear gain, 0..1.5
//   float "pan"    — -1..+1
//   bool  "mute"   — muted
//   bool  "solo"   — soloed
constexpr uint32 kMsgApplyMix = 'mxap';

class MixerWindow : public BWindow {
public:
    MixerWindow(BRect frame, std::vector<MixerStripInfo> strips,
                BMessenger apply);

    void MessageReceived(BMessage* msg) override;

private:
    void Build();          // build the UI from the local snapshot
    void Apply(size_t i);  // post strip i's full current state to the main window

    // A strip's controls, kept so we can read live values back out.
    struct Strip {
        MixerStripInfo info;
        BSlider* gain = nullptr;   // vertical, 0..150 -> gain*100
        BSlider* pan  = nullptr;   // horizontal, -100..100 -> pan*100
        BButton* mute = nullptr;   // two-state "M"
        BButton* solo = nullptr;   // two-state "S"
    };

    std::vector<Strip> fStrips;   // local snapshot (edited here)
    BMessenger         fApply;    // -> main window
    BView*             fRoot;
};

} // namespace daw
