#include "MainWindow.h"

#include "TimelineView.h"

#include <Application.h>

namespace daw {

MainWindow::MainWindow(BRect frame, const Project* project,
                       const PeakMap* peaks)
    : BWindow(frame, "Haiku DAW", B_TITLED_WINDOW,
              B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE) {
    BRect bounds = Bounds();
    fTimeline = new TimelineView(bounds, project);
    fTimeline->SetPeaks(peaks);
    AddChild(fTimeline);
}

bool MainWindow::QuitRequested() {
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}

} // namespace daw
