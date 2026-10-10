#include "RenameWindow.h"

#include "UiMetrics.h"
#include "widgets/DawButton.h"      // the kit (M1.3)
#include "widgets/DawTextField.h"

#include <View.h>

namespace daw {

enum { MSG_OK = 'rnok' };

RenameWindow::RenameWindow(BRect frame, TrackId track, const char* current,
                           BMessenger apply, uint32 what)
    : BWindow(frame, what == kMsgRenameMarker ? "Rename Marker" : "Rename Track",
              B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS),
      fTrack(track), fApply(apply), fWhat(what),
      fOldName(current ? current : "") {
    BView* root = new BView(Bounds(), "root", B_FOLLOW_ALL_SIDES, B_WILL_DRAW);
    root->SetViewColor(ColHeader());
    AddChild(root);

    // Design pixels through Themed(): the window is sized by its caller, so
    // only the INSETS scale (M1.2 slice B's rule).
    const float pad = Themed(8.0f);
    fText = new DawTextField(BRect(pad, Themed(10.0f), Bounds().right - pad,
                                   Themed(32.0f)),
                             "name", "Name:", current, new BMessage(MSG_OK));
    root->AddChild(fText);
    fText->MakeFocus(true);

    DawButton* ok = new DawButton(BRect(Bounds().right - Themed(80.0f),
                                        Themed(40.0f),
                                        Bounds().right - pad, Themed(62.0f)),
                                  "ok", "OK", new BMessage(MSG_OK));
    ok->MakeDefault(true);
    root->AddChild(ok);
}

void RenameWindow::MessageReceived(BMessage* msg) {
    if (msg->what == MSG_OK) {
        BMessage m(fWhat);
        m.AddInt64("track", (int64)fTrack);
        m.AddString("name", fText->Text());
        m.AddString("oldname", fOldName.c_str());   // disambiguates same-frame markers
        fApply.SendMessage(&m);
        Quit();
        return;
    }
    BWindow::MessageReceived(msg);
}

} // namespace daw
