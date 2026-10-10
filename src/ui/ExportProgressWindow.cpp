#include "ExportProgressWindow.h"

#include "ExportWindow.h"   // kMsgExportProgress / kMsgExportCancel
#include "UiMetrics.h"

#include "widgets/DawButton.h"   // the kit (M1.3)
#include <StatusBar.h>
#include <View.h>

namespace daw {

ExportProgressWindow::ExportProgressWindow(BRect frame, BMessenger main,
                                           const char* label)
    : BWindow(frame, "Exporting", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_NOT_CLOSABLE
              | B_ASYNCHRONOUS_CONTROLS),
      fMain(main) {
    BView* root = new BView(Bounds(), "root", B_FOLLOW_ALL_SIDES, B_WILL_DRAW);
    root->SetViewColor(ColHeader());
    AddChild(root);

    const float w = Bounds().Width();
    fBar = new BStatusBar(BRect(8, 10, w - 8, 32), "bar", label, nullptr);
    fBar->SetMaxValue(100.0f);
    fBar->SetBarColor(ColAccent());
    root->AddChild(fBar);

    DawButton* cancel = new DawButton(BRect(w - 90, 40, w - 8, 64), "cx", "Cancel",
                                  new BMessage(kMsgExportCancel));
    cancel->SetTarget(fMain);   // straight to MainWindow, which owns the job
    root->AddChild(cancel);
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
