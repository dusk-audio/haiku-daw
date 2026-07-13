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

constexpr uint32 kMsgRenameTrack = 'rntk';   // int64 "track", string "name"

class RenameWindow : public BWindow {
public:
    RenameWindow(BRect frame, TrackId track, const char* current,
                 BMessenger apply);

    void MessageReceived(BMessage* msg) override;

private:
    TrackId       fTrack;
    BMessenger    fApply;
    BTextControl* fText;
};

} // namespace daw
