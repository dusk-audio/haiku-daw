#include "ExportProgressWindow.h"

#include "ExportWindow.h"   // kMsgExportProgress / kMsgExportCancel
#include "UiMetrics.h"

#include "widgets/DawButton.h"   // the kit (M1.3)
#include <LayoutBuilder.h>
#include <StatusBar.h>
#include <View.h>

namespace daw {

ExportProgressWindow::ExportProgressWindow(BRect frame, BMessenger main,
                                           const char* label)
    // Look and feel as separate arguments (see ExportWindow): written as
    // (B_TITLED_WINDOW, B_FLOATING_APP_WINDOW_FEEL, <flags>), this call
    // resolved to BWindow's (window_type, flags, workspace) overload and every
    // flag landed in `workspace` -- the bar was zoomable, resizable and
    // closable, and only a stray B_NOT_H_RESIZABLE survived.
    //
    // B_NOT_CLOSABLE removes the title bar's close box and the Cmd-W shortcut
    // (Window.cpp: the flag is only consulted when the shortcut is installed);
    // the app's own B_QUIT_REQUESTED still closes the bar when the render
    // ends, and Cancel still stops it.
    : BWindow(frame, label ? label : "Working", B_TITLED_WINDOW_LOOK,
              B_NORMAL_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_NOT_CLOSABLE
              | B_ASYNCHRONOUS_CONTROLS),
      fMain(main) {
    BView* root = new ThemedView("root", B_WILL_DRAW);
    BLayoutBuilder::Group<>(this, B_VERTICAL).Add(root);

    fBar = new BStatusBar("bar", label, nullptr);
    fBar->SetMaxValue(100.0f);
    fBar->SetBarColor(ColAccent());
    DawButton* cancel = new DawButton("cx", "Cancel",
                                      new BMessage(kMsgExportCancel));
    cancel->SetTarget(fMain);   // straight to MainWindow, which owns the job
    BLayoutBuilder::Group<>(root, B_VERTICAL, Themed(8.0f))
        .SetInsets(Themed(8.0f))
        .Add(fBar)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
        .End()
        .End();

    ResizeToPreferred();
}

void ExportProgressWindow::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgExportProgress) {
        float f = 0.0f;
        if (msg->FindFloat("f", &f) == B_OK && fBar) {
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            fBar->SetTo(f * 100.0f);
        }
        return;
    }
    BWindow::MessageReceived(msg);
}

} // namespace daw
