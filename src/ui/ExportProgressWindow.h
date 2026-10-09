// ExportProgressWindow — a status bar and a Cancel button for a running bounce.
//
// Owned by MainWindow for the duration of one export. The worker thread never
// talks to it: MainWindow pushes the fraction it reads from its own atomic on
// the 60 Hz pulse (kMsgExportProgress), and Cancel posts kMsgExportCancel back,
// which raises the job's cancel flag. The window closes when MainWindow says
// the export is over — never on its own, so a finished bar stays visible until
// the outcome is reported.
#pragma once

#include <Messenger.h>
#include <Window.h>

class BStatusBar;

namespace daw {

class ExportProgressWindow : public BWindow {
public:
    ExportProgressWindow(BRect frame, BMessenger main, const char* label);
    void MessageReceived(BMessage* msg) override;

private:
    BStatusBar* fBar  = nullptr;
    BMessenger  fMain;
};

} // namespace daw
