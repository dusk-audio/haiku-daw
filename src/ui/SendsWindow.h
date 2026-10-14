// SendsWindow — a small editor for one track's aux sends.
//
// Same threading contract as EffectsWindow: a separate BWindow on its own
// looper thread never touches the shared Project. It holds a local snapshot of
// the send list plus the set of bus targets to choose from, edits the snapshot,
// and posts the whole updated list to the main window (via `apply`), which owns
// all model mutation. Opened from a track's "Snd" header box.
#pragma once


#include "Theme.h"   // ThemeAware (T1)
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

// MainWindow -> SendsWindow: point the (single-instance) window at another
// track's sends — the whole list, the bus targets to choose from and the
// track's name for the title. The same rule the effects window follows (T2),
// for the same reason: the window holds a WHOLE-SNAPSHOT copy of the list and
// commits it wholesale, so two windows on one track silently overwrite each
// other's edits. Retargeting beats a second window.
constexpr uint32 kMsgSendsRetarget = 'snrt';
// Bring the (already open) sends window to the front.
constexpr uint32 kMsgSendsActivate = 'snac';

class SendsWindow : public BWindow, public ThemeAware {
public:
    // `buses` is the list of routable bus targets (id + display name).
    // `trackName` goes into the title: one window is retargeted from track to
    // track now, so the title is what says WHICH track's sends are in it.
    SendsWindow(BRect frame, std::vector<Send> sends,
                std::vector<std::pair<TrackId, std::string>> buses,
                TrackId track, BMessenger apply, const char* trackName = nullptr);

    void MessageReceived(BMessage* msg) override;
    void DispatchMessage(BMessage* msg, BHandler* h) override;  // spacebar -> transport

    // Point the window at another track (see kMsgSendsRetarget). Any edit the
    // local snapshot holds but has not applied is NOT committed here: the
    // window applies every edit as it is made, so the snapshot is never ahead
    // of the model.
    void Retarget(std::vector<Send> sends,
                  std::vector<std::pair<TrackId, std::string>> buses,
                  TrackId track, const char* trackName);
    TrackId Track() const { return fTrack; }

private:
    void Rebuild();   // (re)build the UI from the local send list
    void UpdateTitle(const char* trackName);
    // T1: a mode switch re-runs the build, so the panel, the hint and every
    // row take the new colours instead of keeping the old ones.
    void ApplyTheme() override { Rebuild(); }
    void Apply();     // post the local send list to the main window

    std::vector<Send>                            fSends;   // local snapshot
    std::vector<std::pair<TrackId, std::string>> fBuses;   // available targets
    TrackId    fTrack;
    std::string fTrackName;
    BMessenger fApply;
    BView*     fRoot;
};

} // namespace daw
