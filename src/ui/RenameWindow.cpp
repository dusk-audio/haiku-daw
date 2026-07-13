#include "RenameWindow.h"

#include "UiMetrics.h"

#include <Button.h>
#include <TextControl.h>
#include <View.h>

namespace daw {

enum { MSG_OK = 'rnok' };

RenameWindow::RenameWindow(BRect frame, TrackId track, const char* current,
                           BMessenger apply)
    : BWindow(frame, "Rename Track", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS),
      fTrack(track), fApply(apply) {
    BView* root = new BView(Bounds(), "root", B_FOLLOW_ALL_SIDES, B_WILL_DRAW);
    root->SetViewColor(ColHeader());
    AddChild(root);

    fText = new BTextControl(BRect(8, 10, Bounds().right - 8, 32),
                             "name", "Name:", current, new BMessage(MSG_OK));
    root->AddChild(fText);
    fText->MakeFocus(true);

    BButton* ok = new BButton(BRect(Bounds().right - 80, 40,
                                    Bounds().right - 8, 62),
                              "ok", "OK", new BMessage(MSG_OK));
    ok->MakeDefault(true);
    root->AddChild(ok);
}

void RenameWindow::MessageReceived(BMessage* msg) {
    if (msg->what == MSG_OK) {
        BMessage m(kMsgRenameTrack);
        m.AddInt64("track", (int64)fTrack);
        m.AddString("name", fText->Text());
        fApply.SendMessage(&m);
        Quit();
        return;
    }
    BWindow::MessageReceived(msg);
}

} // namespace daw
