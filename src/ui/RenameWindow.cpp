#include "RenameWindow.h"

#include "UiMetrics.h"
#include "widgets/DawButton.h"      // the kit (M1.3)
#include "widgets/DawTextField.h"

#include <LayoutBuilder.h>
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
    // Layout Kit (M1.4): the window sizes itself to its contents, which is
    // also what keeps it right at a larger font -- the old fixed rects did
    // not grow with the controls inside them.
    BView* root = new BView("root", B_WILL_DRAW);
    root->SetViewColor(ColHeader());
    BLayoutBuilder::Group<>(this, B_VERTICAL).Add(root);

    fText = new DawTextField("name", "Name:", current, new BMessage(MSG_OK));
    DawButton* ok = new DawButton("ok", "OK", new BMessage(MSG_OK));
    ok->MakeDefault(true);
    BLayoutBuilder::Group<>(root, B_VERTICAL, Themed(8.0f))
        .SetInsets(Themed(8.0f))
        .Add(fText)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(ok)
        .End()
        .End();

    ResizeToPreferred();
    fText->MakeFocus(true);
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
