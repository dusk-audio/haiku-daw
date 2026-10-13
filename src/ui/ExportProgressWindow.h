// ExportProgressWindow — a status bar and a Cancel button for a running job.
//
// Owned by MainWindow for the duration of ONE job — an export, or (M4.4) a
// region job: the freeze render, a reverse, a normalize, a strip silence. The
// label names what is running; the protocol is the same either way, which is
// why this one window serves both.
//
// The worker thread never talks to it: MainWindow pushes the fraction it reads
// from its own atomic on the 60 Hz pulse (kMsgExportProgress), and Cancel posts
// kMsgExportCancel back, which raises the job's cancel flag. The window closes
// when MainWindow says the job is over — never on its own, so a finished bar
// stays visible until the outcome is reported.
#pragma once

#include <Messenger.h>
#include <Window.h>

class BStatusBar;

namespace daw {

class ExportProgressWindow : public BWindow {
public:
    // `label` is what is running ("Rendering mix", "Freezing track"); the
    // window's title comes from it, so the Deskbar entry says the same thing.
    ExportProgressWindow(BRect frame, BMessenger main, const char* label);
    void MessageReceived(BMessage* msg) override;

private:
    BStatusBar* fBar  = nullptr;
    BMessenger  fMain;
};

} // namespace daw
