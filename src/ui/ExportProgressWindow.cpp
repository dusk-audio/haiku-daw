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
    : BWindow(frame, "Exporting", B_TITLED_WINDOW, B_FLOATING_APP_WINDOW_FEEL,
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
