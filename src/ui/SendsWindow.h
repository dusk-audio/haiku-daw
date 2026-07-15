// SendsWindow — a small editor for one track's aux sends.
//
// Same threading contract as EffectsWindow: a separate BWindow on its own
// looper thread never touches the shared Project. It holds a local snapshot of
// the send list plus the set of bus targets to choose from, edits the snapshot,
// and posts the whole updated list to the main window (via `apply`), which owns
// all model mutation. Opened from a track's "Snd" header box.
#pragma once

#include "../model/Project.h"   // daw::Send
#include "../model/types.h"

#include <Messenger.h>
#include <Window.h>

#include <string>
#include <utility>
#include <vector>

namespace daw {

// Message the main window handles to apply an edited send list to the model.
// Fields: int64 "track"; per send: int64 "sd" (dest), float "sl" (level),
// int32 "sp" (preFader 0/1).
constexpr uint32 kMsgApplySends = 'snap';

class SendsWindow : public BWindow {
public:
    // `buses` is the list of routable bus targets (id + display name).
    SendsWindow(BRect frame, std::vector<Send> sends,
                std::vector<std::pair<TrackId, std::string>> buses,
                TrackId track, BMessenger apply);

    void MessageReceived(BMessage* msg) override;
    void DispatchMessage(BMessage* msg, BHandler* h) override;  // spacebar -> transport

private:
    void Rebuild();   // (re)build the UI from the local send list
    void Apply();     // post the local send list to the main window

    std::vector<Send>                            fSends;   // local snapshot
    std::vector<std::pair<TrackId, std::string>> fBuses;   // available targets
    TrackId    fTrack;
    BMessenger fApply;
    BView*     fRoot;
};

} // namespace daw
