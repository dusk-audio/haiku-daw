#include "MainWindow.h"

#include "TimelineView.h"
#include "MeterView.h"
#include "EffectsWindow.h"
#include "SendsWindow.h"
#include "InstrumentWindow.h"
#include "SampleBrowser.h"
#include "MixerWindow.h"
#include "../storage/BfsAttr.h"
#include "../app/AppSettings.h"
#include "RenameWindow.h"
#include "UiMetrics.h"

#include "../engine/WavSource.h"
#include "../engine/Exporter.h"
#include "../model/ProjectIO.h"
#include "../model/Commands.h"
#include "../model/RecordPlan.h"

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <File.h>
#include <FindDirectory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Path.h>
#include <Slider.h>
#include <StringView.h>
#include <TextControl.h>

#include <sys/stat.h>

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
    MSG_MASTER   = 'mvol',   // master volume slider moved
    MSG_ZOOM_IN  = 'zmin',
    MSG_ZOOM_OUT = 'zmot',
    MSG_EXPORT   = 'expt',
    MSG_EXPORT_REF = 'exrf',
    MSG_NEW_AUDIO = 'naud',
    MSG_NEW_MIDI  = 'nmid',
    MSG_NEW_BUS   = 'nbus',
    MSG_MIXER     = 'mixr',
    MSG_METRONOME = 'metr',
    MSG_IMPORT    = 'impt',
    MSG_IMPORT_REF = 'imrf',
    MSG_PASTE     = 'past',
    MSG_TEMPO     = 'tmpo',
    MSG_BUFFER    = 'bufs',
    MSG_MASTER_FX = 'mfx ',
    MSG_MON_DIM   = 'mdim',
    MSG_MON_MONO  = 'mmon',
    MSG_COUNTIN   = 'cnti',
    MSG_MONITOR_IN = 'moni',
    MSG_SHORTCUTS = 'keys',
    MSG_ZOOMFIT   = 'zfit',
    MSG_BROWSER   = 'brws',
    MSG_AUTOSAVE  = 'asav',
    MSG_RECOVER   = 'rcvr',   // deferred startup recovery check
};

// Defined below; used by MessageReceived above its definition.
static bool RecoveryPath(BPath& out);

// Sentinel "track id" the effects editor uses to target the master FX chain.
static const TrackId kMasterFxTarget = ~(TrackId)0;

static constexpr float kTransportH = 36.0f;
static constexpr bigtime_t kPulseInterval = 16000;   // ~60 Hz, microseconds

MainWindow::MainWindow(BRect frame, Project* project, CommandStack* stack,
                       PeakMap* peaks)
    : BWindow(frame, "Haiku DAW", B_TITLED_WINDOW,
              B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE),
      fProject(project), fStack(stack), fPeaks(peaks) {
    BRect bounds = Bounds();
    // Keep the tempo map's rate in sync with the project's sample rate.
    fProject->tempoMap.sampleRate = fProject->sampleRate;



    // --- Menu bar ---
    BMenuBar* menuBar = new BMenuBar(BRect(0, 0, bounds.right, 20), "menubar");
    BMenu* fileMenu = new BMenu("File");
    fileMenu->AddItem(new BMenuItem("Open" B_UTF8_ELLIPSIS, new BMessage(MSG_OPEN), 'O'));
    fileMenu->AddItem(new BMenuItem("Save" B_UTF8_ELLIPSIS, new BMessage(MSG_SAVE), 'S'));
    fileMenu->AddItem(new BMenuItem("Import Audio" B_UTF8_ELLIPSIS, new BMessage(MSG_IMPORT)));
    fileMenu->AddItem(new BMenuItem("Export WAV" B_UTF8_ELLIPSIS, new BMessage(MSG_EXPORT)));
    fileMenu->AddSeparatorItem();
    fileMenu->AddItem(new BMenuItem("Quit", new BMessage(B_QUIT_REQUESTED), 'Q'));
    menuBar->AddItem(fileMenu);
    BMenu* editMenu = new BMenu("Edit");
    editMenu->AddItem(new BMenuItem("Undo", new BMessage(MSG_UNDO), 'Z'));
    editMenu->AddItem(new BMenuItem("Redo", new BMessage(MSG_REDO), 'Z', B_SHIFT_KEY));
    editMenu->AddSeparatorItem();
    editMenu->AddItem(new BMenuItem("Paste", new BMessage(MSG_PASTE), 'V'));
    menuBar->AddItem(editMenu);
    BMenu* trackMenu = new BMenu("Track");
    trackMenu->AddItem(new BMenuItem("New Audio Track", new BMessage(MSG_NEW_AUDIO)));
    trackMenu->AddItem(new BMenuItem("New MIDI Track", new BMessage(MSG_NEW_MIDI)));
    trackMenu->AddItem(new BMenuItem("New Bus", new BMessage(MSG_NEW_BUS)));
    menuBar->AddItem(trackMenu);
    BMenu* viewMenu = new BMenu("View");
    viewMenu->AddItem(new BMenuItem("Mixer", new BMessage(MSG_MIXER)));
    fMetItem = new BMenuItem("Metronome", new BMessage(MSG_METRONOME));
    viewMenu->AddItem(fMetItem);
    viewMenu->AddItem(new BMenuItem("Master Effects" B_UTF8_ELLIPSIS,
                                    new BMessage(MSG_MASTER_FX)));
    viewMenu->AddSeparatorItem();
    fDimItem = new BMenuItem("Monitor: Dim", new BMessage(MSG_MON_DIM));
    viewMenu->AddItem(fDimItem);
    fMonoItem = new BMenuItem("Monitor: Mono", new BMessage(MSG_MON_MONO));
    viewMenu->AddItem(fMonoItem);
    viewMenu->AddSeparatorItem();
    viewMenu->AddItem(new BMenuItem("Zoom to Fit", new BMessage(MSG_ZOOMFIT), 'F'));
    viewMenu->AddItem(new BMenuItem("Keyboard Shortcuts" B_UTF8_ELLIPSIS,
                                    new BMessage(MSG_SHORTCUTS)));
    viewMenu->AddItem(new BMenuItem("Sample Browser" B_UTF8_ELLIPSIS,
                                    new BMessage(MSG_BROWSER)));
    menuBar->AddItem(viewMenu);

    // Audio > Buffer Size (latency vs xrun; applies on the next Play).
    BMenu* audioMenu = new BMenu("Audio");
    fBufMenu = new BMenu("Buffer Size");
    fBufMenu->SetRadioMode(true);
    const int bufOpts[] = { 128, 256, 512, 1024, 2048 };
    for (int n : bufOpts) {
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "%d frames (~%.1f ms)", n,
                      1000.0 * n / fProject->sampleRate);
        BMessage* m = new BMessage(MSG_BUFFER);
        m->AddInt32("frames", n);
        BMenuItem* it = new BMenuItem(lbl, m);
        if ((size_t)n == fBufferFrames) it->SetMarked(true);
        fBufMenu->AddItem(it);
    }
    audioMenu->AddItem(fBufMenu);
    audioMenu->AddSeparatorItem();
    // Count-in: metronome bars before capture begins.
    fCountInMenu = new BMenu("Count-in");
    fCountInMenu->SetRadioMode(true);
    const int ciOpts[] = { 0, 1, 2 };
    for (int n : ciOpts) {
        char lbl[24];
        if (n == 0) std::snprintf(lbl, sizeof(lbl), "Off");
        else        std::snprintf(lbl, sizeof(lbl), "%d bar%s", n, n > 1 ? "s" : "");
        BMessage* m = new BMessage(MSG_COUNTIN);
        m->AddInt32("bars", n);
        BMenuItem* it = new BMenuItem(lbl, m);
        if (n == fCountInBars) it->SetMarked(true);
        fCountInMenu->AddItem(it);
    }
    audioMenu->AddItem(fCountInMenu);
    fMonInItem = new BMenuItem("Monitor Input", new BMessage(MSG_MONITOR_IN));
    audioMenu->AddItem(fMonInItem);
    menuBar->AddItem(audioMenu);
    AddChild(menuBar);
    float menuH = menuBar->Bounds().Height();
    if (menuH < 1) menuH = 19;

    // --- Transport bar (below the menu) ---
    const float barTop = menuH + 1;
    BRect barRect(0, barTop, bounds.right, barTop + kTransportH);
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

    fTimeView = new BStringView(BRect(210, 8, 344, kTransportH - 6),
                                "time", "1.1   0:00.000");
    fTimeView->SetViewColor(ColHeader());
    fTimeView->SetHighColor(ColText());
    bar->AddChild(fTimeView);

    // Horizontal zoom buttons (keyboard +/- and arrows also work).
    BButton* zoomOut = new BButton(BRect(346, 5, 374, kTransportH - 5), "zoomout",
                                   "-", new BMessage(MSG_ZOOM_OUT));
    BButton* zoomIn  = new BButton(BRect(378, 5, 406, kTransportH - 5), "zoomin",
                                   "+", new BMessage(MSG_ZOOM_IN));
    bar->AddChild(zoomOut);
    bar->AddChild(zoomIn);

    // Master volume slider (0..150% -> gain 0..1.5), live/non-undoable.
    fMaster = new BSlider(BRect(414, 4, 538, kTransportH - 4),
                          "master", "Vol", new BMessage(MSG_MASTER),
                          0, 150, B_HORIZONTAL);
    fMaster->SetModificationMessage(new BMessage(MSG_MASTER));
    fMaster->SetValue((int32)(fProject->masterGain * 100.0f));
    bar->AddChild(fMaster);

    // Tempo (BPM) — affects the grid/snap and the metronome (next Play).
    char bpm[16];
    std::snprintf(bpm, sizeof(bpm), "%.0f", fProject->tempoBPM);
    fTempo = new BTextControl(BRect(548, 6, 664, kTransportH - 6),
                              "tempo", "BPM", bpm, new BMessage(MSG_TEMPO));
    fTempo->SetDivider(32.0f);
    bar->AddChild(fTempo);

    // Loudness readout (momentary / short-term LUFS + true peak dBTP).
    fLoudView = new BStringView(BRect(672, 8, 900, kTransportH - 6),
                                "loud", "M --  S --  TP --");
    fLoudView->SetViewColor(ColHeader());
    fLoudView->SetHighColor(ColText());
    bar->AddChild(fLoudView);

    // Master output meter, pinned to the right of the transport bar.
    fMeter = new MeterView(BRect(bounds.right - 130, 5, bounds.right - 6,
                                 kTransportH - 5));
    bar->AddChild(fMeter);

    // --- Timeline (fills the rest) ---
    BRect tlRect(0, barTop + kTransportH + 1, bounds.right, bounds.bottom);
    fTimeline = new TimelineView(tlRect, project, stack);
    fTimeline->SetPeaks(peaks);
    AddChild(fTimeline);

    // Restore persisted preferences + window layout (after the menus exist).
    LoadSettings();
    // Autosave for crash recovery; check for a leftover once the looper runs.
    fAutosave = new BMessageRunner(BMessenger(this), new BMessage(MSG_AUTOSAVE),
                                   30LL * 1000 * 1000);   // every 30 s
    PostMessage(MSG_RECOVER);
}

MainWindow::~MainWindow() {
    delete fPulse;
    delete fAutosave;
    delete fSavePanel;
    delete fOpenPanel;
    delete fExportPanel;
    delete fImportPanel;
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
            if (fRecMode) StopRecording();
            else          StartRecording();
            break;
        case kMsgSeek: {
            const Frame ph = fProject->transport.playhead;
            UpdateTimeReadout(ph);
            if (fPlaying)        // restart from the new position
                StartPlayback();
            break;
        }
        case MSG_ZOOM_IN:  fTimeline->ZoomBy(0.5); break;
        case MSG_ZOOM_OUT: fTimeline->ZoomBy(2.0); break;
        case MSG_MON_DIM:
            fMonDim = !fMonDim;
            if (fDimItem) fDimItem->SetMarked(fMonDim);
            if (fEngine) fEngine->SetMonitorDim(fMonDim);
            break;
        case MSG_MON_MONO:
            fMonMono = !fMonMono;
            if (fMonoItem) fMonoItem->SetMarked(fMonMono);
            if (fEngine) fEngine->SetMonitorMono(fMonMono);
            break;
        case MSG_COUNTIN: {
            int32 bars = 0;
            msg->FindInt32("bars", &bars);
            fCountInBars = bars;
            if (fCountInMenu)
                for (int32 i = 0; i < fCountInMenu->CountItems(); i++)
                    if (BMenuItem* it = fCountInMenu->ItemAt(i))
                        it->SetMarked(it->Message()
                            && it->Message()->FindInt32("bars") == bars);
            break;
        }
        case MSG_MONITOR_IN:
            fMonitorInput = !fMonitorInput;
            if (fMonInItem) fMonInItem->SetMarked(fMonitorInput);
            // Live toggle while a take is running.
            if (fRecorder) fRecorder->SetMonitor(fMonitorInput);
            if (fEngine)   fEngine->SetInputMonitor(fMonitorInput);
            break;
        case MSG_METRONOME:
            fMetronome = !fMetronome;
            if (fMetItem) fMetItem->SetMarked(fMetronome);
            if (fEngine) fEngine->SetMetronome(fMetronome);
            break;
        case MSG_EXPORT:
            if (!fExportPanel) {
                BMessenger to(this);
                fExportPanel = new BFilePanel(B_SAVE_PANEL, &to, NULL, 0, false,
                                              new BMessage(MSG_EXPORT_REF));
            }
            fExportPanel->Show();
            break;
        case MSG_EXPORT_REF: {
            entry_ref dir; const char* name = nullptr;
            if (msg->FindRef("directory", &dir) == B_OK
                && msg->FindString("name", &name) == B_OK) {
                BPath path(&dir);
                path.Append(name);
                StopPlayback();
                if (!ExportWav(*fProject, path.Path(), fProject->sampleRate))
                    std::fprintf(stderr, "MainWindow: export failed: %s\n",
                                 path.Path());
            }
            break;
        }
        case MSG_NEW_AUDIO:
        case MSG_NEW_MIDI:
        case MSG_NEW_BUS: {
            TrackType ty = msg->what == MSG_NEW_MIDI ? TrackType::Midi
                         : msg->what == MSG_NEW_BUS  ? TrackType::Bus
                                                     : TrackType::Audio;
            const char* pfx = ty == TrackType::Midi ? "MIDI"
                            : ty == TrackType::Bus  ? "Bus" : "Audio";
            char nm[32];
            std::snprintf(nm, sizeof(nm), "%s %d", pfx,
                          (int)fProject->Tracks().size() + 1);
            fStack->Execute(std::make_unique<AddTrackCommand>(ty, nm), *fProject);
            fTimeline->Invalidate();
            break;
        }
        case MSG_MASTER_FX: {
            BRect wr(160, 160, 460, 720);
            (new EffectsWindow(wr, fProject->masterFx, kMasterFxTarget,
                               BMessenger(this)))->Show();
            break;
        }
        case MSG_MIXER: {
            std::vector<MixerStripInfo> strips;
            for (const Track& t : fProject->Tracks())
                strips.push_back(MixerStripInfo{ (uint64)t.id, t.name,
                    t.gain, t.pan, t.muted, t.soloed, t.colorIndex });
            const float ww = 24 + (strips.size() + 1) * 90;   // + master
            BRect wr(120, 120, 120 + ww, 120 + 300);
            MixerWindow* mx = new MixerWindow(wr, strips, fProject->masterGain,
                                              BMessenger(this));
            fMixerMsgr = BMessenger(mx);
            mx->Show();
            break;
        }
        case kMsgApplyMaster:
            msg->FindFloat("gain", &fProject->masterGain);
            if (fMaster) fMaster->SetValue((int32)(fProject->masterGain * 100.0f));
            break;
        case kMsgRenameTrack: {
            int64 tid = 0; const char* name = nullptr;
            msg->FindInt64("track", &tid);
            if (msg->FindString("name", &name) == B_OK && name && name[0]) {
                fStack->Execute(std::make_unique<SetTrackNameCommand>(
                    (TrackId)tid, name), *fProject);
                fTimeline->Invalidate();
            }
            break;
        }
        case kMsgApplyMix: {
            int64 tid = 0; float g = 1, p = 0; bool mu = false, so = false;
            msg->FindInt64("track", &tid);
            msg->FindFloat("gain", &g);
            msg->FindFloat("pan", &p);
            msg->FindBool("mute", &mu);
            msg->FindBool("solo", &so);
            if (Track* t = fProject->FindTrack((TrackId)tid)) {
                t->gain = g; t->pan = p; t->muted = mu; t->soloed = so;
                fTimeline->Invalidate();
            }
            break;
        }
        case kMsgApplyFx: {
            // An EffectsWindow (its own thread) sends the edited chain here;
            // the model is mutated only on this (main) thread.
            int64 tid = 0;
            msg->FindInt64("track", &tid);
            // The chain goes to a track, or to the master when tid is the
            // master sentinel.
            std::vector<EffectDesc>* dst = nullptr;
            if ((TrackId)tid == kMasterFxTarget)
                dst = &fProject->masterFx;
            else if (Track* t = fProject->FindTrack((TrackId)tid))
                dst = &t->fx;
            if (dst) {
                dst->clear();
                int32 type = 0, epIdx = 0;
                for (int32 i = 0; msg->FindInt32("et", i, &type) == B_OK; i++) {
                    EffectDesc d;
                    d.type = (type >= 0 && type <= 7) ? (EffectType)type
                                                      : EffectType::Biquad;
                    int32 count = 0;
                    msg->FindInt32("ec", i, &count);
                    for (int32 j = 0; j < count; j++) {
                        float v = 0.0f;
                        msg->FindFloat("ep", epIdx++, &v);
                        d.params.push_back(v);
                    }
                    dst->push_back(d);
                }
                fTimeline->Invalidate();
            }
            break;
        }
        case kMsgApplySends: {
            // A SendsWindow (its own thread) posts the edited send list here.
            // Direct mutation (like kMsgApplyFx) so dragging a level slider
            // doesn't flood the undo stack. Sends take effect on the next Play.
            int64 tid = 0;
            msg->FindInt64("track", &tid);
            if (Track* t = fProject->FindTrack((TrackId)tid)) {
                t->sends.clear();
                int64 dest = 0;
                for (int32 i = 0; msg->FindInt64("sd", i, &dest) == B_OK; i++) {
                    Send s;
                    s.dest = (TrackId)dest;
                    float lvl = 1.0f; int32 pre = 0;
                    msg->FindFloat("sl", i, &lvl);
                    msg->FindInt32("sp", i, &pre);
                    s.level = lvl;
                    s.preFader = (pre != 0);
                    if (s.dest != kInvalidTrackId && s.dest != (TrackId)tid)
                        t->sends.push_back(s);
                }
                fTimeline->Invalidate();
            }
            break;
        }
        case kMsgApplyInstrument: {
            // An InstrumentWindow posts the edited voice; direct mutation (like
            // fx/sends). Applies on the next Play.
            int64 tid = 0;
            msg->FindInt64("track", &tid);
            if (Track* t = fProject->FindTrack((TrackId)tid)) {
                int32 wv = 0; float a = 0, d = 0, s = 0, r = 0;
                msg->FindInt32("wave", &wv);
                msg->FindFloat("a", &a); msg->FindFloat("d", &d);
                msg->FindFloat("s", &s); msg->FindFloat("r", &r);
                t->instrument.waveform = (wv >= 0 && wv <= 3) ? wv : 0;
                t->instrument.attack = a; t->instrument.decay = d;
                t->instrument.sustain = s; t->instrument.release = r;
                fTimeline->Invalidate();
            }
            break;
        }
        case MSG_ZOOMFIT:
            fTimeline->ZoomToFit();
            break;
        case MSG_AUTOSAVE: {
            // Save a recovery copy while there's content and we're not mid-take.
            if (!fRecMode && !fProject->Tracks().empty()) {
                BPath p;
                if (RecoveryPath(p)) ProjectIO::Save(*fProject, p.Path());
            }
            break;
        }
        case MSG_RECOVER: {
            BPath p;
            BEntry e;
            if (RecoveryPath(p) && (e.SetTo(p.Path()), e.Exists())) {
                BAlert* a = new BAlert("Recover",
                    "Unsaved work from a previous session was found. Recover it?",
                    "Discard", "Recover");
                if (a->Go() == 1) LoadFrom(p.Path());
                else              std::remove(p.Path());
            }
            break;
        }
        case MSG_BROWSER: {
            BRect wr = BWindow::Frame();
            wr.OffsetBy(40, 40);
            wr.right = wr.left + 420; wr.bottom = wr.top + 380;
            (new SampleBrowser(wr, BMessenger(this)))->Show();
            break;
        }
        case kMsgBrowserImport: {
            const char* path = nullptr;
            if (msg->FindString("path", &path) == B_OK && path)
                ImportAudio(path);
            break;
        }
        case MSG_SHORTCUTS: {
            BAlert* a = new BAlert("Keyboard Shortcuts",
                "File:  Cmd-O open   Cmd-S save   Cmd-Q quit\n"
                "Edit:  Cmd-Z undo   Cmd-Shift-Z redo   Cmd-V paste\n"
                "       Ctrl-D duplicate   Del delete selection   Esc clear\n"
                "View:  + / - zoom   F fit   arrows pan   wheel / PgUp-PgDn scroll   Home start\n"
                "Clips: drag move   right edge resize   top corners fade\n"
                "       Ctrl-drag gain   Shift free-snap   right-click menu\n"
                "Select: click / Shift-click / drag rubber-band\n"
                "Ruler: click seek   drag loop   Ctrl-drag punch   right-click tempo/meter\n"
                "Track header: M mute  S solo  R arm  route/FX/sends/auto/inst boxes",
                "OK");
            a->SetShortcut(0, B_ESCAPE);
            a->Go(NULL);   // async; non-blocking
            break;
        }
        case MSG_MASTER:
            // Live; the engine reads project.masterGain each poll (and at Load).
            fProject->masterGain = fMaster->Value() / 100.0f;
            break;
        case MSG_UNDO:
            if (fStack->CanUndo()) { fStack->Undo(*fProject); fTimeline->Invalidate(); }
            break;
        case MSG_REDO:
            if (fStack->CanRedo()) { fStack->Redo(*fProject); fTimeline->Invalidate(); }
            break;
        case MSG_PASTE:
            fTimeline->PasteAtPlayhead();
            break;
        case MSG_BUFFER: {
            int32 frames = 512;
            msg->FindInt32("frames", &frames);
            fBufferFrames = (size_t)frames;   // applied at the next Play
            break;
        }
        case MSG_TEMPO: {
            double bpm = atof(fTempo->Text());
            if (bpm < 20.0)  bpm = 20.0;
            if (bpm > 300.0) bpm = 300.0;
            fProject->tempoBPM = bpm;
            // The BPM field edits the tempo map's frame-0 (initial) tempo.
            fProject->tempoMap.sampleRate = fProject->sampleRate;
            fProject->tempoMap.SetTempoAt(0, bpm);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f", bpm);
            fTempo->SetText(buf);
            fTimeline->Invalidate();   // grid + ruler follow tempo
            // Metronome/engine pick up the new tempo on the next Play (rebuild).
            break;
        }
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
        case MSG_IMPORT:
            if (!fImportPanel) {
                BMessenger to(this);
                fImportPanel = new BFilePanel(B_OPEN_PANEL, &to, NULL, 0, false,
                                              new BMessage(MSG_IMPORT_REF));
            }
            fImportPanel->Show();
            break;
        case MSG_IMPORT_REF: {
            entry_ref ref;
            if (msg->FindRef("refs", &ref) == B_OK) {
                BPath path(&ref);
                ImportAudio(path.Path());
            }
            break;
        }
        case MSG_PULSE: {
            if (fEngine && (fPlaying || fRecMode)) {
                fEngine->UpdateMix(*fProject);   // live gain/pan/mute/solo
                const Frame ph = fEngine->Playhead();
                const Transport& tr = fProject->transport;

                if (fRecMode) {
                    // Count-in over: begin capture once we reach the record point.
                    if (fCapturePending && ph >= fRecPoint)
                        StartCapture();
                    // Loop-record: at the loop end, rewind the engine to the
                    // loop start (the recorder keeps capturing across the seam).
                    if (fLoopRecord && tr.loopEnabled && ph >= tr.loopEnd) {
                        fProject->transport.playhead = tr.loopStart;
                        StartRecordEngine(tr.loopStart);
                        break;
                    }
                    const bool capturing = fRecorder && fRecorder->IsRecording();
                    fTimeline->SetPlayhead(ph);
                    UpdateTimeReadout(ph);
                    if (capturing) {
                        fTimeline->SetRecording(true, fRecStart, ph - fRecStart);
                        fMeter->SetLevels(fRecorder->PeakL(), fRecorder->PeakR());
                    } else {
                        fMeter->SetLevels(fEngine->PeakL(), fEngine->PeakR());
                    }
                    PushTrackPeaks();
                    break;
                }

                // Playback loop: restart at the loop start past the loop end.
                if (tr.loopEnabled && tr.loopEnd > tr.loopStart
                    && ph >= tr.loopEnd) {
                    fProject->transport.playhead = tr.loopStart;
                    StartPlayback();
                    break;
                }
                fTimeline->SetPlayhead(ph);
                UpdateTimeReadout(ph);
                fMeter->SetLevels(fEngine->PeakL(), fEngine->PeakR());
                UpdateLoudnessReadout(fEngine->LufsMomentary(),
                                      fEngine->LufsShort(),
                                      fEngine->TruePeakDb());
                PushTrackPeaks();
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
    const bool need = fPlaying || fRecMode
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
    if (fRecMode)
        return;   // recording runs its own engine (overdub)
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
    Frame minEnd  = looping ? tr.loopEnd : 0;   // run through silence to loop end
    // Metronome with no audio content: run the transport (10 min) so the click
    // plays over silence rather than the engine reporting "nothing to play".
    if (fMetronome && ProjectEndFrame(*fProject) == 0) {
        const Frame ten = (Frame)(fProject->sampleRate * 600.0);
        if (ten > minEnd) minEnd = ten;
    }
    fEngine.reset(new Engine());
    fEngine->SetBufferFrames(fBufferFrames);
    if (fEngine->Load(*fProject, start, minEnd) != B_OK) {
        std::fprintf(stderr, "MainWindow: nothing to play\n");
        fEngine.reset();
        return;
    }
    fEngine->Start();
    fEngine->SetMetronome(fMetronome);
    fEngine->SetMonitorDim(fMonDim);
    fEngine->SetMonitorMono(fMonMono);
    fPlaying = true;
    UpdatePulse();
}

void MainWindow::StopPlayback() {
    if (fEngine)
        fEngine->Stop();
    fPlaying = false;
    UpdatePulse();
    fMeter->SetLevels(0.0f, 0.0f);
    fTimeline->ClearTrackPeaks();
    UpdateLoudnessReadout(Loudness::kSilenceLufs, Loudness::kSilenceLufs,
                          Loudness::kSilenceDb);
    // Leave the playhead where it stopped; the readout keeps its last value.
}

// Start the playback engine from `engineStart` for overdub monitoring during a
// record pass (existing tracks + metronome play while capturing). Returns true
// on success. Runs a long transport so it keeps advancing through silence.
bool MainWindow::StartRecordEngine(Frame engineStart) {
    const Frame tenMin = (Frame)(fProject->sampleRate * 600.0);
    fEngine.reset(new Engine());
    fEngine->SetBufferFrames(fBufferFrames);
    if (fEngine->Load(*fProject, engineStart, engineStart + tenMin) != B_OK) {
        fEngine.reset();
        return false;
    }
    fEngine->Start();
    // Count-in needs the click; force it on during record if a count-in is set.
    fEngine->SetMetronome(fMetronome || fCountInBars > 0);
    fEngine->SetMonitorDim(fMonDim);
    fEngine->SetMonitorMono(fMonMono);
    // Re-attach input monitoring across an engine restart (loop-record seam).
    if (fRecorder) {
        fEngine->SetMonitorSource(fRecorder.get());
        fEngine->SetInputMonitor(fMonitorInput);
    }
    return true;
}

void MainWindow::StartCapture() {
    // Write takes into the project's directory (a self-contained bundle) when
    // the project has been saved; otherwise the working directory.
    char name[64];
    std::snprintf(name, sizeof(name), "take-%d.wav", ++fTakeCounter);
    fTakePath = fTakeDir.empty() ? std::string(name)
                                 : fTakeDir + "/" + name;
    fRecorder.reset(new Recorder());
    if (fRecorder->Start(fTakePath.c_str()) != B_OK) {
        std::fprintf(stderr, "MainWindow: recording failed to start\n");
        fRecorder.reset();
        --fTakeCounter;
        return;
    }
    // Wire input monitoring: the engine mixes the recorder's live input (only
    // if the input rate matches the output rate).
    fRecorder->SetMonitor(fMonitorInput);
    if (fEngine) {
        fEngine->SetMonitorSource(fRecorder.get());
        fEngine->SetInputMonitor(fMonitorInput);
    }
    fRecStart = fRecPoint;      // clip origin = record point
    fCapturePending = false;
}

void MainWindow::StartRecording() {
    if (fRecMode || (fRecorder && fRecorder->IsRecording()))
        return;
    if (fPlaying) StopPlayback();

    // Record onto every armed audio track (one input take, dropped on each).
    fRecTracks.clear();
    for (const Track& t : fProject->Tracks())
        if (t.type == TrackType::Audio && t.armed)
            fRecTracks.push_back(t.id);
    if (fRecTracks.empty()) {
        std::fprintf(stderr, "MainWindow: arm a track (R) before recording\n");
        return;
    }

    // Loop-record when a loop range is set: capture aligns to the loop start
    // and each pass becomes a stacked take.
    const Transport& tr = fProject->transport;
    fLoopRecord = tr.loopEnabled && tr.loopEnd > tr.loopStart;

    // Record point = loop start (loop-record) or the playhead. A count-in plays
    // the engine (existing tracks + click) for N bars leading up to it.
    fProject->tempoMap.sampleRate = fProject->sampleRate;
    fRecPoint = fLoopRecord ? tr.loopStart : fProject->transport.playhead;
    const Frame countIn = CountInFrames(fProject->tempoMap, fRecPoint,
                                        fCountInBars);
    const Frame engineStart = (fRecPoint > countIn) ? fRecPoint - countIn : 0;

    if (!StartRecordEngine(engineStart)) {
        std::fprintf(stderr, "MainWindow: record engine failed to start\n");
        return;
    }
    fRecMode = true;
    fCapturePending = (countIn > 0);
    if (!fCapturePending)
        StartCapture();         // no count-in: capture immediately
    UpdatePulse();
}

void MainWindow::StopRecording() {
    if (!fRecMode)
        return;

    const bool captured = fRecorder && fRecorder->IsRecording();
    int64_t frames = 0;
    double  recRate = fProject->sampleRate;
    if (captured) {
        fRecorder->Stop();
        frames  = fRecorder->FramesWritten();
        recRate = fRecorder->SampleRate();
    }
    const std::string path = fTakePath;   // the file StartCapture opened
    std::vector<TrackId> targets = fRecTracks;

    // Stop the overdub engine + live REC region, end the poll.
    if (fEngine) fEngine->Stop();
    fRecMode = false;
    fCapturePending = false;
    fTimeline->SetRecording(false, 0, 0);
    fRecTracks.clear();
    UpdatePulse();
    fMeter->SetLevels(0.0f, 0.0f);

    if (!captured || frames <= 0 || targets.empty()) {
        std::fprintf(stderr, "MainWindow: empty take, no clip added\n");
        fRecorder.reset();
        return;
    }

    // Build the waveform envelope once; the take drops on every armed track.
    WavSource src;
    if (src.Open(path))
        (*fPeaks)[path].Build(src);

    // lengthFrames is timeline (project-rate) frames, converted from the
    // recorder-rate frame count. `ratio` = project/record rate; a source-frame
    // offset is a timeline offset divided by ratio (the capture file is at the
    // record rate).
    const double ratio = recRate > 0 ? fProject->sampleRate / recRate : 1.0;
    const Frame captureLen = (Frame)llround(frames * ratio);
    auto toSourceOffset = [&](Frame timelineOffset) -> Frame {
        return (Frame)llround(timelineOffset / ratio);
    };

    const Transport& tr = fProject->transport;

    // Loop-record: split the linear capture into one take per loop pass and
    // stack them as a take group on each armed track (last pass active).
    if (fLoopRecord) {
        const std::vector<TakeRegion> takes =
            LoopTakes(tr.loopStart, tr.loopEnd, captureLen);
        fLoopRecord = false;
        if (takes.empty()) { fRecorder.reset(); return; }
        for (TrackId target : targets) {
            const int group = ++fTakeGroup;
            auto macro = std::make_unique<MacroCommand>("Loop Takes");
            for (size_t k = 0; k < takes.size(); k++) {
                Clip clip;
                clip.startFrame   = takes[k].startFrame;
                clip.lengthFrames = takes[k].lengthFrames;
                clip.sourceOffset = toSourceOffset(takes[k].sourceOffset);
                clip.sourcePath   = path;
                clip.takeGroup    = group;
                clip.takeActive   = (k + 1 == takes.size());  // last pass active
                macro->Add(std::make_unique<AddClipCommand>(target, clip));
            }
            fStack->Execute(std::move(macro), *fProject);
        }
        fRecorder.reset();
        fTimeline->Invalidate();
        return;
    }

    // Punch: trim the take to the punch range (non-destructive — the clip just
    // references a sub-span of the captured file).
    TakeRegion region;
    region.startFrame = fRecStart;
    region.sourceOffset = 0;
    region.lengthFrames = captureLen;
    if (tr.punchEnabled) {
        if (!PunchedTake(fRecStart, captureLen, tr.punchIn, tr.punchOut,
                         &region)) {
            std::fprintf(stderr, "MainWindow: take outside punch range, discarded\n");
            fRecorder.reset();
            return;
        }
    }

    for (TrackId target : targets) {
        Clip clip;
        clip.startFrame   = region.startFrame;
        clip.lengthFrames = region.lengthFrames;
        clip.sourceOffset = toSourceOffset(region.sourceOffset);
        clip.sourcePath   = path;
        fStack->Execute(std::make_unique<AddClipCommand>(target, clip), *fProject);
    }

    fRecorder.reset();
    fTimeline->Invalidate();
}

// The directory portion of a path (empty if none), for bundling takes.
static std::string DirOfPath(const char* path) {
    std::string p(path ? path : "");
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

void MainWindow::SaveTo(const char* path) {
    if (!ProjectIO::Save(*fProject, path)) {
        std::fprintf(stderr, "MainWindow: save failed: %s\n", path);
        return;
    }
    fTakeDir = DirOfPath(path);   // new takes land beside the project
    fLastDir = fTakeDir;
}

void MainWindow::LoadFrom(const char* path) {
    StopPlayback();
    StopRecording();
    if (!ProjectIO::Load(*fProject, path)) {
        std::fprintf(stderr, "MainWindow: load failed: %s\n", path);
        return;
    }
    fTakeDir = DirOfPath(path);
    fLastDir = fTakeDir;
    fStack->Clear();          // history from the previous project is invalid
    RebuildPeaks();           // waveform envelopes for the loaded clips
    fMaster->SetValue((int32)(fProject->masterGain * 100.0f));   // sync slider
    fTimeline->SetProject(fProject);
    fTimeline->SetPlayhead(fProject->transport.playhead);
    UpdateTimeReadout(fProject->transport.playhead);
    fTimeline->Invalidate();
}

void MainWindow::ImportAudio(const char* path) {
    WavSource src;
    if (!src.Open(path)) {
        std::fprintf(stderr, "MainWindow: cannot import '%s'\n", path);
        return;
    }
    // Target the first audio track; create one if the project has none.
    TrackId tid = kInvalidTrackId;
    for (const Track& t : fProject->Tracks())
        if (t.type == TrackType::Audio) { tid = t.id; break; }
    if (tid == kInvalidTrackId) {
        auto add = std::make_unique<AddTrackCommand>(TrackType::Audio, "Audio 1");
        AddTrackCommand* ap = add.get();
        fStack->Execute(std::move(add), *fProject);
        tid = ap->CreatedId();
    }

    const double srcRate = src.FrameRate();
    const double ratio = srcRate > 0 ? fProject->sampleRate / srcRate : 1.0;
    Clip clip;
    clip.startFrame   = fProject->transport.playhead;
    clip.lengthFrames = (int64_t)llround(src.TotalFrames() * ratio);
    clip.sourceOffset = 0;
    clip.sourcePath   = path;
    fStack->Execute(std::make_unique<AddClipCommand>(tid, clip), *fProject);

    // Tag the file with its duration (BFS attribute) so the sample browser's
    // BQuery can find/sort it. BPM/Key are user-set in the browser.
    if (srcRate > 0) {
        EnsureDawIndexes(path);
        WriteAttrFloat(path, kAttrDuration, (float)(src.TotalFrames() / srcRate));
    }

    (*fPeaks)[path].Build(src);   // waveform envelope (src cursor is at start)
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

// Resolve ~/config/settings/HaikuDAW/settings, creating the dir if needed.
static bool SettingsPath(BPath& out) {
    BPath p;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &p) != B_OK) return false;
    p.Append("HaikuDAW");
    mkdir(p.Path(), 0755);   // ignore EEXIST
    p.Append("settings");
    out = p;
    return true;
}

// The crash-recovery autosave file (settings dir). A leftover after startup
// means the last session didn't exit cleanly.
static bool RecoveryPath(BPath& out) {
    BPath p;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &p) != B_OK) return false;
    p.Append("HaikuDAW");
    mkdir(p.Path(), 0755);
    p.Append("recovery.dawproj");
    out = p;
    return true;
}

// Mark the radio item in `menu` whose message's int32 `field` equals `value`.
static void MarkRadio(BMenu* menu, const char* field, int32 value) {
    if (!menu) return;
    for (int32 i = 0; i < menu->CountItems(); i++)
        if (BMenuItem* it = menu->ItemAt(i))
            it->SetMarked(it->Message()
                          && it->Message()->FindInt32(field) == value);
}

void MainWindow::LoadSettings() {
    BPath p;
    if (!SettingsPath(p)) return;
    BFile f(p.Path(), B_READ_ONLY);
    if (f.InitCheck() != B_OK) return;
    off_t sz = 0;
    if (f.GetSize(&sz) != B_OK || sz <= 0 || sz > 65536) return;
    std::string text;
    text.resize((size_t)sz);
    if (f.Read(&text[0], (size_t)sz) != (ssize_t)sz) return;

    AppSettings s;
    s.bufferFrames = (int)fBufferFrames;
    s.countInBars  = fCountInBars;
    s.metronome    = fMetronome;
    s.monitorInput = fMonitorInput;
    if (!s.Deserialize(text)) return;

    fBufferFrames = (size_t)s.bufferFrames;
    fCountInBars  = s.countInBars;
    fMetronome    = s.metronome;
    fMonitorInput = s.monitorInput;
    fLastDir      = s.lastDir;
    MarkRadio(fBufMenu, "frames", (int32)fBufferFrames);
    MarkRadio(fCountInMenu, "bars", fCountInBars);
    if (fMetItem)   fMetItem->SetMarked(fMetronome);
    if (fMonInItem) fMonInItem->SetMarked(fMonitorInput);
    // Restore the window frame (clamped to something sane).
    if (s.winR - s.winL > 320 && s.winB - s.winT > 240) {
        MoveTo(s.winL, s.winT);
        ResizeTo(s.winR - s.winL, s.winB - s.winT);
    }
}

void MainWindow::SaveSettings() {
    AppSettings s;
    s.bufferFrames = (int)fBufferFrames;
    s.countInBars  = fCountInBars;
    s.metronome    = fMetronome;
    s.monitorInput = fMonitorInput;
    s.lastDir      = fLastDir;
    const BRect fr = BWindow::Frame();
    s.winL = fr.left; s.winT = fr.top; s.winR = fr.right; s.winB = fr.bottom;

    BPath p;
    if (!SettingsPath(p)) return;
    BFile f(p.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    if (f.InitCheck() != B_OK) return;
    const std::string t = s.Serialize();
    f.Write(t.data(), t.size());
}

void MainWindow::UpdateTimeReadout(Frame playhead) {
    const double rate = fEngine ? fEngine->OutputRate() : fProject->sampleRate;
    const double sec  = rate > 0 ? playhead / rate : 0.0;
    const int    mins = static_cast<int>(sec / 60.0);
    const double rem  = sec - mins * 60.0;
    // Bar:beat from the tempo map alongside min:sec.
    fProject->tempoMap.sampleRate = fProject->sampleRate;
    int bar = 1, beat = 1;
    fProject->tempoMap.BarBeat(playhead, &bar, &beat);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d.%d   %d:%06.3f", bar, beat, mins, rem);
    fTimeView->SetText(buf);
}

void MainWindow::PushTrackPeaks() {
    if (!fEngine) return;
    std::map<TrackId, std::pair<float, float>> tp;
    for (const Track& t : fProject->Tracks())
        tp[t.id] = { fEngine->TrackPeakL(t.id), fEngine->TrackPeakR(t.id) };
    fTimeline->SetTrackPeaks(tp);

    // Also feed the mixer window (its own looper) if one is open.
    if (fMixerMsgr.IsValid()) {
        BMessage m(kMsgMixPeaks);
        for (const auto& kv : tp) {
            m.AddInt64("tid", (int64)kv.first);
            m.AddFloat("pl", kv.second.first);
            m.AddFloat("pr", kv.second.second);
        }
        m.AddFloat("mpl", fEngine->PeakL());
        m.AddFloat("mpr", fEngine->PeakR());
        fMixerMsgr.SendMessage(&m);
    }
}

void MainWindow::UpdateLoudnessReadout(float momLufs, float shortLufs,
                                       float truePeakDb) {
    if (!fLoudView) return;
    // Below the meter's floor reads as "--" rather than a huge negative number.
    auto fmt = [](char* dst, size_t n, const char* tag, float v, float floor) {
        if (v <= floor + 0.5f) std::snprintf(dst, n, "%s --", tag);
        else                   std::snprintf(dst, n, "%s %.1f", tag, v);
    };
    char m[24], s[24], tp[24], buf[80];
    fmt(m,  sizeof(m),  "M",  momLufs,    Loudness::kSilenceLufs);
    fmt(s,  sizeof(s),  "S",  shortLufs,  Loudness::kSilenceLufs);
    fmt(tp, sizeof(tp), "TP", truePeakDb, Loudness::kSilenceDb);
    std::snprintf(buf, sizeof(buf), "%s  %s  %s dBTP", m, s, tp);
    fLoudView->SetText(buf);
}

bool MainWindow::QuitRequested() {
    StopPlayback();
    StopRecording();
    SaveSettings();
    // Clean exit: drop the recovery file so next launch doesn't offer it.
    BPath rp;
    if (RecoveryPath(rp)) std::remove(rp.Path());
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}

} // namespace daw
