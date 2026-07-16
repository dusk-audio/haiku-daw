// RenameWindow — a tiny modal-ish prompt to rename a track.
//
// Like the other auxiliary windows it runs on its own thread and never touches
// the model: on OK it posts kMsgRenameTrack (int64 "track", string "name") to
// the main window, which applies it through the command stack.
#pragma once

#include "../model/types.h"

#include <Messenger.h>
#include <Window.h>

class BTextControl;

namespace daw {

constexpr uint32 kMsgRenameTrack  = 'rntk';   // int64 "track", string "name"
constexpr uint32 kMsgRenameMarker = 'rnmk';   // int64 "track" = marker frame

class RenameWindow : public BWindow {
public:
    // `what` is the message posted on OK (default renames a track; pass
    // kMsgRenameMarker to rename a marker, with the frame in the "track" field).
    RenameWindow(BRect frame, TrackId track, const char* current,
                 BMessenger apply, uint32 what = kMsgRenameTrack);

    void MessageReceived(BMessage* msg) override;

private:
    TrackId       fTrack;
    BMessenger    fApply;
    uint32        fWhat;
    BTextControl* fText;
};

} // namespace daw
