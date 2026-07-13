#include "MainWindow.h"

#include "TimelineView.h"
#include "MeterView.h"
#include "UiMetrics.h"

#include "../engine/WavSource.h"
#include "../model/ProjectIO.h"

#include <Application.h>
#include <Button.h>
#include <Entry.h>
#include <FilePanel.h>
#include <MessageRunner.h>
#include <Path.h>
#include <StringView.h>

#include <cmath>
#include <cstdio>

namespace daw {

enum {
    MSG_PLAY  = 'play',
    MSG_STOP  = 'stop',
    MSG_REC   = 'rec ',
    MSG_PULSE = 'puls',
    MSG_UNDO  = 'undo',
    MSG_REDO  = 'redo',
    MSG_SAVE  = 'save',
    MSG_OPEN  = 'open',
    MSG_SAVE_REF = 'svrf',   // from the save file panel
    MSG_OPEN_REF = 'oprf',   // from the open file panel
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

    BButton* save = new BButton(BRect(478, 5, 542, kTransportH - 5), "save",
                                "Save", new BMessage(MSG_SAVE));
    BButton* open = new BButton(BRect(546, 5, 610, kTransportH - 5), "open",
                                "Open", new BMessage(MSG_OPEN));
    bar->AddChild(save);
    bar->AddChild(open);

    // Master output meter, pinned to the right of the transport bar.
    fMeter = new MeterView(BRect(bounds.right - 130, 5, bounds.right - 6,
                                 kTransportH - 5));
    bar->AddChild(fMeter);

    // Keyboard: Cmd-Z / Cmd-Shift-Z / Cmd-S / Cmd-O.
    AddShortcut('Z', B_COMMAND_KEY, new BMessage(MSG_UNDO));
    AddShortcut('Z', B_COMMAND_KEY | B_SHIFT_KEY, new BMessage(MSG_REDO));
    AddShortcut('S', B_COMMAND_KEY, new BMessage(MSG_SAVE));
    AddShortcut('O', B_COMMAND_KEY, new BMessage(MSG_OPEN));

    // --- Timeline (fills the rest) ---
    BRect tlRect(0, kTransportH + 1, bounds.right, bounds.bottom);
    fTimeline = new TimelineView(tlRect, project, stack);
    fTimeline->SetPeaks(peaks);
    AddChild(fTimeline);
}

MainWindow::~MainWindow() {
    delete fPulse;
    delete fSavePanel;
    delete fOpenPanel;
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
        case MSG_SAVE:
            if (!fSavePanel) {
                BMessenger to(this);
                fSavePanel = new BFilePanel(B_SAVE_PANEL, &to, NULL, 0, false,
                                            new BMessage(MSG_SAVE_REF));
            }
            fSavePanel->Show();
            break;
        case MSG_OPEN:
            if (!fOpenPanel) {
                BMessenger to(this);
                fOpenPanel = new BFilePanel(B_OPEN_PANEL, &to, NULL, 0, false,
                                            new BMessage(MSG_OPEN_REF));
            }
            fOpenPanel->Show();
            break;
        case MSG_SAVE_REF: {
            entry_ref dir; const char* name = nullptr;
            if (msg->FindRef("directory", &dir) == B_OK
                && msg->FindString("name", &name) == B_OK) {
                BPath path(&dir);
                path.Append(name);
                SaveTo(path.Path());
            }
            break;
        }
        case MSG_OPEN_REF: {
            entry_ref ref;
            if (msg->FindRef("refs", &ref) == B_OK) {
                BPath path(&ref);
                LoadFrom(path.Path());
            }
            break;
        }
        case MSG_PULSE: {
            if (fRecorder && fRecorder->IsRecording()) {
                fMeter->SetLevels(fRecorder->PeakL(), fRecorder->PeakR());
                // Advance the playhead + grow the REC block from frames
                // captured so far (converted recorder-rate -> timeline).
                const double recRate = fRecorder->SampleRate();
                if (recRate > 0) {
                    const double ratio = fProject->sampleRate / recRate;
                    const Frame len = (Frame)(fRecorder->FramesWritten() * ratio);
                    const Frame pos = fRecStart + len;
                    fTimeline->SetRecording(true, fRecStart, len);
                    fTimeline->SetPlayhead(pos);
                    UpdateTimeReadout(pos);
                }
            } else if (fPlaying && fEngine) {
                fEngine->UpdateMix(*fProject);   // live gain/pan/mute/solo
                const Frame ph = fEngine->Playhead();
                const Transport& tr = fProject->transport;
                // Loop: when the playhead passes the loop end, restart at the
                // loop start (rebuild-on-play seek; a small gap at the seam).
                if (tr.loopEnabled && tr.loopEnd > tr.loopStart
                    && ph >= tr.loopEnd) {
                    fProject->transport.playhead = tr.loopStart;
                    StartPlayback();
                    break;
                }
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

// Latest frame any clip or note reaches in the project (timeline frames).
static Frame ProjectEndFrame(const Project& p) {
    Frame end = 0;
    for (const Track& t : p.Tracks()) {
        for (const Clip& c : t.clips)
            if (c.startFrame + c.lengthFrames > end) end = c.startFrame + c.lengthFrames;
        for (const MidiNote& n : t.notes)
            if (n.startFrame + n.lengthFrames > end) end = n.startFrame + n.lengthFrames;
    }
    return end;
}

void MainWindow::StartPlayback() {
    if (fRecorder && fRecorder->IsRecording())
        return;   // no play-while-record in this milestone
    // Rebuild the engine from the current model each time (RT-safe: no live
    // mutation of a running graph). Playback begins at the current playhead;
    // if it's already at/after the end (and not looping), rewind first.
    const Transport& tr = fProject->transport;
    const bool looping = tr.loopEnabled && tr.loopEnd > tr.loopStart;
    if (!looping && fProject->transport.playhead >= ProjectEndFrame(*fProject)) {
        fProject->transport.playhead = 0;
        fTimeline->SetPlayhead(0);
    }
    // When looping, begin at the loop start unless already inside the loop.
    if (looping && (fProject->transport.playhead < tr.loopStart
                    || fProject->transport.playhead >= tr.loopEnd)) {
        fProject->transport.playhead = tr.loopStart;
        fTimeline->SetPlayhead(tr.loopStart);
    }
    const Frame start   = fProject->transport.playhead;
    const Frame minEnd  = looping ? tr.loopEnd : 0;   // run through silence to loop end
    fEngine.reset(new Engine());
    if (fEngine->Load(*fProject, start, minEnd) != B_OK) {
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

    // Record onto every armed audio track (one input take, dropped on each).
    fRecTracks.clear();
    for (const Track& t : fProject->Tracks())
        if (t.type == TrackType::Audio && t.armed)
            fRecTracks.push_back(t.id);
    if (fRecTracks.empty()) {
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
    fRecStart = fProject->transport.playhead;
    UpdatePulse();
}

void MainWindow::StopRecording() {
    if (!fRecorder || !fRecorder->IsRecording())
        return;

    fRecorder->Stop();
    const int64_t frames = fRecorder->FramesWritten();
    const double  recRate = fRecorder->SampleRate();
    const std::string path = std::string("take-") + std::to_string(fTakeCounter)
                           + ".wav";
    std::vector<TrackId> targets = fRecTracks;

    // Clear the live REC region and stop the poll.
    fTimeline->SetRecording(false, 0, 0);
    fRecTracks.clear();
    UpdatePulse();
    fMeter->SetLevels(0.0f, 0.0f);

    if (frames <= 0 || targets.empty()) {
        std::fprintf(stderr, "MainWindow: empty take, no clip added\n");
        fRecorder.reset();
        return;
    }

    // Build the waveform envelope once; the take drops on every armed track.
    WavSource src;
    if (src.Open(path))
        (*fPeaks)[path].Build(src);

    // lengthFrames is timeline (project-rate) frames, converted from the
    // recorder-rate frame count.
    const double ratio = recRate > 0 ? fProject->sampleRate / recRate : 1.0;
    for (TrackId target : targets) {
        Clip clip;
        clip.startFrame   = fRecStart;
        clip.lengthFrames = (int64_t)llround(frames * ratio);
        clip.sourceOffset = 0;
        clip.sourcePath   = path;
        fStack->Execute(std::make_unique<AddClipCommand>(target, clip), *fProject);
    }

    fRecorder.reset();
    fTimeline->Invalidate();
}

void MainWindow::SaveTo(const char* path) {
    if (!ProjectIO::Save(*fProject, path))
        std::fprintf(stderr, "MainWindow: save failed: %s\n", path);
}

void MainWindow::LoadFrom(const char* path) {
    StopPlayback();
    StopRecording();
    if (!ProjectIO::Load(*fProject, path)) {
        std::fprintf(stderr, "MainWindow: load failed: %s\n", path);
        return;
    }
    fStack->Clear();          // history from the previous project is invalid
    RebuildPeaks();           // waveform envelopes for the loaded clips
    fTimeline->SetProject(fProject);
    fTimeline->SetPlayhead(fProject->transport.playhead);
    UpdateTimeReadout(fProject->transport.playhead);
    fTimeline->Invalidate();
}

void MainWindow::RebuildPeaks() {
    fPeaks->clear();
    for (const Track& t : fProject->Tracks())
        for (const Clip& c : t.clips) {
            if (c.sourcePath.empty() || fPeaks->count(c.sourcePath))
                continue;
            WavSource src;
            if (src.Open(c.sourcePath))
                (*fPeaks)[c.sourcePath].Build(src);
        }
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
