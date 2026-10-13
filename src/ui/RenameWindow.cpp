#include "RenameWindow.h"

#include "UiMetrics.h"
#include "widgets/DawButton.h"      // the kit (M1.3)
#include "widgets/DawTextField.h"

#include <LayoutBuilder.h>
#include <Screen.h>
#include <View.h>

namespace daw {

enum { MSG_OK = 'rnok' };

RenameWindow::RenameWindow(BRect frame, TrackId track, const char* current,
                           BMessenger apply, uint32 what, int64 aux,
                           const char* title)
    : BWindow(frame,
              title ? title
                    : (what == kMsgRenameMarker ? "Rename Marker"
                                                : "Rename Track"),
              B_TITLED_WINDOW, B_FLOATING_APP_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS
              | B_CLOSE_ON_ESCAPE),
      fTrack(track), fApply(apply), fWhat(what), fAux(aux),
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
    // A visible way out beside the keyboard one: the same B_QUIT_REQUESTED
    // Escape posts (B_CLOSE_ON_ESCAPE above), so both leave without renaming.
    DawButton* cancel = new DawButton("cx", "Cancel",
                                      new BMessage(B_QUIT_REQUESTED));
    BLayoutBuilder::Group<>(root, B_VERTICAL, Themed(8.0f))
        .SetInsets(Themed(8.0f))
        .Add(fText)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(ok)
        .End()
        .End();

    ResizeToPreferred();
    // Clamp to the screen (package 50). The window opens AT the caller's
    // anchor — a clip under the pointer, a row in a list — and that anchor can
    // sit near an edge (or be off the screen entirely, for a clip at the far
    // right of a scrolled timeline), which used to put the whole window,
    // OK button included, where it could not be reached. The guidelines' rule
    // for a content-sized window (UI_GUIDELINES §4): size from the content,
    // then clamp to BScreen.
    {
        BScreen screen(this);
        if (screen.IsValid()) {
            const BRect sf = screen.Frame();
            BRect f = Frame();
            if (f.right  > sf.right)  f.OffsetBy(sf.right - f.right, 0.0f);
            if (f.bottom > sf.bottom) f.OffsetBy(0.0f, sf.bottom - f.bottom);
            if (f.left   < sf.left)   f.OffsetBy(sf.left - f.left, 0.0f);
            if (f.top    < sf.top)    f.OffsetBy(0.0f, sf.top - f.top);
            MoveTo(f.left, f.top);
        }
    }
    fText->MakeFocus(true);
}

void RenameWindow::MessageReceived(BMessage* msg) {
    if (msg->what == MSG_OK) {
        BMessage m(fWhat);
        m.AddInt64("track", (int64)fTrack);
        m.AddInt64("aux", fAux);   // 0 for a rename; the insert index for a preset
        m.AddString("name", fText->Text());
        m.AddString("oldname", fOldName.c_str());   // disambiguates same-frame markers
        fApply.SendMessage(&m);
        Quit();
        return;
    }
    BWindow::MessageReceived(msg);
}

} // namespace daw
