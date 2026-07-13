#include "MainWindow.h"

#include "TimelineView.h"
#include "UiMetrics.h"

#include <Application.h>
#include <Button.h>
#include <MessageRunner.h>
#include <StringView.h>

#include <cstdio>

namespace daw {

enum {
    MSG_PLAY  = 'play',
    MSG_STOP  = 'stop',
    MSG_PULSE = 'puls',
};

static constexpr float kTransportH = 36.0f;
static constexpr bigtime_t kPulseInterval = 16000;   // ~60 Hz, microseconds

MainWindow::MainWindow(BRect frame, const Project* project,
                       const PeakMap* peaks)
    : BWindow(frame, "Haiku DAW", B_TITLED_WINDOW,
              B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE),
      fProject(project) {
    BRect bounds = Bounds();

    // --- Transport bar (top strip) ---
    BRect barRect(0, 0, bounds.right, kTransportH);
    BView* bar = new BView(barRect, "transport",
                           B_FOLLOW_LEFT_RIGHT | B_FOLLOW_TOP, B_WILL_DRAW);
    bar->SetViewColor(ColHeader());
    AddChild(bar);

    BButton* play = new BButton(BRect(6, 5, 76, kTransportH - 5), "play",
                                "Play", new BMessage(MSG_PLAY));
    BButton* stop = new BButton(BRect(82, 5, 152, kTransportH - 5), "stop",
                                "Stop", new BMessage(MSG_STOP));
    bar->AddChild(play);
    bar->AddChild(stop);

    fTimeView = new BStringView(BRect(170, 8, 320, kTransportH - 6),
                                "time", "0:00.000");
    fTimeView->SetViewColor(ColHeader());
    fTimeView->SetHighColor(ColText());
    bar->AddChild(fTimeView);

    // --- Timeline (fills the rest) ---
    BRect tlRect(0, kTransportH + 1, bounds.right, bounds.bottom);
    fTimeline = new TimelineView(tlRect, project);
    fTimeline->SetPeaks(peaks);
    AddChild(fTimeline);
}

MainWindow::~MainWindow() {
    delete fPulse;
    // fEngine's destructor stops playback and joins disk threads.
}

void MainWindow::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case MSG_PLAY:  StartPlayback(); break;
        case MSG_STOP:  StopPlayback();  break;
        case MSG_PULSE: {
            if (!fEngine) break;
            const Frame ph = fEngine->Playhead();
            fTimeline->SetPlayhead(ph);
            UpdateTimeReadout(ph);
            if (fEngine->IsFinished())
                StopPlayback();
            break;
        }
        default:
            BWindow::MessageReceived(msg);
    }
}

void MainWindow::StartPlayback() {
    // Rebuild the engine from the current model each time (RT-safe: no live
    // mutation of a running graph). A fresh Engine starts its playhead at 0.
    fEngine.reset(new Engine());
    if (fEngine->Load(*fProject) != B_OK) {
        std::fprintf(stderr, "MainWindow: nothing to play\n");
        fEngine.reset();
        return;
    }
    fEngine->Start();

    delete fPulse;
    fPulse = new BMessageRunner(BMessenger(this), new BMessage(MSG_PULSE),
                                kPulseInterval);
}

void MainWindow::StopPlayback() {
    delete fPulse;
    fPulse = nullptr;
    if (fEngine)
        fEngine->Stop();
    // Leave the playhead where it stopped; the readout keeps its last value.
}

void MainWindow::UpdateTimeReadout(Frame playhead) {
    const double rate = fEngine ? fEngine->OutputRate() : fProject->sampleRate;
    const double sec  = rate > 0 ? playhead / rate : 0.0;
    const int    mins = static_cast<int>(sec / 60.0);
    const double rem  = sec - mins * 60.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%06.3f", mins, rem);
    fTimeView->SetText(buf);
}

bool MainWindow::QuitRequested() {
    StopPlayback();
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}

} // namespace daw
