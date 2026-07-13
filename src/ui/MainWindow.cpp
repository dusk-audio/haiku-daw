#include "MainWindow.h"

#include "TimelineView.h"
#include "MeterView.h"
#include "UiMetrics.h"

#include "../engine/WavSource.h"

#include <Application.h>

#include <cmath>
#include <Button.h>
#include <MessageRunner.h>
#include <StringView.h>

#include <cstdio>

namespace daw {

enum {
    MSG_PLAY  = 'play',
    MSG_STOP  = 'stop',
    MSG_REC   = 'rec ',
    MSG_PULSE = 'puls',
    MSG_UNDO  = 'undo',
    MSG_REDO  = 'redo',
};

static constexpr float kTransportH = 36.0f;
static constexpr bigtime_t kPulseInterval = 16000;   // ~60 Hz, microseconds

MainWindow::MainWindow(BRect frame, Project* project, CommandStack* stack,
                       PeakMap* peaks)
    : BWindow(frame, "Haiku DAW", B_TITLED_WINDOW,
              B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE),
      fProject(project), fStack(stack), fPeaks(peaks) {
    BRect bounds = Bounds();

    // --- Transport bar (top strip) ---
    BRect barRect(0, 0, bounds.right, kTransportH);
    BView* bar = new BView(barRect, "transport",
                           B_FOLLOW_LEFT_RIGHT | B_FOLLOW_TOP, B_WILL_DRAW);
    bar->SetViewColor(ColHeader());
    AddChild(bar);

    BButton* play = new BButton(BRect(6, 5, 70, kTransportH - 5), "play",
                                "Play", new BMessage(MSG_PLAY));
    BButton* stop = new BButton(BRect(74, 5, 138, kTransportH - 5), "stop",
                                "Stop", new BMessage(MSG_STOP));
    BButton* rec  = new BButton(BRect(142, 5, 206, kTransportH - 5), "rec",
                                "Rec", new BMessage(MSG_REC));
    bar->AddChild(play);
    bar->AddChild(stop);
    bar->AddChild(rec);

    fTimeView = new BStringView(BRect(216, 8, 330, kTransportH - 6),
                                "time", "0:00.000");
    fTimeView->SetViewColor(ColHeader());
    fTimeView->SetHighColor(ColText());
    bar->AddChild(fTimeView);

    BButton* undo = new BButton(BRect(340, 5, 404, kTransportH - 5), "undo",
                                "Undo", new BMessage(MSG_UNDO));
    BButton* redo = new BButton(BRect(408, 5, 472, kTransportH - 5), "redo",
                                "Redo", new BMessage(MSG_REDO));
    bar->AddChild(undo);
    bar->AddChild(redo);

    // Master output meter, pinned to the right of the transport bar.
    fMeter = new MeterView(BRect(bounds.right - 130, 5, bounds.right - 6,
                                 kTransportH - 5));
    bar->AddChild(fMeter);

    // Keyboard: Cmd-Z / Cmd-Shift-Z.
    AddShortcut('Z', B_COMMAND_KEY, new BMessage(MSG_UNDO));
    AddShortcut('Z', B_COMMAND_KEY | B_SHIFT_KEY, new BMessage(MSG_REDO));

    // --- Timeline (fills the rest) ---
    BRect tlRect(0, kTransportH + 1, bounds.right, bounds.bottom);
    fTimeline = new TimelineView(tlRect, project, stack);
    fTimeline->SetPeaks(peaks);
    AddChild(fTimeline);
}

MainWindow::~MainWindow() {
    delete fPulse;
    // fEngine / fRecorder destructors stop their threads.
}

void MainWindow::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case MSG_PLAY:  StartPlayback(); break;
        case MSG_STOP:
            StopPlayback();
            StopRecording();
            break;
        case MSG_REC:
            // Toggle: Rec starts, Rec again (or Stop) finishes the take.
            if (fRecorder && fRecorder->IsRecording()) StopRecording();
            else                                       StartRecording();
            break;
        case kMsgSeek: {
            const Frame ph = fProject->transport.playhead;
            UpdateTimeReadout(ph);
            if (fPlaying)        // restart from the new position
                StartPlayback();
            break;
        }
        case MSG_UNDO:
            if (fStack->CanUndo()) { fStack->Undo(*fProject); fTimeline->Invalidate(); }
            break;
        case MSG_REDO:
            if (fStack->CanRedo()) { fStack->Redo(*fProject); fTimeline->Invalidate(); }
            break;
        case MSG_PULSE: {
            if (fRecorder && fRecorder->IsRecording()) {
                fMeter->SetLevels(fRecorder->PeakL(), fRecorder->PeakR());
            } else if (fPlaying && fEngine) {
                fEngine->UpdateMix(*fProject);   // live gain/pan/mute/solo
                const Frame ph = fEngine->Playhead();
                fTimeline->SetPlayhead(ph);
                UpdateTimeReadout(ph);
                fMeter->SetLevels(fEngine->PeakL(), fEngine->PeakR());
                if (fEngine->IsFinished())
                    StopPlayback();
            }
            break;
        }
        default:
            BWindow::MessageReceived(msg);
    }
}

void MainWindow::UpdatePulse() {
    const bool need = fPlaying
                   || (fRecorder && fRecorder->IsRecording());
    if (need && !fPulse) {
        fPulse = new BMessageRunner(BMessenger(this), new BMessage(MSG_PULSE),
                                    kPulseInterval);
    } else if (!need && fPulse) {
        delete fPulse;
        fPulse = nullptr;
    }
}

void MainWindow::StartPlayback() {
    if (fRecorder && fRecorder->IsRecording())
        return;   // no play-while-record in this milestone
    // Rebuild the engine from the current model each time (RT-safe: no live
    // mutation of a running graph). Playback begins at the current playhead.
    const Frame start = fProject->transport.playhead;
    fEngine.reset(new Engine());
    if (fEngine->Load(*fProject, start) != B_OK) {
        std::fprintf(stderr, "MainWindow: nothing to play\n");
        fEngine.reset();
        return;
    }
    fEngine->Start();
    fPlaying = true;
    UpdatePulse();
}

void MainWindow::StopPlayback() {
    if (fEngine)
        fEngine->Stop();
    fPlaying = false;
    UpdatePulse();
    fMeter->SetLevels(0.0f, 0.0f);
    // Leave the playhead where it stopped; the readout keeps its last value.
}

void MainWindow::StartRecording() {
    if (fPlaying || (fRecorder && fRecorder->IsRecording()))
        return;

    // Record onto the first armed audio track.
    TrackId target = kInvalidTrackId;
    for (const Track& t : fProject->Tracks())
        if (t.type == TrackType::Audio && t.armed) { target = t.id; break; }
    if (target == kInvalidTrackId) {
        std::fprintf(stderr, "MainWindow: arm a track (R) before recording\n");
        return;
    }

    char path[64];
    std::snprintf(path, sizeof(path), "take-%d.wav", ++fTakeCounter);

    fRecorder.reset(new Recorder());
    if (fRecorder->Start(path) != B_OK) {
        std::fprintf(stderr, "MainWindow: recording failed to start\n");
        fRecorder.reset();
        return;
    }
    UpdatePulse();
}

void MainWindow::StopRecording() {
    if (!fRecorder || !fRecorder->IsRecording())
        return;

    fRecorder->Stop();
    const int64_t frames = fRecorder->FramesWritten();
    const std::string path = std::string("take-") + std::to_string(fTakeCounter)
                           + ".wav";

    UpdatePulse();
    fMeter->SetLevels(0.0f, 0.0f);

    if (frames <= 0) {
        std::fprintf(stderr, "MainWindow: empty take, no clip added\n");
        fRecorder.reset();
        return;
    }

    // Find the armed track again and drop the take as a clip at the playhead.
    TrackId target = kInvalidTrackId;
    for (const Track& t : fProject->Tracks())
        if (t.type == TrackType::Audio && t.armed) { target = t.id; break; }
    if (target != kInvalidTrackId) {
        // The take is `frames` at the recorder's rate; store its length in
        // timeline (project-rate) frames.
        const double recRate = fRecorder->SampleRate();
        const double ratio = recRate > 0 ? fProject->sampleRate / recRate : 1.0;
        Clip clip;
        clip.startFrame   = fProject->transport.playhead;
        clip.lengthFrames = (int64_t)llround(frames * ratio);
        clip.sourceOffset = 0;
        clip.sourcePath   = path;
        fStack->Execute(std::make_unique<AddClipCommand>(target, clip),
                        *fProject);

        // Build the waveform envelope for the new take so it draws.
        WavSource src;
        if (src.Open(path))
            (*fPeaks)[path].Build(src);
    }

    fRecorder.reset();
    fTimeline->Invalidate();
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
    StopRecording();
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}

} // namespace daw
