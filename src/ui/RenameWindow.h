// RenameWindow — a tiny modal-ish prompt to rename a track.
//
// Like the other auxiliary windows it runs on its own thread and never touches
// the model: on OK it posts kMsgRenameTrack (int64 "track", string "name") to
// the main window, which applies it through the command stack.
#pragma once

#include "../model/types.h"

#include <Messenger.h>
#include <Window.h>

#include <string>

class BTextControl;

namespace daw {

constexpr uint32 kMsgRenameTrack  = 'rntk';   // int64 "track", string "name"
constexpr uint32 kMsgRenameMarker = 'rnmk';   // int64 "track" = marker frame

class RenameWindow : public BWindow {
public:
    // `what` is the message posted on OK (default renames a track; pass
    // kMsgRenameMarker to rename a marker, with the frame in the "track" field).
    // `aux` is echoed back as int64 "aux" for a caller that needs a second
    // number in the answer (the insert index, for "Save Preset..."), and
    // `title` overrides the window title for a non-rename use.
    RenameWindow(BRect frame, TrackId track, const char* current,
                 BMessenger apply, uint32 what = kMsgRenameTrack,
                 int64 aux = 0, const char* title = nullptr);

    void MessageReceived(BMessage* msg) override;

private:
    TrackId       fTrack;
    BMessenger    fApply;
    uint32        fWhat;
    int64         fAux;
    std::string   fOldName;   // the name before edit, echoed back as "oldname"
    BTextControl* fText;
};

} // namespace daw
