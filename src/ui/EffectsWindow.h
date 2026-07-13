// EffectsWindow — a small editor for one track's effect chain.
//
// A separate BWindow runs on its own looper thread, so it must NOT touch the
// shared Project directly (that would race the main thread's paint/playback).
// Instead it holds a local snapshot of the chain, edits that, and posts the
// whole updated chain to the main window (via `apply`), which owns all model
// mutation and repaints. Opened from a track's FX box.
#pragma once

#include "../model/Effect.h"
#include "../model/types.h"

#include <Messenger.h>
#include <Window.h>

#include <vector>

namespace daw {

// Message the main window handles to apply an edited chain to the model.
// Fields: int64 "track", int32[] "et" (type), float[] "e0".."e3" (params).
constexpr uint32 kMsgApplyFx = 'fxap';

class EffectsWindow : public BWindow {
public:
    EffectsWindow(BRect frame, std::vector<EffectDesc> chain, TrackId track,
                  BMessenger apply);

    void MessageReceived(BMessage* msg) override;

private:
    void Rebuild();   // (re)build the UI from the local chain
    void Apply();     // post the local chain to the main window

    std::vector<EffectDesc> fChain;   // local snapshot (edited here)
    TrackId    fTrack;
    BMessenger fApply;                // -> main window
    BView*     fRoot;
};

} // namespace daw
