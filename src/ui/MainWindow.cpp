#include "MainWindow.h"

#include "TimelineView.h"
#include "InspectorView.h"
#include "TransportBar.h"
#include "MeterView.h"
#include "EffectsWindow.h"
#include "SendsWindow.h"
#include "InstrumentWindow.h"
#include "PianoRoll.h"
#include "SampleBrowser.h"
#include "MixerWindow.h"
#include "../storage/BfsAttr.h"
#include "../app/AppSettings.h"
#include "RenameWindow.h"
#include "UiMetrics.h"

#include "../engine/WavSource.h"
#include "../engine/WavWriter.h"
#include "../engine/Exporter.h"
#include "../engine/Resampler.h"
#include "../model/ProjectIO.h"
#include "../model/Commands.h"
#include "../model/RegionOps.h"
#include "../model/SmfIO.h"
#include "../model/RecordPlan.h"

#include <Alert.h>
#include <Application.h>
#include <OS.h>   // system_time() for MIDI event timestamping
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
#include <String.h>
#include <StringView.h>
#include <TextControl.h>

#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <set>

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
    MSG_IMPORT_MIDI     = 'imid',
    MSG_IMPORT_MIDI_REF = 'imdr',
    MSG_EXPORT_MIDI     = 'emid',
    MSG_EXPORT_MIDI_REF = 'emdr',
    MSG_FOLLOW    = 'folw',   // toggle: chase the playhead
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
    fileMenu->AddItem(new BMenuItem("Import MIDI" B_UTF8_ELLIPSIS, new BMessage(MSG_IMPORT_MIDI)));
    fileMenu->AddItem(new BMenuItem("Export WAV" B_UTF8_ELLIPSIS, new BMessage(MSG_EXPORT)));
    fileMenu->AddItem(new BMenuItem("Export MIDI" B_UTF8_ELLIPSIS, new BMessage(MSG_EXPORT_MIDI)));
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
    fFollowItem = new BMenuItem("Follow Playhead", new BMessage(MSG_FOLLOW));
    fFollowItem->SetMarked(true);   // chase on by default
    viewMenu->AddItem(fFollowItem);
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
    fTransport = new TransportBar(barRect, BMessenger(this),
                                  MSG_PLAY, MSG_STOP, MSG_REC,
                                  MSG_ZOOM_OUT, MSG_ZOOM_IN);
    BView* bar = fTransport;
    AddChild(bar);

    BFont lcdFont(be_bold_font);
    lcdFont.SetSize(15.0f);
    fTimeView = new BStringView(BRect(218, 6, 354, kTransportH - 5),
                                "time", "1.1   0:00.000");
    fTimeView->SetViewColor(ColLcd());
    fTimeView->SetHighColor(ColLcdText());
    fTimeView->SetFont(&lcdFont);
    fTimeView->SetAlignment(B_ALIGN_CENTER);
    bar->AddChild(fTimeView);
    // (Zoom -/+ buttons are drawn by the TransportBar at x366..420.)

    // "Vol" label + master volume slider (0..150% -> gain 0..1.5).
    BStringView* volLbl = new BStringView(BRect(430, 8, 460, kTransportH - 6),
                                          "vollbl", "Vol");
    volLbl->SetViewColor(ColChrome());
    volLbl->SetHighColor(ColText());
    bar->AddChild(volLbl);
    fMaster = new BSlider(BRect(462, 4, 588, kTransportH - 4),
                          "master", NULL, new BMessage(MSG_MASTER),
                          0, 150, B_HORIZONTAL);
    fMaster->SetModificationMessage(new BMessage(MSG_MASTER));
    fMaster->SetValue((int32)(fProject->masterGain * 100.0f));
    fMaster->SetViewColor(ColChrome());
    fMaster->SetLowColor(ColChrome());
    rgb_color fill = ColAccent();
    fMaster->UseFillColor(true, &fill);
    fMaster->SetBarColor(Rgb(20, 22, 26));
    bar->AddChild(fMaster);

    // "BPM" label + tempo field (light field for legibility; affects grid/snap
    // + metronome on the next Play).
    BStringView* bpmLbl = new BStringView(BRect(602, 8, 636, kTransportH - 6),
                                          "bpmlbl", "BPM");
    bpmLbl->SetViewColor(ColChrome());
    bpmLbl->SetHighColor(ColText());
    bar->AddChild(bpmLbl);
    char bpm[16];
    std::snprintf(bpm, sizeof(bpm), "%.0f", fProject->tempoBPM);
    fTempo = new BTextControl(BRect(638, 6, 704, kTransportH - 6),
                              "tempo", NULL, bpm, new BMessage(MSG_TEMPO));
    fTempo->SetDivider(0.0f);
    bar->AddChild(fTempo);

    // Loudness readout (momentary / short-term LUFS + true peak dBTP).
    fLoudView = new BStringView(BRect(722, 8, 858, kTransportH - 6),
                                "loud", "M -- S -- TP --");
    fLoudView->SetViewColor(ColChrome());
    fLoudView->SetHighColor(ColText());
    bar->AddChild(fLoudView);

    // Master output meter, pinned to the right of the transport bar.
    fMeter = new MeterView(BRect(bounds.right - 130, 5, bounds.right - 6,
                                 kTransportH - 5));
    bar->AddChild(fMeter);

    // --- Inspector column (left) + timeline (fills the rest) ---
    const float contentTop = barTop + kTransportH + 1;
    BRect inspRect(0, contentTop, kInspectorWidth, bounds.bottom);
    fInspector = new InspectorView(inspRect, project, stack);
    AddChild(fInspector);

    BRect tlRect(kInspectorWidth + 1, contentTop, bounds.right, bounds.bottom);
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
    delete fMidiImportPanel;
    delete fMidiExportPanel;
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
        case kMsgMonitorRefresh:   // arming / input changed in the timeline
            UpdateMidiMonitor();
            break;
        case kMsgTrackSelected: {  // point the inspector at the clicked track
            int64 tid = 0;
            msg->FindInt64("track", &tid);
            if (fInspector) fInspector->SetTrack((TrackId)tid);
            break;
        }
        case kMsgUiRefresh:        // a track edit: keep both panes consistent
            if (fInspector) fInspector->Invalidate();
            if (fTimeline)  fTimeline->Invalidate();
            break;
        case kMsgCycleAuto:        // inspector Auto button -> cycle timeline mode
            if (fInspector && fTimeline)
                fTimeline->CycleAuto(fInspector->SelectedTrack());
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
            if (fTimeline)  fTimeline->SetMonitorInput(fMonitorInput);  // lane "I" lamp
            // Live toggle while a take is running.
            if (fRecorder) fRecorder->SetMonitor(fMonitorInput);
            if (fEngine)   fEngine->SetInputMonitor(fMonitorInput);
            break;
        case MSG_METRONOME:
            fMetronome = !fMetronome;
            if (fMetItem) fMetItem->SetMarked(fMetronome);
            if (fEngine) fEngine->SetMetronome(fMetronome);
            break;
        case MSG_FOLLOW: {
            const bool on = !(fFollowItem && fFollowItem->IsMarked());
            if (fFollowItem) fFollowItem->SetMarked(on);
            fTimeline->SetFollow(on);
            break;
        }
        case kMsgTransportToggle:   // spacebar
            if (fPlaying || fRecMode) { StopPlayback(); StopRecording(); }
            else                        StartPlayback();
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
        case MSG_IMPORT_MIDI:
            if (!fMidiImportPanel) {
                BMessenger to(this);
                fMidiImportPanel = new BFilePanel(B_OPEN_PANEL, &to, NULL, 0,
                    false, new BMessage(MSG_IMPORT_MIDI_REF));
            }
            fMidiImportPanel->Show();
            break;
        case MSG_IMPORT_MIDI_REF: {
            entry_ref ref;
            if (msg->FindRef("refs", &ref) == B_OK) {
                BPath path(&ref);
                ImportMidi(path.Path());
            }
            break;
        }
        case MSG_EXPORT_MIDI:
            if (!fMidiExportPanel) {
                BMessenger to(this);
                fMidiExportPanel = new BFilePanel(B_SAVE_PANEL, &to, NULL, 0,
                    false, new BMessage(MSG_EXPORT_MIDI_REF));
            }
            fMidiExportPanel->Show();
            break;
        case MSG_EXPORT_MIDI_REF: {
            entry_ref dir; const char* name = nullptr;
            if (msg->FindRef("directory", &dir) == B_OK
                && msg->FindString("name", &name) == B_OK) {
                BPath path(&dir);
                path.Append(name);
                ExportMidi(path.Path());
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
            BRect wr(120, 120, 600, 740);
            (new EffectsWindow(wr, fProject->masterFx, kMasterFxTarget,
                               BMessenger(this)))->Show();
            break;
        }
        case kMsgFxWinOpen: {   // an effects editor opened: meter its track
            int64 tid = 0; BMessenger m;
            msg->FindInt64("track", &tid);
            msg->FindMessenger("msgr", &m);
            fFxMsgr = m;
            fFxTrack = (TrackId)tid;
            if (fEngine) fEngine->SetMeterFocus((TrackId)tid);
            break;
        }
        case kMsgFxWinClosed: {
            int64 tid = 0;
            msg->FindInt64("track", &tid);
            if (fFxTrack == (TrackId)tid) {
                fFxMsgr = BMessenger();
                fFxTrack = kInvalidTrackId;
                if (fEngine) fEngine->SetMeterFocus(kInvalidTrackId);
            }
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
        case kMsgApplyNotes: {
            // A PianoRoll posts one region's edited note list (clip-relative).
            // Undoable via SetMidiClipNotesCommand (one step per gesture).
            int64 tid = 0, cid = 0;
            msg->FindInt64("track", &tid);
            msg->FindInt64("clip", &cid);
            Track* tr = fProject->FindTrack((TrackId)tid);
            if (tr && tr->FindMidiClip((ClipId)cid)) {
                std::vector<MidiNote> notes;
                int32 pitch = 0;
                for (int32 i = 0; msg->FindInt32("np", i, &pitch) == B_OK; i++) {
                    MidiNote n;
                    int32 vel = 100; int64 st = 0, len = 1;
                    msg->FindInt32("nv", i, &vel);
                    msg->FindInt64("ns", i, &st);
                    msg->FindInt64("nl", i, &len);
                    n.pitch = pitch; n.velocity = vel;
                    n.startFrame = (Frame)st; n.lengthFrames = (Frame)len;
                    notes.push_back(n);
                }
                fStack->Execute(std::make_unique<SetMidiClipNotesCommand>(
                    (TrackId)tid, (ClipId)cid, std::move(notes)), *fProject);
                fTimeline->Invalidate();
            }
            break;
        }
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
            const bool master = ((TrackId)tid == kMasterFxTarget);
            if (!master && !fProject->FindTrack((TrackId)tid)) break;
            std::vector<EffectDesc> chain;
            int32 type = 0, epIdx = 0;
            for (int32 i = 0; msg->FindInt32("et", i, &type) == B_OK; i++) {
                EffectDesc d;
                d.type = (type >= 0 && type <= 8) ? (EffectType)type
                                                  : EffectType::Biquad;
                BString pn;
                if (msg->FindString("en", i, &pn) == B_OK)
                    d.pluginName = pn.String();
                int32 count = 0;
                msg->FindInt32("ec", i, &count);
                for (int32 j = 0; j < count; j++) {
                    float v = 0.0f;
                    msg->FindFloat("ep", epIdx++, &v);
                    d.params.push_back(v);
                }
                chain.push_back(d);
            }
            fStack->Execute(std::make_unique<SetFxCommand>(
                (TrackId)tid, master, std::move(chain)), *fProject);
            // Apply the edit to the running engine so it takes effect live.
            // Param tweaks sync in place; a structural change (add/remove/
            // reorder/replace) rebuilds the engine at the playhead.
            if (fEngine && !fEngine->SyncFx(*fProject))
                ReloadActiveEngine();
            fTimeline->Invalidate();
            break;
        }
        case kMsgToggleFxAuto: {
            // Toggle an effect-parameter automation lane (create seeded at the
            // current value, or remove if one already exists for this fx+slot).
            int64 tid = 0; int32 fx = 0, slot = 0; float val = 0;
            msg->FindInt64("track", &tid);
            msg->FindInt32("fx", &fx);
            msg->FindInt32("slot", &slot);
            msg->FindFloat("val", &val);
            if (Track* t = fProject->FindTrack((TrackId)tid)) {
                auto it = std::find_if(t->fxAuto.begin(), t->fxAuto.end(),
                    [&](const FxAutoLane& fa) {
                        return fa.fxIndex == fx && fa.slot == slot; });
                if (it != t->fxAuto.end()) t->fxAuto.erase(it);
                else {
                    FxAutoLane fa; fa.fxIndex = fx; fa.slot = slot;
                    fa.lane.AddPoint(0, val);   // seed constant; edit in timeline
                    t->fxAuto.push_back(fa);
                }
                fTimeline->Invalidate();
            }
            break;
        }
        case kMsgApplySends: {
            // A SendsWindow posts the edited send list. Undoable via a coalescing
            // SetSendsCommand (its native sliders post continuously -> one undo
            // step per drag). Applies on the next Play.
            int64 tid = 0;
            msg->FindInt64("track", &tid);
            if (fProject->FindTrack((TrackId)tid)) {
                std::vector<Send> sends;
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
                        sends.push_back(s);
                }
                fStack->Execute(std::make_unique<SetSendsCommand>(
                    (TrackId)tid, std::move(sends)), *fProject);
                fTimeline->Invalidate();
            }
            break;
        }
        case kMsgApplyInstrument: {
            // An InstrumentWindow posts the edited voice. Undoable via a
            // coalescing SetInstrumentCommand. Applies on the next Play.
            int64 tid = 0;
            msg->FindInt64("track", &tid);
            if (fProject->FindTrack((TrackId)tid)) {
                Instrument in;
                int32 wv = 0; float a = 0, d = 0, s = 0, r = 0;
                msg->FindInt32("wave", &wv);
                msg->FindFloat("a", &a); msg->FindFloat("d", &d);
                msg->FindFloat("s", &s); msg->FindFloat("r", &r);
                in.waveform = (wv >= 0 && wv <= 3) ? wv : 0;
                in.attack = a; in.decay = d; in.sustain = s; in.release = r;
                fStack->Execute(std::make_unique<SetInstrumentCommand>(
                    (TrackId)tid, in), *fProject);
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
            if (msg->FindString("path", &path) == B_OK && path) {
                int64 tid = 0, start = 0;
                if (msg->FindInt64("tid", &tid) == B_OK) {   // drag-drop target
                    msg->FindInt64("start", &start);
                    ImportAudioAt(path, (TrackId)tid, (Frame)start);
                } else {
                    ImportAudio(path);   // double-click: first track / playhead
                }
            }
            break;
        }
        case kMsgRegionNormalize: case kMsgRegionReverse: case kMsgRegionStrip: {
            int64 tid = 0, cid = 0;
            if (msg->FindInt64("track", &tid) != B_OK) break;
            msg->FindInt64("clip", &cid);
            if (msg->what == kMsgRegionNormalize)
                RegionNormalize((TrackId)tid, (ClipId)cid);
            else if (msg->what == kMsgRegionReverse)
                RegionReverse((TrackId)tid, (ClipId)cid);
            else
                RegionStripSilence((TrackId)tid, (ClipId)cid);
            break;
        }
        case kMsgFreezeTrack: {
            int64 tid = 0; bool freeze = true;
            if (msg->FindInt64("track", &tid) != B_OK) break;
            msg->FindBool("freeze", &freeze);
            FreezeTrack((TrackId)tid, freeze);
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
            // Rebuild the running engine so the metronome + tempo-synced effects
            // (delay) pick up the new tempo live; stopped, it applies next Play.
            ReloadActiveEngine();
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
            if (fEngine && fMonitoring && !fPlaying && !fRecMode) {
                // Idle live-monitoring: apply live gain/pan/mute edits, then
                // drive the meters (no playhead / transport).
                fEngine->UpdateMix(*fProject);
                fMeter->SetLevels(fEngine->PeakL(), fEngine->PeakR());
                PushTrackPeaks();
                break;
            }
            if (fEngine && (fPlaying || fRecMode)) {
                fEngine->UpdateMix(*fProject);   // live gain/pan/mute/solo
                const Frame ph = fEngine->Playhead();
                const Transport& tr = fProject->transport;

                if (fRecMode) {
                    // Count-in over: begin capture once we reach the record point.
                    if (fCapturePending && ph >= fRecPoint)
                        StartCapture();
                    // Drain live MIDI into the take (stamped by kit timestamp,
                    // independent of this ~60 Hz poll's granularity).
                    if (fMidiIn && !fCapturePending) {
                        MidiEvent ev[64];
                        std::size_t n;
                        while ((n = fMidiIn->ReadEvents(ev, 64)) > 0)
                            for (std::size_t i = 0; i < n; i++) {
                                Frame mf = fRecStart
                                    + (Frame)((double)(ev[i].timeUs - fMidiT0)
                                              * 1e-6 * fProject->sampleRate);
                                if (mf < fRecStart) mf = fRecStart;
                                fMidiRec.OnEvent(ev[i], mf);
                            }
                    }
                    // Loop-record: at the loop end, rewind the engine to the
                    // loop start (the recorder keeps capturing across the seam).
                    if (fLoopRecord && tr.loopEnabled && ph >= tr.loopEnd) {
                        fProject->transport.playhead = tr.loopStart;
                        if (!StartRecordEngine(tr.loopStart)) {
                            std::fprintf(stderr, "MainWindow: loop-record engine "
                                                 "restart failed; stopping\n");
                            StopRecording();
                        }
                        break;
                    }
                    const bool capturing = fRecorder && fRecorder->IsRecording();
                    const bool midiCap   = fMidiIn && !fCapturePending;
                    fTimeline->SetPlayhead(ph);
                    UpdateTimeReadout(ph);
                    if (capturing || midiCap) {
                        // Growing recording region + its live content.
                        fTimeline->SetRecording(true, fRecStart, ph - fRecStart);
                        if (capturing)
                            fTimeline->SetLiveAudio(fRecorder.get(), fProject->sampleRate);
                        if (midiCap) {
                            std::set<TrackId> mt(fMidiRecTracks.begin(),
                                                 fMidiRecTracks.end());
                            fTimeline->SetLiveMidiNotes(mt, fMidiRec.SnapshotNotes(ph));
                        }
                    }
                    fMeter->SetLevels(capturing ? fRecorder->PeakL() : fEngine->PeakL(),
                                      capturing ? fRecorder->PeakR() : fEngine->PeakR());
                    PushTrackPeaks();
                    PushFxMeters();
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
                PushFxMeters();
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
    const bool need = fPlaying || fRecMode || fMonitoring
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
        for (const MidiClip& mc : t.midiClips)
            if (mc.startFrame + mc.lengthFrames > end) end = mc.startFrame + mc.lengthFrames;
    }
    return end;
}

void MainWindow::StartPlayback() {
    if (fRecMode)
        return;   // recording runs its own engine (overdub)
    StopMidiMonitor();   // playback owns the engine + MIDI input
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
    // Re-apply the effect-meter focus onto the fresh engine (else an open FX
    // editor's GR/FFT meters die on every play / loop-wrap / seek rebuild).
    if (fFxTrack != kInvalidTrackId) fEngine->SetMeterFocus(fFxTrack);
    fPlaying = true;
    if (fTransport) fTransport->SetPlaying(true);
    UpdatePulse();
}

void MainWindow::StopPlayback(bool resumeMonitor) {
    if (fEngine)
        fEngine->Stop();
    fPlaying = false;
    if (fTransport) fTransport->SetPlaying(false);
    UpdatePulse();
    fMeter->SetLevels(0.0f, 0.0f);
    fTimeline->ClearTrackPeaks();
    UpdateLoudnessReadout(Loudness::kSilenceLufs, Loudness::kSilenceLufs,
                          Loudness::kSilenceDb);
    // Leave the playhead where it stopped; the readout keeps its last value.
    if (resumeMonitor)
        UpdateMidiMonitor();   // resume idle monitoring if a MIDI track is armed
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
        fEngine->SetInputMonitor(AudioMonitorOn());
    }
    if (fMidiIn) fEngine->SetLiveMidi(fMidiIn->MonitorInput());
    if (fFxTrack != kInvalidTrackId) fEngine->SetMeterFocus(fFxTrack);
    return true;
}

bool MainWindow::AudioMonitorOn() const {
    if (fMonitorInput) return true;
    for (TrackId id : fRecTracks)
        if (const Track* t = fProject->FindTrack(id))
            if (t->inputMonitor) return true;
    return false;
}

void MainWindow::StartCapture() {
    fRecStart = fRecPoint;      // clip origin = record point
    fCapturePending = false;

    // Audio capture: only when an audio track is armed (a MIDI-only take opens
    // no input device and writes no WAV).
    if (!fRecTracks.empty()) {
        // Write takes into the project's directory (a self-contained bundle)
        // when the project has been saved; otherwise the working directory.
        char name[64];
        std::snprintf(name, sizeof(name), "take-%d.wav", ++fTakeCounter);
        fTakePath = fTakeDir.empty() ? std::string(name)
                                     : fTakeDir + "/" + name;
        fRecorder.reset(new Recorder());
        if (fRecorder->Start(fTakePath.c_str()) != B_OK) {
            std::fprintf(stderr, "MainWindow: recording failed to start\n");
            fRecorder.reset();
            --fTakeCounter;
        } else {
            // Wire input monitoring: the engine mixes the recorder's live input
            // (only if the input rate matches the output rate).
            const bool audMon = AudioMonitorOn();
            fRecorder->SetMonitor(audMon);
            if (fEngine) {
                fEngine->SetMonitorSource(fRecorder.get());
                fEngine->SetInputMonitor(audMon);
            }
        }
    }

    StartMidiCapture();
}

// Open a MIDI consumer, connect the input endpoint of every MIDI track that is
// armed OR input-monitored (so you hear yourself either way), and begin the
// note-pairing recorder. Recorded clips still drop only on armed tracks.
void MainWindow::StartMidiCapture() {
    std::vector<TrackId> monitor;   // armed OR input-monitor, with a MIDI input
    for (const Track& t : fProject->Tracks())
        if (t.type == TrackType::Midi && (t.armed || t.inputMonitor)
            && t.input.kind == InputSource::kMidi)
            monitor.push_back(t.id);
    if (monitor.empty()) return;
    fMidiIn.reset(new MidiInputPort("HaikuDAW In"));
    if (fMidiIn->Register() != B_OK) {
        std::fprintf(stderr, "MainWindow: MIDI input register failed\n");
        fMidiIn.reset();
        return;
    }
    const std::vector<MidiEndpointInfo> eps = EnumerateMidiEndpoints();
    std::set<int32> connected;   // dedupe: tracks may share an endpoint
    for (TrackId id : monitor) {
        const Track* t = fProject->FindTrack(id);
        if (!t) continue;
        for (const MidiEndpointInfo& e : eps)
            if (e.isProducer && e.name == t->input.name) {
                if (connected.insert(e.id).second) fMidiIn->ConnectFrom(e.id);
                break;
            }
    }
    // Discard events queued before this take (both the record and monitor
    // rings) so stale pre-connect notes don't record or sound, then start.
    MidiEvent tmp[64];
    while (fMidiIn->ReadEvents(tmp, 64) > 0) {}
    while (fMidiIn->MonitorInput()->ReadEvents(tmp, 64) > 0) {}
    fMidiRec.Begin(fRecStart);
    fMidiT0 = system_time();
    // Route live events to the engine so armed MIDI tracks sound as you play.
    if (fEngine) fEngine->SetLiveMidi(fMidiIn->MonitorInput());
}

// End the MIDI take at `endFrame` and drop the resulting region onto each armed
// MIDI track (one fresh clip id per track). Tears down the input consumer.
void MainWindow::StopMidiCapture(Frame endFrame) {
    if (!fMidiIn) return;
    // Drain any events still queued, stamping each by wall-clock frame.
    MidiEvent ev[64];
    std::size_t n;
    while ((n = fMidiIn->ReadEvents(ev, 64)) > 0)
        for (std::size_t i = 0; i < n; i++) {
            Frame mf = fRecStart + (Frame)((double)(ev[i].timeUs - fMidiT0)
                                           * 1e-6 * fProject->sampleRate);
            if (mf < fRecStart) mf = fRecStart;
            fMidiRec.OnEvent(ev[i], mf);
        }
    const MidiClip take = fMidiRec.End(endFrame);
    if (fEngine) fEngine->SetLiveMidi(nullptr);   // stop monitoring this source
    fMidiIn.reset();   // disconnect + unregister the consumer

    std::vector<TrackId> targets;
    targets.swap(fMidiRecTracks);
    if (take.notes.empty()) return;   // nothing played: no clip
    for (TrackId target : targets) {
        MidiClip c = take;             // AddMidiClipCommand assigns a fresh id
        c.id = kInvalidClipId;
        fStack->Execute(std::make_unique<AddMidiClipCommand>(target, c), *fProject);
    }
    fTimeline->Invalidate();
}

void MainWindow::ReloadActiveEngine() {
    // Rebuild whatever engine is running, at the current position, so a change
    // that can't be applied in place (adding/removing an effect, a tempo edit)
    // takes effect without a manual stop/play. A brief seam is expected.
    if (fPlaying) {
        // If the engine has already reached the end, let it stop naturally
        // rather than restart from 0 (StartPlayback rewinds a past-end playhead).
        if (fEngine && fEngine->IsFinished()) return;
        if (fEngine) fProject->transport.playhead = fEngine->Playhead();
        StartPlayback();          // rebuilds at the playhead and keeps playing
    } else if (fMonitoring) {
        UpdateMidiMonitor();      // rebuilds the idle monitor engine
    }
    // Stopped / recording: the change applies on the next Play / take.
}

void MainWindow::StopMidiMonitor() {
    if (!fMonitoring) return;
    if (fEngine) {
        fEngine->SetLiveMidi(nullptr);
        fEngine->Stop();
        fEngine.reset();
    }
    fMidiIn.reset();
    fMonitoring = false;
    UpdatePulse();
}

// Reconcile idle live-monitoring with the current arming. When idle (not
// playing / recording) and at least one MIDI track is armed with a MIDI input,
// run a monitor-only engine so the player hears themselves before pressing
// record. Rebuilt on every change (cheap) to pick up new arming / inputs.
void MainWindow::UpdateMidiMonitor() {
    StopMidiMonitor();
    if (fPlaying || fRecMode) return;   // playback / record own the engine + input

    std::vector<TrackId> armed;   // MIDI tracks to monitor: armed OR input-monitor
    for (const Track& t : fProject->Tracks())
        if (t.type == TrackType::Midi && (t.armed || t.inputMonitor)
            && t.input.kind == InputSource::kMidi)
            armed.push_back(t.id);
    if (armed.empty()) return;

    // Open a consumer and connect each armed track's endpoint by name.
    fMidiIn.reset(new MidiInputPort("HaikuDAW In"));
    if (fMidiIn->Register() != B_OK) { fMidiIn.reset(); return; }
    const std::vector<MidiEndpointInfo> eps = EnumerateMidiEndpoints();
    std::set<int32> connected;   // dedupe: two armed tracks may share an endpoint
    for (TrackId id : armed) {
        const Track* t = fProject->FindTrack(id);
        if (!t) continue;
        for (const MidiEndpointInfo& e : eps)
            if (e.isProducer && e.name == t->input.name) {
                if (connected.insert(e.id).second) fMidiIn->ConnectFrom(e.id);
                break;
            }
    }

    // A monitor-only engine: renders live voices through the armed instruments,
    // no clip playback, no playhead advance.
    const Frame ph = fProject->transport.playhead;
    const Frame tenMin = (Frame)(fProject->sampleRate * 600.0);
    fEngine.reset(new Engine());
    fEngine->SetBufferFrames(fBufferFrames);
    fEngine->SetMonitorOnly(true);
    if (fEngine->Load(*fProject, ph, ph + tenMin) != B_OK) {
        fEngine.reset();
        fMidiIn.reset();
        return;
    }
    MidiEvent tmp[64];   // drop stale pre-connect events before monitoring
    while (fMidiIn->MonitorInput()->ReadEvents(tmp, 64) > 0) {}
    fEngine->SetLiveMidi(fMidiIn->MonitorInput());
    fEngine->Start();
    fMonitoring = true;
    UpdatePulse();   // poll the meters while monitoring
}

void MainWindow::StartRecording() {
    if (fRecMode || (fRecorder && fRecorder->IsRecording()))
        return;
    if (fPlaying) StopPlayback(false);   // don't spin up a monitor we replace
    StopMidiMonitor();   // the record engine takes over monitoring

    // Record onto every armed audio track (one input take, dropped on each),
    // and capture live MIDI onto every armed MIDI track that has a MIDI input.
    fRecTracks.clear();
    fMidiRecTracks.clear();
    for (const Track& t : fProject->Tracks()) {
        if (!t.armed) continue;
        if (t.type == TrackType::Audio)
            fRecTracks.push_back(t.id);
        else if (t.type == TrackType::Midi && t.input.kind == InputSource::kMidi)
            fMidiRecTracks.push_back(t.id);
    }
    if (fRecTracks.empty() && fMidiRecTracks.empty()) {
        std::fprintf(stderr, "MainWindow: arm a track (R) before recording "
                             "(MIDI tracks also need an input: right-click Arm)\n");
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
    if (fTransport) fTransport->SetRecording(true);
    fCapturePending = (countIn > 0);
    if (!fCapturePending)
        StartCapture();         // no count-in: capture immediately
    UpdatePulse();
}

void MainWindow::StopRecording() {
    if (!fRecMode)
        return;

    // Take end = the playhead now, before the engine stops (MIDI needs it to
    // close held notes at the take boundary).
    const Frame endPh = fEngine ? fEngine->Playhead() : fRecStart;
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

    // Stop the overdub engine + live REC region, end the poll. Detach the
    // monitor source before the recorder is freed so the RT callback (already
    // halted by Stop) never dereferences it again.
    if (fEngine) {
        fEngine->SetInputMonitor(false);
        fEngine->SetMonitorSource(nullptr);
        fEngine->Stop();
    }
    fRecMode = false;
    if (fTransport) fTransport->SetRecording(false);
    fCapturePending = false;
    fTimeline->SetRecording(false, 0, 0);
    fRecTracks.clear();
    UpdatePulse();
    fMeter->SetLevels(0.0f, 0.0f);

    // Finalize the MIDI take independently of the audio take (a MIDI-only
    // record has no audio recorder / targets, so this must run before the
    // audio empty-take early-out below).
    StopMidiCapture(endPh);
    // Resume idle monitoring if a MIDI track is still armed (runs on every
    // StopRecording exit path, since the clip-drop code below may early-return).
    UpdateMidiMonitor();

    if (!captured || frames <= 0 || targets.empty()) {
        if (fRecorder) std::fprintf(stderr, "MainWindow: empty take, no clip added\n");
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
    // Default target: the first audio track (created if none), at the playhead.
    ImportAudioAt(path, kInvalidTrackId, fProject->transport.playhead);
}

void MainWindow::ImportAudioAt(const char* path, TrackId track, Frame start) {
    WavSource src;
    if (!src.Open(path)) {
        std::fprintf(stderr, "MainWindow: cannot import '%s'\n", path);
        return;
    }
    // Resolve the target track: the requested one if it's audio, else the first
    // audio track, creating one if the project has none.
    TrackId tid = kInvalidTrackId;
    if (const Track* t = fProject->FindTrack(track))
        if (t->type == TrackType::Audio) tid = track;
    if (tid == kInvalidTrackId)
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
    clip.startFrame   = start < 0 ? 0 : start;
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

void MainWindow::ImportMidi(const char* path) {
    SmfData d;
    if (!ReadSmf(path, d)) {
        std::fprintf(stderr, "MainWindow: cannot import MIDI '%s'\n", path);
        return;
    }
    // Convert ticks -> frames at the project tempo (beat = tick / division). Each
    // SMF track with notes becomes a new MIDI track holding one region. Undo is
    // per-track (AddTrack + AddMidiClip) — MacroCommand can't thread the new
    // track id to the clip add, and per-track granularity is acceptable here.
    const double perTick = (d.division > 0)
        ? (fProject->sampleRate * 60.0 / fProject->tempoBPM) / (double)d.division
        : (fProject->sampleRate * 60.0 / fProject->tempoBPM) / 480.0;

    int added = 0;
    for (const SmfTrack& st : d.tracks) {
        if (st.notes.empty()) continue;             // skip conductor/empty tracks
        MidiClip clip;
        clip.startFrame = 0;
        Frame maxEnd = 0;
        for (const SmfNote& sn : st.notes) {
            MidiNote mn;
            mn.pitch      = sn.pitch;
            mn.velocity   = sn.velocity < 1 ? 1 : (sn.velocity > 127 ? 127 : sn.velocity);
            mn.startFrame = (Frame)llround(sn.startTick * perTick);
            mn.lengthFrames = (Frame)llround(sn.lengthTick * perTick);
            if (mn.lengthFrames < 1) mn.lengthFrames = 1;
            clip.notes.push_back(mn);
            if (mn.startFrame + mn.lengthFrames > maxEnd)
                maxEnd = mn.startFrame + mn.lengthFrames;
        }
        clip.lengthFrames = maxEnd;

        char nm[48];
        if (!st.name.empty()) std::snprintf(nm, sizeof(nm), "%.31s", st.name.c_str());
        else                  std::snprintf(nm, sizeof(nm), "MIDI %d", added + 1);

        auto add = std::make_unique<AddTrackCommand>(TrackType::Midi, nm);
        AddTrackCommand* ap = add.get();
        fStack->Execute(std::move(add), *fProject);
        fStack->Execute(std::make_unique<AddMidiClipCommand>(ap->CreatedId(), clip),
                        *fProject);
        ++added;
    }
    if (added == 0)
        std::fprintf(stderr, "MainWindow: '%s' had no note tracks\n", path);
    fTimeline->Invalidate();
}

void MainWindow::ExportMidi(const char* path) {
    const double fpb = fProject->sampleRate * 60.0 / fProject->tempoBPM;
    const uint16_t division = 480;
    SmfData d;
    d.division = division;
    d.tempoBpm = fProject->tempoBPM;
    for (const Track& t : fProject->Tracks()) {
        if (t.type != TrackType::Midi) continue;
        SmfTrack st;
        st.name = t.name;
        for (const MidiNote& n : t.CollectNotes()) {   // absolute-timeline notes
            SmfNote sn;
            sn.pitch      = n.pitch;
            sn.velocity   = n.velocity;
            sn.startTick  = (uint32_t)llround((n.startFrame / fpb) * division);
            sn.lengthTick = (uint32_t)llround((n.lengthFrames / fpb) * division);
            if (sn.lengthTick < 1) sn.lengthTick = 1;
            st.notes.push_back(sn);
        }
        d.tracks.push_back(std::move(st));
    }
    if (d.tracks.empty()) {
        std::fprintf(stderr, "MainWindow: no MIDI tracks to export\n");
        return;
    }
    if (!WriteSmf(path, d))
        std::fprintf(stderr, "MainWindow: MIDI export failed: %s\n", path);
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

// A unique path for a rendered region/freeze file: in the take dir (beside the
// project) if known, else the working dir. Suffixed with a session counter so
// repeated ops don't collide.
std::string MainWindow::RenderPath(const std::string& tag) const {
    std::string dir = fTakeDir.empty() ? std::string(".") : fTakeDir;
    char name[64];
    std::snprintf(name, sizeof(name), "/%s-%d.wav", tag.c_str(), fRenderSeq);
    return dir + name;
}

int64_t MainWindow::DecodeClipRegion(const Clip& c, std::vector<float>& out,
                                     double& outRate) const {
    out.clear();
    outRate = 0.0;
    WavSource src;
    if (!src.Open(c.sourcePath))
        return 0;
    outRate = src.FrameRate();
    if (c.sourceOffset > 0)
        src.Seek(c.sourceOffset);
    // The clip plays lengthFrames project-frames == that many source-frames
    // scaled by the rate ratio, starting at sourceOffset.
    const double projRate = fProject->sampleRate;
    int64_t wantSrc = c.lengthFrames;
    if (outRate > 0 && projRate > 0)
        wantSrc = (int64_t)llround((double)c.lengthFrames * outRate / projRate);
    const float* chunk = nullptr;
    size_t frames = 0;
    while ((int64_t)(out.size() / 2) < wantSrc && src.ReadChunk(&chunk, &frames)) {
        int64_t have = (int64_t)(out.size() / 2);
        int64_t take = wantSrc - have;
        if ((int64_t)frames > take) frames = (size_t)take;
        out.insert(out.end(), chunk, chunk + frames * 2);
    }
    return (int64_t)(out.size() / 2);
}

void MainWindow::RegionNormalize(TrackId track, ClipId clip) {
    const Track* t = fProject->FindTrack(track);
    const Clip*  c = t ? t->FindClip(clip) : nullptr;
    if (!c) return;
    std::vector<float> buf; double rate = 0;
    const int64_t n = DecodeClipRegion(*c, buf, rate);
    if (n <= 0) return;
    const float peak = PeakLinear(buf.data(), n);
    if (peak <= 1e-6f) return;              // silent: nothing to normalize
    float g = 1.0f / peak;
    if (g > 64.0f) g = 64.0f;               // ceiling for near-silent clips
    fStack->Execute(std::make_unique<SetClipGainCommand>(track, clip, g), *fProject);
    fTimeline->Invalidate();
}

void MainWindow::RegionReverse(TrackId track, ClipId clip) {
    const Track* t = fProject->FindTrack(track);
    const Clip*  c = t ? t->FindClip(clip) : nullptr;
    if (!c) return;
    std::vector<float> buf; double rate = 0;
    const int64_t n = DecodeClipRegion(*c, buf, rate);
    if (n <= 0 || rate <= 0) return;
    ReverseStereo(buf.data(), n);

    const std::string path = RenderPath("reversed");
    std::vector<int16_t> pcm((size_t)n * 2);
    for (int64_t i = 0; i < n * 2; ++i) {
        float s = buf[i];
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = (int16_t)lround(s * 32767.0f);
    }
    WavWriter w;
    if (!w.Open(path, (int)lround(rate), 2)
        || !w.WriteInt16(pcm.data(), pcm.size()) || !w.Close()) {
        std::fprintf(stderr, "Reverse: cannot write %s\n", path.c_str());
        return;
    }
    ++fRenderSeq;

    // Replace the clip with one pointing at the reversed file (fades swap so the
    // fade follows the now-reversed audio); same position, length, gain.
    Clip nc = *c;
    nc.id = kInvalidClipId;
    nc.sourcePath   = path;
    nc.sourceOffset = 0;
    nc.takeGroup    = 0;
    std::swap(nc.fadeInFrames, nc.fadeOutFrames);
    auto macro = std::make_unique<MacroCommand>("Reverse Clip");
    macro->Add(std::make_unique<RemoveClipCommand>(track, clip));
    macro->Add(std::make_unique<AddClipCommand>(track, nc));
    fStack->Execute(std::move(macro), *fProject);

    WavSource src;
    if (src.Open(path)) (*fPeaks)[path].Build(src);
    fTimeline->Invalidate();
}

void MainWindow::RegionStripSilence(TrackId track, ClipId clip) {
    const Track* t = fProject->FindTrack(track);
    const Clip*  c = t ? t->FindClip(clip) : nullptr;
    if (!c) return;
    std::vector<float> buf; double rate = 0;
    const int64_t n = DecodeClipRegion(*c, buf, rate);
    if (n <= 0 || rate <= 0) return;

    const float   thresh = 0.00316f;               // ~ -50 dBFS
    const int64_t minSil = (int64_t)(0.25 * rate); // 250 ms of silence = a gap
    const int64_t pad    = (int64_t)(0.02 * rate); // keep 20 ms of air each side
    auto spans = NonSilentSpans(buf.data(), n, thresh, minSil, pad);
    if (spans.size() <= 1) return;                 // no gaps worth cutting

    const double projRate = fProject->sampleRate;
    const double toProj = (rate > 0) ? projRate / rate : 1.0;   // src -> project
    auto macro = std::make_unique<MacroCommand>("Strip Silence");
    macro->Add(std::make_unique<RemoveClipCommand>(track, clip));
    for (const daw::Span& s : spans) {
        Clip nc = *c;
        nc.id           = kInvalidClipId;
        nc.sourceOffset = c->sourceOffset + s.start;              // source frames
        nc.startFrame   = c->startFrame + (Frame)llround(s.start * toProj);
        nc.lengthFrames = (Frame)llround((s.end - s.start) * toProj);
        nc.fadeInFrames = 0;
        nc.fadeOutFrames = 0;
        nc.takeGroup    = 0;
        macro->Add(std::make_unique<AddClipCommand>(track, nc));
    }
    fStack->Execute(std::move(macro), *fProject);
    fTimeline->Invalidate();
}

// Render one track in isolation (its clips/notes through its fader + effect
// chain) to a stereo WAV — the basis for Freeze. Routing/sends are stripped so
// only the track's own output is baked; solo/mute are cleared so it's audible.
static bool RenderTrackToWav(const Project& src, TrackId id,
                             const std::string& out) {
    const Track* t = src.FindTrack(id);
    if (!t) return false;
    Project iso;
    iso.sampleRate = src.sampleRate;
    iso.tempoBPM   = src.tempoBPM;
    iso.timeSig    = src.timeSig;
    iso.tempoMap   = src.tempoMap;
    Track copy = *t;
    copy.output = kRoutingMaster;      // straight to the master sum
    copy.sends.clear();
    copy.muted = false;
    copy.soloed = false;
    copy.frozen = false;
    copy.freezeClips.clear();
    copy.freezeMidi.clear();
    copy.freezeFx.clear();
    iso.AddTrack(copy);
    return ExportWav(iso, out, src.sampleRate);
}

void MainWindow::FreezeTrack(TrackId track, bool freeze) {
    Track* t = fProject->FindTrack(track);
    if (!t) return;

    if (!freeze) {                     // unfreeze: pure model restore
        if (!t->frozen) return;
        fStack->Execute(std::make_unique<FreezeTrackCommand>(track, false),
                        *fProject);
        RebuildPeaks();
        fTimeline->Invalidate();
        return;
    }
    if (t->frozen) return;

    const std::string path = RenderPath("frozen");
    if (!RenderTrackToWav(*fProject, track, path)) {
        std::fprintf(stderr, "Freeze: render failed for track %ld\n", (long)track);
        return;
    }
    ++fRenderSeq;
    WavSource src;
    if (!src.Open(path)) return;
    const double fileRate = src.FrameRate();
    const double projRate = fProject->sampleRate;
    Clip fc;
    fc.startFrame   = 0;
    fc.sourceOffset = 0;
    fc.sourcePath   = path;
    fc.lengthFrames = (Frame)llround(src.TotalFrames()
                        * (fileRate > 0 ? projRate / fileRate : 1.0));
    fStack->Execute(std::make_unique<FreezeTrackCommand>(track, true, fc),
                    *fProject);
    (*fPeaks)[path].Build(src);
    fTimeline->Invalidate();
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
    if (fTimeline) fTimeline->SetMonitorInput(fMonitorInput);
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

    // Feed the inspector's VU meter for the selected track.
    if (fInspector) {
        auto it = tp.find(fInspector->SelectedTrack());
        if (it != tp.end()) fInspector->SetMeter(it->second.first, it->second.second);
        else                fInspector->SetMeter(0.0f, 0.0f);
    }

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

void MainWindow::PushFxMeters() {
    if (!fEngine || !fFxMsgr.IsValid()) return;
    BMessage m(kMsgFxMeter);
    float gr[16];
    for (int i = 0; i < 16; i++) gr[i] = fEngine->MeterGrDb(i);
    m.AddData("gr", B_FLOAT_TYPE, gr, sizeof(gr));
    float spec[256]; int specFx = -1;
    const int n = fEngine->MeterSpectrum(spec, 256, &specFx);
    if (n > 0) m.AddData("spec", B_FLOAT_TYPE, spec, n * sizeof(float));
    m.AddInt32("specfx", specFx);
    m.AddInt32("specn", n);
    fFxMsgr.SendMessage(&m);
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
