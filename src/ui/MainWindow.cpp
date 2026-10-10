#include "MainWindow.h"

#include "TimelineView.h"
#include "InspectorView.h"
#include "TransportBar.h"
#include "MeterView.h"
#include "EffectsWindow.h"
#include "SendsWindow.h"
#include "InstrumentWindow.h"
#include "../synth/SampleBank.h"
#include "PianoRoll.h"
#include "SampleBrowser.h"
#include "MixerWindow.h"
#include "PluginBrowser.h"
#include "ExportWindow.h"
#include "ExportProgressWindow.h"
#include "Version.h"   // DAW_VERSION_STRING (generated from CMake)
#ifdef DAW_HAVE_LV2
#include "Lv2UiWindow.h"
#endif
#include "../storage/BfsAttr.h"
#include "../app/AppSettings.h"
#include "RenameWindow.h"
#include "UiMetrics.h"
#include "widgets/DawSlider.h"      // the kit (M1.3)
#include "widgets/DawTextField.h"

#include "../engine/DeviceLatency.h"
#include "../engine/WavSource.h"
#include "../engine/WavWriter.h"
#include "../engine/Exporter.h"
#include "../engine/Resampler.h"
#include "../model/ProjectIO.h"
#include "../model/Commands.h"
#include "../model/RegionOps.h"
#include "../model/SmfIO.h"
#include "../model/MidiOps.h"
#include "../model/RecordPlan.h"
#include "../model/TakeNames.h"

#include <LayoutBuilder.h>
#include <SplitView.h>
#include <TabView.h>
#include <Alert.h>
#include <Application.h>
#include <OS.h>   // system_time() for MIDI event timestamping
#include <Button.h>
#include <File.h>
#include <FindDirectory.h>
#include <Entry.h>
#include <Directory.h>   // create_directory (stems folder)
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace daw {

enum {
    MSG_PLAY  = 'play',
    MSG_STOP  = 'stop',
    MSG_REC   = 'rec ',
    MSG_UNDO  = 'undo',
    MSG_REDO  = 'redo',
    MSG_OPEN  = 'open',
    MSG_OPEN_RECENT = 'orpj',   // File > Open Recent (path in the message)
    MSG_CLOSE = 'clos',         // File > Close: the window's quit path
    MSG_MASTER   = 'mvol',   // master volume slider moved
    MSG_ZOOM_IN  = 'zmin',
    MSG_ZOOM_OUT = 'zmot',
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
    MSG_TITLE     = 'titl',   // 2 Hz: refresh the dirty marker in the title
    MSG_IMPORT_MIDI     = 'imid',
    MSG_IMPORT_MIDI_REF = 'imdr',
    MSG_EXPORT_MIDI     = 'emid',
    MSG_EXPORT_MIDI_REF = 'emdr',
    MSG_FOLLOW    = 'folw',   // toggle: chase the playhead
    MSG_TOGGLE_INSPECTOR = 'tins',   // show/hide the inspector pane (M1.4)
    MSG_TOGGLE_DOCK      = 'tdck',   // show/hide the docked bottom pane
};

// Sentinel "track id" the effects editor uses to target the master FX chain.
static const TrackId kMasterFxTarget = ~(TrackId)0;

// Snapshot the mixer strip state from the model (used to open the mixer and to
// refresh it live when the model changes elsewhere).
static std::vector<MixerStripInfo> BuildMixerStrips(const Project& p) {
    std::vector<MixerStripInfo> strips;
    for (const Track& t : p.Tracks()) {
        MixerStripInfo s{};
        s.trackId = (uint64)t.id;
        s.name = t.name;
        s.gain = t.gain; s.pan = t.pan;
        s.muted = t.muted; s.soloed = t.soloed;
        s.armed = t.armed; s.inputMonitor = t.inputMonitor;
        s.colorIndex = t.colorIndex;
        s.type = t.type == TrackType::Midi ? 1 : t.type == TrackType::Bus ? 2 : 0;
        // The strip draws a label and a bypass dot per insert. Naming an insert
        // is done HERE, on the main thread, because EffectDisplayName resolves an
        // LV2 URI through the host -- which the mixer's own looper must not do.
        for (const EffectDesc& d : t.fx)
            s.fx.push_back(MixerInsertInfo{ EffectDisplayName(d), d.bypassed });
        s.sendCount = (int)t.sends.size();
        s.hasInput = t.input.kind != InputSource::kNone;
        if (t.output == kInvalidTrackId) s.outLabel = "Mst";
        else if (const Track* bt = p.FindTrack(t.output))
            s.outLabel = bt->name.substr(0, 6);
        else s.outLabel = "Bus";
        strips.push_back(std::move(s));
    }
    return strips;
}

// Decode a posted note list (PianoRoll -> here). kMsgApplyNotes and
// kMsgApplyMidiOp carry the same np/nv/ns/nl shape; missing fields fall back to
// the defaults the roll always writes, and the frames stay clip-relative.
static std::vector<MidiNote> ParseNoteList(const BMessage* msg) {
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
    return notes;
}

static constexpr float kTransportH = kDesignTransportH;   // design px
static constexpr bigtime_t kPulseInterval = 16000;   // ~60 Hz, microseconds

// --- native editor watches -------------------------------------------------
//
// A real editor window, as the kit-free table (src/plugin/FxWatchTable.h) sees
// it: whether it is still there, how to ask it to close, how to tell it its
// insert moved, and how to hand it a frame. Keeping this behind that interface
// is what lets the table's rules -- where the bugs were -- be tested on the
// Linux host, where none of this can be built.
struct MainWindow::EditorHandle : public FxWatchEditor {
    BMessenger msgr;
    explicit EditorHandle(const BMessenger& m) : msgr(m) {}
    bool Alive() const override { return msgr.IsValid(); }
    void AskToClose() override { msgr.SendMessage(B_QUIT_REQUESTED); }
    void InsertMoved(TrackId track, int fx) override {
        BMessage rebind(kMsgFxWatch);
        rebind.AddInt64("track", (int64)track);
        rebind.AddInt32("fx", fx);
        msgr.SendMessage(&rebind);
    }
    void SendFrame(const float* values, int count) override {
        BMessage m(kMsgFxParams);
        for (int i = 0; i < count; i++) m.AddFloat("v", values[i]);
        msgr.SendMessage(&m);
    }
};

// The table's view of what is open, for the kit-free rules to read.
std::vector<FxWatch> MainWindow::FxWatchSnapshot() const {
    std::vector<FxWatch> out;
    out.reserve(fFxWatches.size());
    for (const FxEntry& e : fFxWatches) {
        FxWatch w;
        w.editor = e.editor.get();
        w.uri    = e.uri;
        w.track  = e.track;
        w.fx     = e.fx;
        w.slot   = e.slot;
        w.gen    = e.gen;
        out.push_back(w);
    }
    return out;
}

// The chain one entry names, as the model sees it.
FxChainView MainWindow::FxChainFor(const FxWatch& w) const {
    FxChainView v;
    const std::vector<EffectDesc>* chain = nullptr;
    if (w.track == kMasterFxTarget) {
        chain = &fProject->masterFx;
    } else if (const Track* t = fProject->FindTrack(w.track)) {
        chain = &t->fx;
    }
    if (!chain) return v;
    v.exists = true;
    for (const EffectDesc& d : *chain) v.uris.push_back(d.pluginName);
    return v;
}

MainWindow::MainWindow(BRect frame, Project* project, CommandStack* stack,
                       PeakMap* peaks)
    : BWindow(frame, "Haiku DAW", B_TITLED_WINDOW,
              B_ASYNCHRONOUS_CONTROLS | B_QUIT_ON_WINDOW_CLOSE),
      fProject(project), fStack(stack), fPeaks(peaks),
      fDoc(*project, *stack) {
    fTransportCtl.SetWindow(this);
    fRecCtl.SetWindow(this);
    fRender.SetWindow(this);
    BRect bounds = Bounds();
    // Keep the tempo map's rate in sync with the project's sample rate.
    fProject->tempoMap.sampleRate = fProject->sampleRate;



    // --- Menu bar ---
    BMenuBar* menuBar = new BMenuBar(BRect(0, 0, bounds.right, 20), "menubar");
    BMenu* fileMenu = new BMenu("File");
    fileMenu->AddItem(new BMenuItem("New", new BMessage(MSG_NEW_PROJECT), 'N'));
    fileMenu->AddItem(new BMenuItem("Open" B_UTF8_ELLIPSIS, new BMessage(MSG_OPEN), 'O'));
    fRecentMenu = new BMenu("Open Recent");
    fileMenu->AddItem(fRecentMenu);
    // Save has no ellipsis: once the project has a path it writes straight to
    // it (Save As is the way to move it). Both ask when there is no path yet.
    fileMenu->AddItem(new BMenuItem("Save", new BMessage(MSG_SAVE), 'S'));
    fileMenu->AddItem(new BMenuItem("Save As" B_UTF8_ELLIPSIS,
                                    new BMessage(MSG_SAVE_AS), 'S', B_SHIFT_KEY));
    fileMenu->AddItem(new BMenuItem("Import Audio" B_UTF8_ELLIPSIS, new BMessage(MSG_IMPORT)));
    fileMenu->AddItem(new BMenuItem("Import MIDI" B_UTF8_ELLIPSIS, new BMessage(MSG_IMPORT_MIDI)));
    fileMenu->AddItem(new BMenuItem("Export WAV" B_UTF8_ELLIPSIS, new BMessage(MSG_EXPORT)));
    fileMenu->AddItem(new BMenuItem("Export Stems" B_UTF8_ELLIPSIS, new BMessage(MSG_EXPORT_STEMS)));
    fileMenu->AddItem(new BMenuItem("Export MIDI" B_UTF8_ELLIPSIS, new BMessage(MSG_EXPORT_MIDI)));
    fileMenu->AddSeparatorItem();
    // Close is the window's own quit request, so the unsaved-changes prompt
    // applies exactly as it does to the title-bar button and Cmd-Q.
    fileMenu->AddItem(new BMenuItem("Close", new BMessage(MSG_CLOSE), 'W'));
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
    BMenu* helpMenu = new BMenu("Help");
    helpMenu->AddItem(new BMenuItem("About Haiku DAW" B_UTF8_ELLIPSIS,
                                    new BMessage(MSG_ABOUT)));
    menuBar->AddItem(helpMenu);

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
    // The panes (M1.4): the inspector toggles with I, the dock with J.
    fInspectorItem = new BMenuItem("Inspector", new BMessage(MSG_TOGGLE_INSPECTOR), 'I');
    fInspectorItem->SetMarked(true);
    viewMenu->AddItem(fInspectorItem);
    fDockItem = new BMenuItem("Editor & Browsers", new BMessage(MSG_TOGGLE_DOCK), 'J');
    viewMenu->AddItem(fDockItem);
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
        if ((size_t)n == fTransportCtl.fBufferFrames) it->SetMarked(true);
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
        if (n == fRecCtl.fCountInBars) it->SetMarked(true);
        fCountInMenu->AddItem(it);
    }
    audioMenu->AddItem(fCountInMenu);
    fMonInItem = new BMenuItem("Monitor Input", new BMessage(MSG_MONITOR_IN));
    audioMenu->AddItem(fMonInItem);
    menuBar->AddItem(audioMenu);

    // --- Transport bar (below the menu) ---
    BRect barRect(0, 0, bounds.right, Themed(kTransportH));
    fTransport = new TransportBar(barRect, BMessenger(this),
                                  MSG_PLAY, MSG_STOP, MSG_REC,
                                  MSG_ZOOM_OUT, MSG_ZOOM_IN);
    BView* bar = fTransport;

    BFont lcdFont(be_bold_font);
    lcdFont.SetSize(Themed(15.0f));
    fTimeView = new BStringView(BRect(Themed(218), Themed(6), Themed(354),
                                        Themed(kTransportH) - Themed(5)),
                                "time", "1.1   0:00.000");
    fTimeView->SetViewColor(ColLcd());
    fTimeView->SetHighColor(ColLcdText());
    fTimeView->SetFont(&lcdFont);
    fTimeView->SetAlignment(B_ALIGN_CENTER);
    bar->AddChild(fTimeView);
    // (Zoom -/+ buttons are drawn by the TransportBar at x366..420.)

    // "Vol" label + master volume slider (0..150% -> gain 0..1.5).
    fVolLbl = new BStringView(BRect(Themed(430), Themed(8), Themed(460),
                                    Themed(kTransportH) - Themed(6)),
                              "vollbl", "Vol");
    fVolLbl->SetViewColor(ColChrome());
    fVolLbl->SetHighColor(ColText());
    bar->AddChild(fVolLbl);
    fMaster = new BSlider(BRect(Themed(462), Themed(4), Themed(588),
                                Themed(kTransportH) - Themed(4)),
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
    fBpmLbl = new BStringView(BRect(Themed(602), Themed(8), Themed(636),
                                    Themed(kTransportH) - Themed(6)),
                                          "bpmlbl", "BPM");
    fBpmLbl->SetViewColor(ColChrome());
    fBpmLbl->SetHighColor(ColText());
    bar->AddChild(fBpmLbl);
    char bpm[16];
    std::snprintf(bpm, sizeof(bpm), "%.0f", fProject->tempoBPM);
    fTempo = new BTextControl(BRect(Themed(638), Themed(6), Themed(704),
                                    Themed(kTransportH) - Themed(6)),
                              "tempo", NULL, bpm, new BMessage(MSG_TEMPO));
    fTempo->SetDivider(0.0f);
    bar->AddChild(fTempo);

    // Loudness readout (momentary / short-term LUFS + true peak dBTP).
    fLoudView = new BStringView(BRect(Themed(722), Themed(8), Themed(858),
                                      Themed(kTransportH) - Themed(6)),
                                "loud", "M -- S -- TP --");
    fLoudView->SetViewColor(ColChrome());
    fLoudView->SetHighColor(ColText());
    bar->AddChild(fLoudView);

    // Master output meter, pinned to the right of the transport bar.
    fMeter = new MeterView(BRect(bounds.right - Themed(130), Themed(5),
                                 bounds.right - Themed(6),
                                 Themed(kTransportH) - Themed(5)));
    bar->AddChild(fMeter);

    // --- The panes (M1.4) ---
    // Top row: the inspector (resizable, collapsible, toggled with I) beside
    // the timeline. Bottom: the dock, which hosts the MIDI editor and the
    // browsers and starts collapsed. A window-level layout drives both, so
    // resizing the window is the layout's business now, not FrameResized's.
    fPaneSplit = new BSplitView(B_HORIZONTAL, 1.0f);
    fInspector = new InspectorView(BRect(0, 0, InspectorWidth(), 100),
                                   project, stack);
    fTimeline = new TimelineView(BRect(0, 0, 400, 100), project, stack);
    fTimeline->SetPeaks(peaks);
    fPaneSplit->AddChild(fInspector, 0.0f);   // keeps its width...
    fPaneSplit->AddChild(fTimeline, 1.0f);    // ...the timeline takes the rest
    fPaneSplit->SetCollapsible(0, true);

    fDock = new BTabView("dock", B_WIDTH_FROM_WIDEST);
    fRootSplit = new BSplitView(B_VERTICAL, 1.0f);
    fRootSplit->AddChild(fPaneSplit, 1.0f);
    fRootSplit->AddChild(fDock, 0.0f);
    fRootSplit->SetCollapsible(1, true);
    fRootSplit->SetItemCollapsed(1, true);    // the dock starts hidden

    BLayoutBuilder::Group<>(this, B_VERTICAL, 0.0f)
        .Add(menuBar)
        .Add(bar)
        .Add(fRootSplit)
        .End();

    // Restore persisted preferences + window layout (after the menus exist).
    LoadSettings();
    // A restored window frame resizes the bar before the user touches anything,
    // so the layout has to be applied once here as well as on every resize.
    LayoutTransportBar();
    // Below this the bar has nothing left to hide: the transport controls, the
    // time readout and the meter need the room, and a window that clipped them
    // would be showing a fault rather than a small window.
    SetSizeLimits(620.0f, 32767.0f, 300.0f, 32767.0f);
    // Autosave for crash recovery; check for a leftover once the looper runs.
    fAutosave = new BMessageRunner(BMessenger(this), new BMessage(MSG_AUTOSAVE),
                                   30LL * 1000 * 1000);   // every 30 s
    fTitlePoll = new BMessageRunner(BMessenger(this), new BMessage(MSG_TITLE),
                                    500LL * 1000);        // 2 Hz dirty marker
    UpdateTitle();
    PostMessage(MSG_RECOVER);
}

MainWindow::~MainWindow() {
    // The export worker renders a snapshot taken on this thread, so it has to
    // stop before anything it touches goes away. The cancel flag is polled
    // between blocks, so this waits at most one block (and none at all when
    // nothing is running).
    if (fRender.fExportThread.joinable()) {
        fRender.fExportCancel.store(true);
        fRender.fExportThread.join();
    }
    delete fPulse;
    delete fAutosave;
    delete fTitlePoll;
    delete fSavePanel;
    delete fOpenPanel;
    delete fExportPanel;
    delete fStemsPanel;
    delete fImportPanel;
    delete fMidiImportPanel;
    delete fMidiExportPanel;
    // fTransportCtl.fEngine / fRecCtl.fRecorder destructors stop their threads.
}

void MainWindow::DispatchMessage(BMessage* message, BHandler* handler) {
    if (message->what == B_KEY_DOWN && fTimeline) {
        // The timeline takes the transport keys only when nothing else is
        // using them: a focused control (a text field being edited, a slider's
        // arrows) keeps its keys, and Tab always belongs to BWindow's own
        // focus navigation -- stealing it made the keyboard unusable. The
        // common cases this covers: nothing focused yet (a fresh window), or
        // the timeline itself after a click on it.
        BView* focus = CurrentFocus();
        const bool held = focus != nullptr && focus != fTimeline;
        int32 mods = 0;
        message->FindInt32("modifiers", &mods);
        const char* bytes = nullptr;
        const bool isTab = message->FindString("bytes", &bytes) == B_OK
                        && bytes && bytes[0] == B_TAB;
        if (!(mods & B_COMMAND_KEY) && !held && !isTab) {
            // Hand it straight to the timeline: a BMessage carries no
            // retarget verb (Haiku's BMessage has no SetTarget), and this is
            // precisely "the timeline gets the key instead of the focus view".
            fTimeline->MessageReceived(message);
            return;
        }
    }
    BWindow::DispatchMessage(message, handler);
}

void MainWindow::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case MSG_PLAY:  fTransportCtl.StartPlayback(); break;
        case MSG_STOP:
            fTransportCtl.StopPlayback();
            fRecCtl.StopRecording();
            break;
        case MSG_REC:
            // Toggle: Rec starts, Rec again (or Stop) finishes the take.
            if (fRecCtl.fRecMode) fRecCtl.StopRecording();
            else          fRecCtl.StartRecording();
            break;
        case kMsgMonitorRefresh:   // arming / input changed in the timeline
            fRecCtl.UpdateMidiMonitor();
            break;
        case kMsgTrackSelected: {  // point the inspector at the clicked track
            int64 tid = 0;
            msg->FindInt64("track", &tid);
            if (fInspector) fInspector->SetTrack((TrackId)tid);
            break;
        }
        case kMsgUiRefresh: {      // a track edit: keep the panes + mixer consistent
            if (fInspector) fInspector->Invalidate();
            if (fTimeline)  fTimeline->Invalidate();
            if (fMixerMsgr.IsValid()) {   // refresh an open mixer's strip state
                BMessage m(kMsgMixStrips);
                for (const MixerStripInfo& s : BuildMixerStrips(*fProject)) {
                    m.AddInt64("tid", (int64)s.trackId);
                    m.AddString("nm", s.name.c_str());
                    m.AddFloat("g", s.gain);  m.AddFloat("p", s.pan);
                    m.AddBool("mu", s.muted); m.AddBool("so", s.soloed);
                    m.AddBool("ar", s.armed); m.AddBool("mo", s.inputMonitor);
                    m.AddInt32("ci", s.colorIndex); m.AddInt32("ty", s.type);
                    // Count first, then this strip's inserts appended to the one
                    // flat run the mixer slices back apart in strip order.
                    m.AddInt32("fx", (int32)s.fx.size());
                    for (const MixerInsertInfo& ins : s.fx) {
                        m.AddString("fxn", ins.name.c_str());
                        m.AddBool("fxb", ins.bypassed);
                    }
                    m.AddInt32("sn", s.sendCount);
                    m.AddBool("hi", s.hasInput);    m.AddString("ol", s.outLabel.c_str());
                }
                m.AddFloat("mg", fProject->masterGain);
                fMixerMsgr.SendMessage(&m);
            }
            break;
        }
        case kMsgCycleAuto:        // inspector Auto button -> cycle timeline mode
            if (fInspector && fTimeline)
                fTimeline->CycleAuto(fInspector->SelectedTrack());
            break;
        case kMsgRollOpened: {     // a piano roll opened; remember it for the playhead
            BMessenger m;
            if (msg->FindMessenger("m", &m) == B_OK) fRollMsgr = m;
            break;
        }
        case kMsgSeek: {
            const Frame ph = fProject->transport.playhead;
            UpdateTimeReadout(ph);
            if (fTransportCtl.fPlaying)        // restart from the new position
                fTransportCtl.StartPlayback();
            break;
        }
        case MSG_ZOOM_IN:  fTimeline->ZoomBy(0.5); break;
        case MSG_ZOOM_OUT: fTimeline->ZoomBy(2.0); break;
        case MSG_MON_DIM:
            fTransportCtl.fMonDim = !fTransportCtl.fMonDim;
            if (fDimItem) fDimItem->SetMarked(fTransportCtl.fMonDim);
            if (fTransportCtl.fEngine) fTransportCtl.fEngine->SetMonitorDim(fTransportCtl.fMonDim);
            break;
        case MSG_MON_MONO:
            fTransportCtl.fMonMono = !fTransportCtl.fMonMono;
            if (fMonoItem) fMonoItem->SetMarked(fTransportCtl.fMonMono);
            if (fTransportCtl.fEngine) fTransportCtl.fEngine->SetMonitorMono(fTransportCtl.fMonMono);
            break;
        case MSG_COUNTIN: {
            int32 bars = 0;
            msg->FindInt32("bars", &bars);
            fRecCtl.fCountInBars = bars;
            if (fCountInMenu)
                for (int32 i = 0; i < fCountInMenu->CountItems(); i++)
                    if (BMenuItem* it = fCountInMenu->ItemAt(i))
                        it->SetMarked(it->Message()
                            && it->Message()->FindInt32("bars") == bars);
            break;
        }
        case MSG_MONITOR_IN:
            fTransportCtl.fMonitorInput = !fTransportCtl.fMonitorInput;
            if (fMonInItem) fMonInItem->SetMarked(fTransportCtl.fMonitorInput);
            if (fTimeline)  fTimeline->SetMonitorInput(fTransportCtl.fMonitorInput);  // lane "I" lamp
            // Live toggle while a take is running.
            if (fRecCtl.fRecorder) fRecCtl.fRecorder->SetMonitor(fTransportCtl.fMonitorInput);
            if (fTransportCtl.fEngine)   fTransportCtl.fEngine->SetInputMonitor(fTransportCtl.fMonitorInput);
            break;
        case MSG_METRONOME:
            fTransportCtl.fMetronome = !fTransportCtl.fMetronome;
            if (fMetItem) fMetItem->SetMarked(fTransportCtl.fMetronome);
            if (fTransportCtl.fEngine) fTransportCtl.fEngine->SetMetronome(fTransportCtl.fMetronome);
            break;
        case MSG_TOGGLE_INSPECTOR:
            SetInspectorShown(!fInspectorShown);
            break;
        case MSG_TOGGLE_DOCK: {
            const bool collapsed = fRootSplit->IsItemCollapsed(1);
            fRootSplit->SetItemCollapsed(1, !collapsed);
            if (fDockItem) fDockItem->SetMarked(collapsed);
            break;
        }
        case MSG_FOLLOW: {
            const bool on = !(fFollowItem && fFollowItem->IsMarked());
            if (fFollowItem) fFollowItem->SetMarked(on);
            fTimeline->SetFollow(on);
            break;
        }
        case kMsgTransportToggle:   // spacebar
            if (fTransportCtl.fPlaying || fRecCtl.fRecMode) { fTransportCtl.StopPlayback(); fRecCtl.StopRecording(); }
            else                        fTransportCtl.StartPlayback();
            break;
        case MSG_EXPORT:
            OpenExportWindow(false);   // mixdown; the dialog can switch to stems
            break;
        case MSG_EXPORT_STEMS:
            OpenExportWindow(true);
            break;
        case kMsgExportOptions: {
            // The dialog's choices. Remembered (AppSettings persists them at
            // quit) and answered with the panel that matches, so the format is
            // settled before a destination is asked for.
            int32 v = 0; bool b = false; float f = 0.0f;
            if (msg->FindInt32("bits", &v) == B_OK) fExportChoices.bitDepth = v;
            if (msg->FindBool("dither", &b) == B_OK) fExportChoices.dither = b;
            if (msg->FindInt32("rate", &v) == B_OK) fExportChoices.sampleRate = v;
            if (msg->FindBool("norm", &b) == B_OK) fExportChoices.normalize = b;
            if (msg->FindFloat("lufs", &f) == B_OK) fExportChoices.targetLufs = f;
            if (msg->FindFloat("ceil", &f) == B_OK) fExportChoices.truePeak = f;
            if (msg->FindBool("lim", &b) == B_OK) fExportChoices.limiter = b;
            if (msg->FindInt32("range", &v) == B_OK) fExportChoices.range = v;
            if (msg->FindInt32("stems", &v) == B_OK) fExportChoices.stems = (v != 0);

            if (fExportChoices.stems) {
                if (!fStemsPanel) {
                    BMessenger to(this);
                    fStemsPanel = new BFilePanel(B_SAVE_PANEL, &to, NULL, 0, false,
                                                 new BMessage(MSG_EXPORT_STEMS_REF));
                    fStemsPanel->SetSaveText("stems");   // subfolder name
                }
                fStemsPanel->Show();
            } else {
                if (!fExportPanel) {
                    BMessenger to(this);
                    fExportPanel = new BFilePanel(B_SAVE_PANEL, &to, NULL, 0, false,
                                                  new BMessage(MSG_EXPORT_REF));
                }
                fExportPanel->Show();
            }
            break;
        }
        case MSG_EXPORT_REF: {
            entry_ref dir; const char* name = nullptr;
            if (msg->FindRef("directory", &dir) == B_OK
                && msg->FindString("name", &name) == B_OK) {
                BPath path(&dir);
                path.Append(name);
                fRender.StartExport(path.Path(), false);
            }
            break;
        }
        case MSG_EXPORT_STEMS_REF: {
            entry_ref dir; const char* name = nullptr;
            if (msg->FindRef("directory", &dir) == B_OK
                && msg->FindString("name", &name) == B_OK) {
                BPath base(&dir);
                BPath stemDir(base.Path(), name && name[0] ? name : "stems");
                create_directory(stemDir.Path(), 0755);
                fRender.StartExport(stemDir.Path(), true);
            }
            break;
        }
        case kMsgExportCancel:
            // The progress window's button. The worker polls this between
            // blocks; the completion path (on the pulse) closes the window, so
            // the bar never disappears before the outcome is known.
            fRender.fExportCancel.store(true);
            break;
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
            if (fTransportCtl.fEngine) fTransportCtl.fEngine->SetMeterFocus((TrackId)tid);
            break;
        }
        case kMsgFxWinClosed: {
            int64 tid = 0;
            msg->FindInt64("track", &tid);
            if (fFxTrack == (TrackId)tid) {
                fFxMsgr = BMessenger();
                fFxTrack = kInvalidTrackId;
                if (fTransportCtl.fEngine) fTransportCtl.fEngine->SetMeterFocus(kInvalidTrackId);
            }
            break;
        }
        case MSG_MIXER: {
            std::vector<MixerStripInfo> strips = BuildMixerStrips(*fProject);
            const float ww = 24 + (strips.size() + 1) * (96 + 4);   // + master
            // Tall enough for the strip, including the insert block: four insert
            // rows cost ~60 px that the old single "FX n" button did not.
            BRect wr(120, 90, 120 + ww, 90 + 620);
            MixerWindow* mx = new MixerWindow(wr, strips, fProject->masterGain,
                                              BMessenger(this));
            fMixerMsgr = BMessenger(mx);
            mx->Show();
            break;
        }
        case kMsgMixArm: {
            int64 tid = 0; msg->FindInt64("track", &tid);
            if (Track* t = fProject->FindTrack((TrackId)tid)) t->armed = !t->armed;
            fRecCtl.UpdateMidiMonitor();
            if (fInspector) fInspector->Invalidate();
            fTimeline->Invalidate();
            break;
        }
        case kMsgMixMon: {
            int64 tid = 0; msg->FindInt64("track", &tid);
            if (Track* t = fProject->FindTrack((TrackId)tid))
                t->inputMonitor = !t->inputMonitor;
            fRecCtl.UpdateMidiMonitor();
            if (fInspector) fInspector->Invalidate();
            fTimeline->Invalidate();
            break;
        }
        case kMsgMixFx: {
            // "slot" is present when a strip row was clicked: the editor then
            // opens on that insert alone. Absent (the overflow row, or an older
            // sender) means the whole chain.
            int64 tid = 0; int32 slot = -1;
            msg->FindInt64("track", &tid);
            msg->FindInt32("slot", &slot);
            Track* t = fProject->FindTrack((TrackId)tid);
            if (!t) break;
            // The mixer names the slot from a snapshot, so the chain may have
            // shrunk since. Resolve that HERE, against the real chain: both
            // consumers below happen to clamp an out-of-range focus to "whole
            // chain" already, but relying on that leaves the decision in two
            // distant places and neither of them can see the model.
            if (slot >= (int32)t->fx.size()) slot = -1;
#ifdef DAW_HAVE_LV2
            // A plugin that ships its own editor opens THAT, exactly as the
            // inspector's slot list does -- the two strips have to behave the
            // same or the mixer looks broken by comparison. A null return means
            // an editor for this plugin is already up and was raised.
            // The editor is told which insert it belongs to, so it can drive it
            // live through fStack's model and this window's engine -- the same
            // (track, fx, slot) address every other live edit uses.
            if (slot >= 0 && OpenNativeEditor((TrackId)tid, (int)slot))
                break;
#endif
            (new EffectsWindow(BRect(200, 150, 680, 770), t->fx,
                               (TrackId)tid, BMessenger(this),
                               (int)slot))->Show();
            break;
        }
        case kMsgMixFxBypass: {
            // The mixer names an insert by index; the command is run here, where
            // the descriptors live. A discrete toggle, so it gets the narrow
            // command rather than a whole-chain replace -- the Edit menu then
            // reads "Bypass Effect".
            int64 tid = 0; int32 idx = -1;
            msg->FindInt64("track", &tid);
            msg->FindInt32("fx", &idx);
            Track* t = fProject->FindTrack((TrackId)tid);
            if (!t || idx < 0 || idx >= (int32)t->fx.size()) break;
            fStack->Execute(std::make_unique<SetFxBypassCommand>(
                (TrackId)tid, (int)idx, !t->fx[(size_t)idx].bypassed), *fProject);
            SyncFxToEngine();
            PostMessage(kMsgUiRefresh);   // inspector + the mixer's own snapshot
            break;
        }
        case kMsgMixFxMove: {
            int64 tid = 0; int32 from = -1, to = -1;
            msg->FindInt64("track", &tid);
            msg->FindInt32("from", &from);
            msg->FindInt32("to", &to);
            Track* t = fProject->FindTrack((TrackId)tid);
            if (!t) break;
            const int32 n = (int32)t->fx.size();
            if (from < 0 || from >= n || to < 0 || to >= n || from == to) break;
            // A reorder is only a permutation of the descriptor vector, so it
            // goes through the ordinary chain-replace command and is one undo
            // step -- the same path the inspector's drag uses.
            std::vector<EffectDesc> chain = t->fx;
            EffectDesc moved = chain[(size_t)from];
            chain.erase(chain.begin() + from);
            chain.insert(chain.begin() + to, moved);
            fStack->Execute(std::make_unique<SetFxCommand>(
                (TrackId)tid, false, std::move(chain)), *fProject);
            SyncFxToEngine();
            PostMessage(kMsgUiRefresh);
            break;
        }
        case kMsgMixFxAdd: {
            // The browser posts its choice to the INSPECTOR, which already turns
            // a chosen plugin into a SetFxCommand against whichever track the
            // message names -- it reads the id from the message precisely so a
            // browser can outlive the selection it was opened from. Routing it
            // there beats a second copy of the same handler.
            int64 tid = 0; msg->FindInt64("track", &tid);
            if (!fInspector || !fProject->FindTrack((TrackId)tid)) break;
            (new PluginBrowser(BRect(240, 190, 700, 590), (TrackId)tid,
                               BMessenger(fInspector), BMessenger(this)))->Show();
            break;
        }
        case kMsgMixSends: {
            int64 tid = 0; msg->FindInt64("track", &tid);
            if (Track* t = fProject->FindTrack((TrackId)tid)) {
                std::vector<std::pair<TrackId, std::string>> buses;
                for (const Track& bt : fProject->Tracks())
                    if (bt.type == TrackType::Bus && bt.id != (TrackId)tid)
                        buses.push_back({bt.id, bt.name});
                (new SendsWindow(BRect(200, 150, 540, 470), t->sends, buses,
                                 (TrackId)tid, BMessenger(this)))->Show();
            }
            break;
        }
        case kMsgMixInst: {
            int64 tid = 0; msg->FindInt64("track", &tid);
            if (Track* t = fProject->FindTrack((TrackId)tid))
                (new InstrumentWindow(BRect(200, 150, 480, 340), t->instrument,
                                      (TrackId)tid, BMessenger(this)))->Show();
            break;
        }
        case kMsgMixSelect: {
            int64 tid = 0; msg->FindInt64("track", &tid);
            if (fInspector) fInspector->SetTrack((TrackId)tid);
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
                fStack->Execute(std::make_unique<SetMidiClipNotesCommand>(
                    (TrackId)tid, (ClipId)cid, ParseNoteList(msg)), *fProject);
                fTimeline->Invalidate();
            }
            break;
        }
        case kMsgApplyMidiOp: {
            // A PianoRoll ran a named transform (quantize / humanize / legato /
            // transpose / velocity) on its snapshot and posted the result. The
            // command only records it -- see MidiOps.h for why the parameters
            // do not travel -- and names the undo step after the transform.
            int64 tid = 0, cid = 0; int32 op = 0;
            msg->FindInt64("track", &tid);
            msg->FindInt64("clip", &cid);
            msg->FindInt32("op", &op);
            Track* tr = fProject->FindTrack((TrackId)tid);
            if (tr && tr->FindMidiClip((ClipId)cid)) {
                fStack->Execute(std::make_unique<ApplyMidiOpCommand>(
                    (TrackId)tid, (ClipId)cid, ParseNoteList(msg),
                    MidiOpName((MidiOp)op)), *fProject);
                fTimeline->Invalidate();
            }
            break;
        }
        case kMsgApplyEvents: {
            // The piano roll's CC lane posts one region's controller list.
            // Undoable via SetMidiClipEventsCommand, which leaves notes alone.
            int64 tid = 0, cid = 0;
            msg->FindInt64("track", &tid);
            msg->FindInt64("clip", &cid);
            Track* tr = fProject->FindTrack((TrackId)tid);
            if (tr && tr->FindMidiClip((ClipId)cid)) {
                std::vector<MidiClipEvent> events;
                int32 type = 0;
                for (int32 i = 0; msg->FindInt32("et", i, &type) == B_OK; i++) {
                    // Every field must be present and in range. A partial or
                    // out-of-range event would land as a CC0 at frame 0 — which
                    // no lane draws, so it would be invisible in the editor yet
                    // still saved into the project and written to any SMF
                    // export. Drop it instead of persisting a ghost.
                    int32 data = 0, val = 0; int64 st = 0;
                    if (msg->FindInt32("ed", i, &data) != B_OK) continue;
                    if (msg->FindInt32("ev", i, &val)  != B_OK) continue;
                    if (msg->FindInt64("es", i, &st)   != B_OK) continue;
                    if (type < MidiClipEvent::CC
                        || type > MidiClipEvent::ChannelPressure) continue;
                    if (data < 0 || data > 127) continue;
                    const int32 vmax =
                        (type == MidiClipEvent::PitchBend) ? 16383 : 127;
                    if (val < 0 || val > vmax) continue;
                    if (st < 0) continue;      // clip-relative; negative is junk
                    MidiClipEvent e;
                    e.type = type; e.data = data; e.value = val;
                    e.startFrame = (Frame)st;
                    events.push_back(e);
                }
                fStack->Execute(std::make_unique<SetMidiClipEventsCommand>(
                    (TrackId)tid, (ClipId)cid, std::move(events)), *fProject);
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
        case kMsgRenameMarker: {
            int64 fr = 0; const char* name = nullptr; const char* oldName = nullptr;
            msg->FindInt64("track", &fr);               // "track" carries the frame
            if (msg->FindString("name", &name) == B_OK && name) {
                // Disambiguate by the pre-edit name when two markers share a frame.
                if (msg->FindString("oldname", &oldName) == B_OK && oldName)
                    fStack->Execute(std::make_unique<RenameMarkerCommand>(
                        (Frame)fr, oldName, name), *fProject);
                else
                    fStack->Execute(std::make_unique<RenameMarkerCommand>(
                        (Frame)fr, name), *fProject);
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
                // One macro, so the whole strip -- gain, pan, mute, solo and the
                // mute-group cascade -- is one undo step. Unchanged fields get
                // no sub-command at all.
                auto macro = std::make_unique<MacroCommand>("Mixer Strip");
                if (t->gain   != g)  macro->Add(std::make_unique<SetTrackGainCommand>((TrackId)tid, g));
                if (t->pan    != p)  macro->Add(std::make_unique<SetTrackPanCommand>((TrackId)tid, p));
                if (t->muted  != mu) macro->Add(std::make_unique<SetTrackMuteCommand>((TrackId)tid, mu));
                if (t->soloed != so) macro->Add(std::make_unique<SetTrackSoloCommand>((TrackId)tid, so));
                if (t->muteGroup > 0 && t->muted != mu) {
                    const int grp = t->muteGroup;
                    for (const Track& o : fProject->Tracks())
                        if (o.muteGroup == grp && o.id != t->id && o.muted != mu)
                            macro->Add(std::make_unique<SetTrackMuteCommand>(o.id, mu));
                }
                if (!macro->Empty()) {
                    fStack->Execute(std::move(macro), *fProject);
                    UpdateTitle();
                }
                // Refresh the other strips/panes so the change shows live.
                if (t->muteGroup > 0)
                    PostMessage(kMsgUiRefresh);
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
            // One decoder for this layout, shared with the push in the other
            // direction (kMsgFxChain), so the two cannot drift field by field.
            fStack->Execute(std::make_unique<SetFxCommand>(
                (TrackId)tid, master, DecodeFxChain(*msg)), *fProject);
            SyncFxToEngine();
            fTimeline->Invalidate();
            break;
        }
        case kMsgFxChanged:      // a channel strip committed a chain edit
            SyncFxToEngine();
            break;
        case kMsgReloadEngine:   // clip/fade edit: rebuild so it takes effect live
            fTransportCtl.ReloadActiveEngine();
            break;
        case kMsgFxParamCommit: {
            // A native plugin editor's committed gesture: the values it already
            // played through kMsgFxLive, made permanent in the model so an
            // engine rebuild (and the saved project) keeps them.
            int64 tid = 0; int32 fx = -1;
            msg->FindInt64("track", &tid);
            msg->FindInt32("fx", &fx);
            if (fx < 0) break;
            std::vector<SetFxParamCommand::SlotValue> vals;
            for (int32 i = 0;; i++) {
                int32 slot = 0; float v = 0.0f;
                if (msg->FindInt32("slot", i, &slot) != B_OK) break;
                if (msg->FindFloat("val", i, &v) != B_OK) break;
                vals.push_back({ slot, v });
            }
            if (vals.empty()) break;
            const bool master = ((TrackId)tid == kMasterFxTarget);
            // The editor's insert may be gone, or a different plugin may have
            // moved into its index since it sent this: the commit arrives on
            // this thread, after the editor has already closed itself (a
            // reloaded project, a removed insert). Applying it then would write
            // one plugin's parameter values into another's descriptor -- as an
            // undoable edit, saved to disk. The URI is what tells them apart.
            BString uri;
            msg->FindString("uri", &uri);
            const Track* t = master ? nullptr : fProject->FindTrack((TrackId)tid);
            const std::vector<EffectDesc>* chain = master ? &fProject->masterFx
                                                : (t ? &t->fx : nullptr);
            if (!chain || fx >= (int32)chain->size()
                || (*chain)[(size_t)fx].pluginName != uri.String())
                break;   // not this editor's insert any more: not ours to edit
            if (fStack->Execute(std::make_unique<SetFxParamCommand>(
                    (TrackId)tid, master, fx, std::move(vals)), *fProject)) {
                // Deliberately no engine sync here. The audio already has these
                // values -- they arrived live before this message did -- and
                // SyncFxToEngine rebuilds the chain, which would cut reverb
                // tails and restart the insert to re-apply what it is playing.
                // An OPEN generic panel, though, still holds the chain from
                // before this commit: left stale, its next Apply() would write
                // the old values back and silently undo this edit.
                PushChainToFxWindow((TrackId)tid);
                fTimeline->Invalidate();
            }
            break;
        }
        case kMsgFxWatch: {
            // A native editor registering the insert it is showing, or (no
            // messenger) saying it is closing. Identified by (uri, track, fx) --
            // the address the editor itself writes through -- so one editor's
            // close can never drop another's watch.
            int64 tid = 0; int32 fx = -1;
            msg->FindInt64("track", &tid);
            msg->FindInt32("fx", &fx);
            BString uri;
            msg->FindString("uri", &uri);
            BMessenger msgr;
            msg->FindMessenger("msgr", &msgr);

            // Drop this editor's entry on close, and any entry whose window died
            // without saying so (closed by the window manager, or crashed). The
            // match itself is the table's rule (FxWatchFind), so the identity an
            // editor registers under and the identity a close is matched by
            // cannot drift apart.
            const std::vector<FxWatch> snapshot = FxWatchSnapshot();
            const int match = FxWatchFind(snapshot, uri.String(), (TrackId)tid, fx);
            for (size_t i = fFxWatches.size(); i > 0; --i) {
                const FxEntry& w = fFxWatches[i - 1];
                const bool same = (match >= 0 && (size_t)match == i - 1);
                if ((same && !msgr.IsValid())
                    || (w.editor && !w.editor->Alive())) {
                    if (fTransportCtl.fEngine && w.slot >= 0)
                        fTransportCtl.fEngine->SetFxWatch(w.slot, kInvalidTrackId, false, -1);
                    fFxWatches.erase(fFxWatches.begin() + (long)(i - 1));
                }
            }
            if (msgr.IsValid() && fx >= 0) {
                const int slot = FxWatchFreeSlot(FxWatchSnapshot(),
                                                 Engine::kWatchSlots);
                FxEntry e;
                e.editor = std::make_shared<EditorHandle>(msgr);
                e.uri    = uri.String();
                e.track  = (TrackId)tid;
                e.fx     = fx;
                e.slot   = slot;
                e.gen    = FxWatch::kForcePush;   // forces the first frame out
                fFxWatches.push_back(e);
                if (slot >= 0) ApplyFxWatchToEngine(fFxWatches.back());
                else std::fprintf(stderr,
                    "daw: more than %d native editors open; '%s' will not "
                    "follow automation\n", Engine::kWatchSlots, e.uri.c_str());
            }
            UpdatePulse();
            break;
        }
        case kMsgOpenFxEditor: {
            // An editor view asking for the plugin's own GUI. It posts rather
            // than opening the window itself because the view's copy of the
            // chain can be stale: the insert at index N there may not be the
            // insert at index N in the model, and a live editor addresses its
            // insert by index. Only this window sees the model.
            int64 tid = 0; int32 fx = -1;
            msg->FindInt64("track", &tid);
            msg->FindInt32("fx", &fx);
            if (OpenNativeEditor((TrackId)tid, fx))
                break;
            // No editor of its own will appear: the plugin has none, or the
            // chain holds it twice and its editor cannot tell the copies apart
            // (the exact case OpenNativeEditor names "use the parameter list").
            // Senders that are ALREADY a generic panel set "fallback" false --
            // there the panel simply stays as it is.
            if (!msg->GetBool("fallback", false))
                break;
            Track* t = fProject->FindTrack((TrackId)tid);
            if (!t) break;
            if (fx >= (int32)t->fx.size()) fx = -1;   // stale index: whole chain
            (new EffectsWindow(BRect(200, 150, 680, 770), t->fx,
                               (TrackId)tid, BMessenger(this), fx))->Show();
            break;
        }
        case kMsgFxLive: {   // live knob-drag preview into the running engine
            int64 tid = 0; int32 fx = 0, slot = 0; float v = 0.0f;
            msg->FindInt64("track", &tid);
            msg->FindInt32("fx", &fx);
            msg->FindInt32("slot", &slot);
            msg->FindFloat("val", &v);
            if (fTransportCtl.fEngine) {
                fTransportCtl.fEngine->SetFxParamLive((TrackId)tid,
                    (TrackId)tid == kMasterFxTarget, fx, slot, v);
                // With the transport stopped there is no audio block to publish
                // the change, so an open native editor on this insert would
                // keep showing the old value. While playing, this returns at
                // once and the block publishes it.
                fTransportCtl.fEngine->PublishFxWatchNow();
            }
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
            // coalescing SetInstrumentCommand, and pushed straight into the
            // running engine below so it is heard immediately.
            // A soundfont is already resident in the SoundfontCache by now —
            // the editor loaded it on its own looper before posting — so this
            // handler never touches the disk.
            int64 tid = 0;
            msg->FindInt64("track", &tid);
            if (fProject->FindTrack((TrackId)tid)) {
                InstrumentDesc in;
                int32 wv = 0, ty = 0, ps = 0; float a = 0, d = 0, s = 0, r = 0;
                const char* path = nullptr;
                msg->FindInt32("type", &ty);
                msg->FindInt32("wave", &wv);
                msg->FindFloat("a", &a); msg->FindFloat("d", &d);
                msg->FindFloat("s", &s); msg->FindFloat("r", &r);
                msg->FindInt32("preset", &ps);
                msg->FindString("path", &path);
                in.type = (ty >= 0 && ty <= kMaxInstrumentTypeId)
                              ? (InstrumentType)ty : InstrumentType::Synth;
                in.synth.waveform = (wv >= 0 && wv <= 3) ? wv : 0;
                in.synth.attack = a; in.synth.decay = d;
                in.synth.sustain = s; in.synth.release = r;
                in.path = path ? path : "";
                in.sf2Preset = ps > 0 ? ps : 0;
                if (in.path.empty()) in.type = InstrumentType::Synth;
                fStack->Execute(std::make_unique<SetInstrumentCommand>(
                    (TrackId)tid, in), *fProject);
                // Apply it to the running engine, the way an effect change
                // does. A voice swap is always structural — there is no
                // in-place equivalent of SyncFx for an instrument — so rebuild.
                // The soundfont is already in the cache (the editor loaded it
                // before posting), so this is a cheap graph rebuild, not a
                // decode. Without this the edit sat in the model and was only
                // heard after the next manual stop/play.
                fTransportCtl.ReloadActiveEngine();
                fTimeline->Invalidate();
            }
            break;
        }
        case MSG_ZOOMFIT:
            fTimeline->ZoomToFit();
            break;
        case MSG_ABOUT: {
            // A BAlert is the whole About box: name, version, what it is built
            // on. The version comes from the generated header, so there is
            // nothing here to keep in step with CMakeLists.
            char text[256];
            std::snprintf(text, sizeof(text),
                          "Haiku DAW %s\n\nA native digital audio workstation "
                          "for Haiku:\nmultitrack audio and MIDI, mixing, "
                          "automation,\nLV2 plugins, and offline export.",
                          DAW_VERSION_STRING);
            BAlert* a = new BAlert("About Haiku DAW", text, "OK", nullptr,
                                   nullptr, B_WIDTH_AS_USUAL, B_INFO_ALERT);
            a->Go(nullptr);   // async, like the other alerts
            break;
        }
        case MSG_AUTOSAVE: {
            // Save a recovery copy while there's content and we're not mid-take.
            if (!fRecCtl.fRecMode && !fProject->Tracks().empty()) {
                // A knob moved in a native editor is audible at once but reaches
                // the model only when its debounce goes quiet, so recover from
                // what is PLAYING, not from a value the user has already moved
                // past (the same reason SaveTo flushes).
                FlushFxEditors();
                BPath p;
                if (fDoc.RecoveryPath(p)) ProjectIO::Save(*fProject, p.Path());
            }
            break;
        }
        case MSG_RECOVER: {
            BPath p;
            BEntry e;
            if (fDoc.RecoveryPath(p) && (e.SetTo(p.Path()), e.Exists())) {
                BAlert* a = new BAlert("Recover",
                    "Unsaved work from a previous session was found. Recover it?",
                    "Discard", "Recover");
                if (a->Go() == 1) LoadFrom(p.Path(), /*asRecovery*/ true);
                else              fDoc.RemoveRecoveryFile();
            }
            break;
        }
        case MSG_TITLE:
            UpdateTitle();   // cheap; SetTitle only fires on an actual change
            break;
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
        case kMsgDropMidi: {   // a .mid dropped on the timeline
            const char* path = nullptr;
            int64 start = 0;
            if (msg->FindString("path", &path) == B_OK && path) {
                msg->FindInt64("start", &start);
                ImportMidi(path, (Frame)start);
            }
            break;
        }
        case kMsgRegionNormalize: case kMsgRegionReverse: case kMsgRegionStrip: {
            int64 tid = 0, cid = 0;
            if (msg->FindInt64("track", &tid) != B_OK) break;
            msg->FindInt64("clip", &cid);
            if (msg->what == kMsgRegionNormalize)
                fRender.RegionNormalize((TrackId)tid, (ClipId)cid);
            else if (msg->what == kMsgRegionReverse)
                fRender.RegionReverse((TrackId)tid, (ClipId)cid);
            else
                fRender.RegionStripSilence((TrackId)tid, (ClipId)cid);
            break;
        }
        case kMsgFreezeTrack: {
            int64 tid = 0; bool freeze = true;
            if (msg->FindInt64("track", &tid) != B_OK) break;
            msg->FindBool("freeze", &freeze);
            fRender.FreezeTrack((TrackId)tid, freeze);
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
            // Through the stack like every other edit: a drag's posts coalesce
            // into one undo step. The engine still reads project.masterGain
            // each poll, so the move is heard while the slider moves.
            fStack->Execute(std::make_unique<SetMasterGainCommand>(
                                (float)(fMaster->Value() / 100.0)), *fProject);
            UpdateTitle();
            break;
        // Undo/redo can change anything the running engine was built from — an
        // instrument, an fx chain, a clip, the tempo. The forward paths reload
        // the engine for those; without the same call here the model and the
        // audio disagree until the next manual stop/play.
        case MSG_UNDO:
            if (fStack->CanUndo()) {
                fStack->Undo(*fProject);
                // An undo can move or remove the insert an open native editor is
                // showing, and with the transport stopped ReloadActiveEngine
                // does nothing at all -- so this cannot wait for a rebuild.
                ValidateFxWatch();
                fTransportCtl.ReloadActiveEngine();   // brings the engine to the model's state
                PublishFxParamsNow();   // ... and only then republish
                // Same staleness rule as a native commit: an undo rewrites the
                // chain under an open panel's copy.
                PushChainToFxWindow(fFxTrack);
                fTimeline->Invalidate();
                UpdateTitle();
            }
            break;
        case MSG_REDO:
            if (fStack->CanRedo()) {
                fStack->Redo(*fProject);
                ValidateFxWatch();
                fTransportCtl.ReloadActiveEngine();   // brings the engine to the model's state
                PublishFxParamsNow();   // ... and only then republish
                PushChainToFxWindow(fFxTrack);
                fTimeline->Invalidate();
                UpdateTitle();
            }
            break;
        case MSG_PASTE:
            fTimeline->PasteAtPlayhead();
            break;
        case MSG_BUFFER: {
            int32 frames = 512;
            msg->FindInt32("frames", &frames);
            fTransportCtl.fBufferFrames = (size_t)frames;   // applied at the next Play
            break;
        }
        case MSG_TEMPO: {
            double bpm = atof(fTempo->Text());
            if (bpm < 20.0)  bpm = 20.0;
            if (bpm > 300.0) bpm = 300.0;
            // The BPM field edits the tempo map's frame-0 (initial) tempo, now
            // as an undoable step (the command sets both the map and tempoBPM).
            fStack->Execute(std::make_unique<SetTempoCommand>(bpm), *fProject);
            UpdateTitle();
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f", bpm);
            fTempo->SetText(buf);
            fTimeline->Invalidate();   // grid + ruler follow tempo
            // Rebuild the running engine so the metronome + tempo-synced effects
            // (delay) pick up the new tempo live; stopped, it applies next Play.
            fTransportCtl.ReloadActiveEngine();
            break;
        }
        case MSG_NEW_PROJECT:
            NewProject();
            break;
        case MSG_SAVE:
        case MSG_SAVE_AS:
            // Save goes straight to the project's own file once it has one --
            // Save As… is the way to move it. Both ask for a path when there
            // is none, through the same panel.
            if (msg->what == MSG_SAVE && fDoc.HasPath()) {
                SaveTo(fDoc.Path().c_str());
                break;
            }
            if (!fSavePanel) {
                BMessenger to(this);
                fSavePanel = new BFilePanel(B_SAVE_PANEL, &to, NULL, 0, false,
                                            new BMessage(MSG_SAVE_REF));
            }
            fSavePanel->Show();
            break;
        case MSG_RELINK_REF: {
            entry_ref ref;
            if (msg->FindRef("refs", &ref) == B_OK) {
                BPath p(&ref);
                for (RelinkEntry& e : fRelinkQueue)
                    if (e.newPath.empty()) {
                        if (p.InitCheck() == B_OK) e.newPath = p.Path();
                        break;
                    }
            }
            RelinkNext();     // the next file, or the end of the walk
            break;
        }
        case MSG_CLOSE:
            // The window's own quit request: whatever Cmd-Q and the title-bar
            // button do (the prompt included) happens here too.
            PostMessage(B_QUIT_REQUESTED);
            break;
        case MSG_OPEN_RECENT: {
            const char* path = nullptr;
            if (msg->FindString("path", &path) != B_OK || !path)
                break;
            BEntry e(path);
            if (!e.Exists()) {
                // Prune rather than hand a vanished file to LoadFrom: the list
                // is a convenience, and a dead entry only ever fails.
                std::fprintf(stderr, "daw: recent project is gone: %s\n", path);
                ForgetRecent(path);
                break;
            }
            LoadFrom(path);
            break;
        }
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
            // An export reports through the pulse: the worker writes only
            // atomics (the codebase's pattern for streamed values), and the bar
            // is a window this looper owns a messenger to.
            if (!fRender.fExportHandled) {
                if (fRender.fExportRunning.load(std::memory_order_acquire)) {
                    const float f =
                        fRender.fExportProgress.load(std::memory_order_relaxed);
                    if (f >= 0.0f && fRender.fExportProgMsgr.IsValid()) {
                        BMessage pm(kMsgExportProgress);
                        pm.AddFloat("f", f);
                        fRender.fExportProgMsgr.SendMessage(&pm);
                    }
                } else {
                    fRender.FinishExport();
                }
            }
            // Before any branch: an open native editor is fed on every pulse,
            // including while stopped (see PushFxParams).
            PushFxParams();
            if (fTransportCtl.fEngine && fTransportCtl.fMonitoring && !fTransportCtl.fPlaying && !fRecCtl.fRecMode) {
                // Idle live-monitoring: apply live gain/pan/mute edits, then
                // drive the meters (no playhead / transport).
                fTransportCtl.fEngine->UpdateMix(*fProject);
                fMeter->SetLevels(fTransportCtl.fEngine->PeakL(), fTransportCtl.fEngine->PeakR());
                PushTrackPeaks();
                break;
            }
            if (fTransportCtl.fEngine && (fTransportCtl.fPlaying || fRecCtl.fRecMode)) {
                fTransportCtl.fEngine->UpdateMix(*fProject);   // live gain/pan/mute/solo
                const Frame ph = fTransportCtl.fEngine->Playhead();
                const Transport& tr = fProject->transport;

                if (fRecCtl.fRecMode) {
                    // The disk thread's failure, seen here (the pulse is the
                    // only place that may touch the UI): report once and stop
                    // the take -- FramesWritten lands what was written, so a
                    // full disk costs the take's tail, not the take.
                    if (fRecCtl.fRecorder && fRecCtl.fRecorder->ErrorCode() != 0) {
                        const int code = fRecCtl.fRecorder->ErrorCode();
                        fRecCtl.StopRecording();
                        ReportError("Recording",
                            code == 1
                                ? std::string("The take file could not be "
                                              "created. The take was stopped.")
                                : std::string("Writing the take failed -- the "
                                              "disk may be full. The take was "
                                              "stopped; what was recorded is "
                                              "kept."));
                        break;
                    }
                    // Count-in over: begin capture once we reach the record point.
                    if (fRecCtl.fCapturePending && ph >= fRecCtl.fRecPoint)
                        fRecCtl.StartCapture();
                    // Drain live MIDI into the take (stamped by kit timestamp,
                    // independent of this ~60 Hz poll's granularity).
                    if (fRecCtl.fMidiIn && !fRecCtl.fCapturePending) {
                        MidiEvent ev[64];
                        std::size_t n;
                        while ((n = fRecCtl.fMidiIn->ReadEvents(ev, 64)) > 0)
                            for (std::size_t i = 0; i < n; i++) {
                                Frame mf = fRecCtl.fRecStart
                                    + (Frame)((double)(ev[i].timeUs - fRecCtl.fMidiT0)
                                              * 1e-6 * fProject->sampleRate);
                                if (mf < fRecCtl.fRecStart) mf = fRecCtl.fRecStart;
                                fRecCtl.FeedMidiEvent(ev[i], mf);
                            }
                    }
                    // Loop-record: at the loop end, rewind the engine to the
                    // loop start (the recorder keeps capturing across the seam).
                    if (fRecCtl.fLoopRecord && tr.loopEnabled && ph >= tr.loopEnd) {
                        fProject->transport.playhead = tr.loopStart;
                        if (!fTransportCtl.StartRecordEngine(tr.loopStart)) {
                            std::fprintf(stderr, "MainWindow: loop-record engine "
                                                 "restart failed; stopping\n");
                            fRecCtl.StopRecording();
                        }
                        break;
                    }
                    const bool capturing = fRecCtl.fRecorder && fRecCtl.fRecorder->IsRecording();
                    const bool midiCap   = fRecCtl.fMidiIn && !fRecCtl.fCapturePending;
                    fTimeline->SetPlayhead(ph);
                    PushRollPlayhead(ph);
                    UpdateTimeReadout(ph);
                    if (capturing || midiCap) {
                        // Growing recording region + its live content.
                        fTimeline->SetRecording(true, fRecCtl.fRecStart, ph - fRecCtl.fRecStart);
                        if (capturing)
                            fTimeline->SetLiveAudio(fRecCtl.fRecorder.get(), fProject->sampleRate);
                        if (midiCap) {
                            std::map<TrackId, std::vector<MidiNote>> live;
                            for (TrackId id : fRecCtl.fMidiRecTracks)
                                live[id] = fRecCtl.fMidiRecs[id].SnapshotNotes(ph);
                            fTimeline->SetLiveMidiNotes(std::move(live));
                        }
                    }
                    fMeter->SetLevels(capturing ? fRecCtl.fRecorder->PeakL() : fTransportCtl.fEngine->PeakL(),
                                      capturing ? fRecCtl.fRecorder->PeakR() : fTransportCtl.fEngine->PeakR());
                    PushTrackPeaks();
                    PushFxMeters();
                    break;
                }

                // Playback loop: restart at the loop start past the loop end.
                if (tr.loopEnabled && tr.loopEnd > tr.loopStart
                    && ph >= tr.loopEnd) {
                    fProject->transport.playhead = tr.loopStart;
                    fTransportCtl.StartPlayback();
                    break;
                }
                fTimeline->SetPlayhead(ph);
                PushRollPlayhead(ph);
                UpdateTimeReadout(ph);
                fMeter->SetLevels(fTransportCtl.fEngine->PeakL(), fTransportCtl.fEngine->PeakR());
                UpdateLoudnessReadout(fTransportCtl.fEngine->LufsMomentary(),
                                      fTransportCtl.fEngine->LufsShort(),
                                      fTransportCtl.fEngine->TruePeakDb());
                PushTrackPeaks();
                PushFxMeters();
                if (fTransportCtl.fEngine->IsFinished())
                    fTransportCtl.StopPlayback();
            }
            break;
        }
        default:
            BWindow::MessageReceived(msg);
    }
}

// Fit the transport bar's controls to whatever width it currently has.
//
// The bar itself follows the window (B_FOLLOW_LEFT_RIGHT) and the master meter
// follows its right edge, but everything between them was placed at absolute x
// for the width the window happened to open at -- so a narrower window slid the
// meter left onto the BPM field and clipped the loudness readout off the edge,
// which reads as a drawing fault rather than as a smaller window. This is what
// the window's FrameResized calls; it is also what the constructor calls, since
// a restored window frame resizes the bar before the user ever touches it.
//
// The right cluster is pinned to the right edge in the design's own offsets, so
// the standard-size layout is unchanged to the pixel. Optional readouts are
// dropped, in order of how little they are missed, before anything is allowed
// to overlap: the loudness numbers first, then the tempo field, then the master
// slider (the mixer window still has it). The transport controls, the time
// readout and the meter are never hidden.
void MainWindow::LayoutTransportBar() {
    if (!fTransport) return;
    const float W = fTransport->Bounds().right + 1.0f;

    // Design offsets, from the original 1000-wide layout: meter 130 from the
    // right edge, loudness 278, BPM field ending at 704, slider ending at 588.
    const float kGap     = Themed(12.0f);
    const float kMeterW  = Themed(124.0f);
    const float kLoudW   = Themed(136.0f);
    const float kBpmR    = Themed(704.0f);   // right edge of the tempo field
    const float kSliderR = Themed(588.0f);   // right edge of the master slider
    const float kVolR    = Themed(460.0f);   // right edge of the "Vol" label

    if (fMeter) fMeter->MoveTo(W - Themed(6.0f) - kMeterW, Themed(5.0f));

    const float rightEdge = W - Themed(6.0f) - kMeterW - kGap;   // what the meter leaves

    // Show()/Hide() are counted, not idempotent: each Hide() adds a level and
    // each Show() removes one. Calling them on every resize -- which is exactly
    // when this runs -- would stack levels until the view could no longer be
    // brought back, so only transitions are applied.
    auto setVisible = [](bool& shown, bool on,
                         std::initializer_list<BView*> views) {
        if (shown == on) return;
        for (BView* v : views) {
            if (!v) continue;
            if (on) v->Show(); else v->Hide();
        }
        shown = on;
    };

    // The loudness readout only fits beside the tempo field.
    const bool showLoud = rightEdge - kLoudW >= kBpmR + kGap;
    if (fLoudView && showLoud) fLoudView->MoveTo(W - Themed(278.0f), Themed(8.0f));
    setVisible(fLoudShown, showLoud, { fLoudView });

    // The tempo field only fits when the meter leaves room for the field
    // ITSELF -- it is the rightmost of the left-hand controls, so clearing the
    // slider is not enough (which is exactly how it ended up under the meter).
    const bool showBpm = rightEdge >= kBpmR + kGap;
    setVisible(fBpmShown, showBpm, { fBpmLbl, fTempo });

    // The master slider only fits beside whatever is now the rightmost control
    // of the left cluster: the tempo field when it is shown, the slider's own
    // right edge when it is not.
    const bool showVol = rightEdge >= (showBpm ? kBpmR : kSliderR) + kGap;
    setVisible(fVolShown, showVol, { fVolLbl, fMaster });
}

// Show or hide the inspector pane (View > Inspector, key I). Collapsing the
// split item is what actually reclaims the width; the view is hidden too, so
// its 60 Hz meter updates stop painting into a collapsed column.
void MainWindow::SetInspectorShown(bool shown) {
    fInspectorShown = shown;
    if (fPaneSplit != nullptr)
        fPaneSplit->SetItemCollapsed(0, !shown);
    if (fInspector != nullptr) {
        if (shown) fInspector->Show();
        else       fInspector->Hide();
    }
    if (fInspectorItem != nullptr) fInspectorItem->SetMarked(shown);
    if (fTimeline != nullptr) fTimeline->Invalidate();
}

void MainWindow::FrameResized(float newWidth, float newHeight) {
    BWindow::FrameResized(newWidth, newHeight);
    // The panes are a layout's business now (M1.4); the transport bar's
    // CONTENTS are not a flow -- they are pinned offsets, so they still need
    // this. The views repaint themselves through the frame-change events.
    LayoutTransportBar();
    if (fTimeline)  fTimeline->Invalidate();
    if (fInspector) fInspector->Invalidate();
}

void MainWindow::UpdatePulse() {
    // A native editor watching an insert keeps the pulse alive even when the
    // transport is stopped: the generic parameter panel can move a value with
    // everything idle, and that has to reach the plugin's own editor too.
    bool watching = false;
    for (const FxEntry& w : fFxWatches)
        if (w.editor && w.editor->Alive()) { watching = true; break; }
    const bool need = fTransportCtl.fPlaying || fRecCtl.fRecMode || fTransportCtl.fMonitoring || watching
                   || (fRecCtl.fRecorder && fRecCtl.fRecorder->IsRecording())
                   || !fRender.fExportHandled;   // an export's progress + completion
    if (need && !fPulse) {
        fPulse = new BMessageRunner(BMessenger(this), new BMessage(MSG_PULSE),
                                    kPulseInterval);
    } else if (!need && fPulse) {
        delete fPulse;
        fPulse = nullptr;
    }
}

// Latest frame any clip or note reaches in the project (timeline frames).
Frame ProjectEndFrame(const Project& p) {
    Frame end = 0;
    for (const Track& t : p.Tracks()) {
        for (const Clip& c : t.clips)
            if (c.startFrame + c.lengthFrames > end) end = c.startFrame + c.lengthFrames;
        for (const MidiClip& mc : t.midiClips)
            if (mc.startFrame + mc.lengthFrames > end) end = mc.startFrame + mc.lengthFrames;
    }
    return end;
}





// Start the playback engine from `engineStart` for overdub monitoring during a
// record pass (existing tracks + metronome play while capturing). Returns true
// on success. Runs a long transport so it keeps advancing through silence.


bool MainWindow::AudioMonitorOn() const {
    if (fTransportCtl.fMonitorInput) return true;
    for (TrackId id : fRecCtl.fRecTracks)
        if (const Track* t = fProject->FindTrack(id))
            if (t->inputMonitor) return true;
    return false;
}



// Resolve every MIDI track's endpoint NAME to the producer id it currently has,
// and hand the routes to the engine. Names are the durable assignment (ids get
// reshuffled across sessions), but only ids reach the RT thread, so the lookup
// happens once here rather than per event. A track whose named endpoint is not
// present right now gets no route and stays permissive — it hears whatever is
// connected instead of going silent because a device was unplugged.


// Hand one live event to each armed track that accepts it. This is where record
// demux happens: two keyboards on two armed tracks fill two separate takes.


// Open a MIDI consumer, connect the input endpoint of every MIDI track that is
// armed OR input-monitored (so you hear yourself either way), and begin the
// note-pairing recorder. Recorded clips still drop only on armed tracks.


// End the MIDI take at `endFrame` and drop the resulting region onto each armed
// MIDI track (one fresh clip id per track). Tears down the input consumer.




// Push a committed effect-chain edit into the running engine.
//
// Parameter tweaks sync into the live effects in place, with no seam. A
// STRUCTURAL change (add, remove, reorder, replace) cannot be done in place, so
// SyncFx reports the mismatch and the engine is rebuilt at the playhead.
//
// Every path that mutates a chain has to come through here. The channel strip's
// edits used to reach the model and the drawing but not the audio: they run
// their command directly and then only asked for a repaint, so adding,
// reordering or bypassing an insert from the strip did nothing audible until
// something else happened to rebuild the engine.
// Hand an open effects panel the chain as the MODEL now has it.
//
// The panel holds the copy it was constructed with and commits by sending that
// whole copy back, so any edit made elsewhere -- another view's reorder, or a
// native plugin editor committing a parameter -- would be rewritten from the
// panel's stale snapshot on its next knob move. The panel is the only window
// with that shape; the strips edit through narrow commands.
void MainWindow::PushChainToFxWindow(TrackId tid) {
    if (!fFxMsgr.IsValid() || fFxTrack != tid) return;
    const bool master = (tid == kMasterFxTarget);
    const std::vector<EffectDesc>* chain = master ? &fProject->masterFx : nullptr;
    if (!chain) {
        if (Track* t = fProject->FindTrack(tid)) chain = &t->fx;
    }
    if (!chain) return;
    BMessage m(kMsgFxChain);
    EncodeFxChain(m, *chain);
    fFxMsgr.SendMessage(&m);
}

void MainWindow::SyncFxToEngine() {
    ValidateFxWatch();
    // An open panel's copy of the chain is now out of date; give it the model's.
    PushChainToFxWindow(fFxTrack);
    if (fTransportCtl.fEngine && !fTransportCtl.fEngine->SyncFx(*fProject))
        fTransportCtl.ReloadActiveEngine();
    // Publish AFTER the engine has been brought up to date, not before. With
    // the transport stopped the sync fails and the rebuild is a no-op, so the
    // engine still holds the OLD chain: publishing first would read the
    // previous insert at the new index and show the editor another plugin's
    // values until the next Play.
    PublishFxParamsNow();
}

// A native editor addresses its insert by INDEX (track, fx, slot) -- that is
// what makes it survive an engine rebuild -- so a chain edit that moves or
// removes inserts can leave it pointing at a different effect, and every knob
// it touches would then drive the wrong one. Called wherever the chain changes:
// the editor follows its insert if it moved within the chain, and closes if its
// insert is gone (the same thing the generic panel does when its insert
// disappears).
void MainWindow::ValidateFxWatch() {
    const std::vector<FxWatch> before = FxWatchSnapshot();
    std::vector<FxChainView> chains;
    chains.reserve(before.size());
    for (const FxWatch& w : before) chains.push_back(FxChainFor(w));

    // The rules live in FxWatchTable.h (host-tested): a dead window or a chain
    // that is gone closes the editor; two inserts carrying the same plugin
    // cannot be told apart by URI, so that closes it too rather than let it
    // follow an index that may now be the other one; the same plugin elsewhere
    // in the chain is a rebind; anything else leaves it alone.
    const std::vector<FxWatchUpdate> updates = FxWatchValidate(before, chains);
    for (const FxWatchUpdate& u : updates) {
        FxEntry& w = fFxWatches[u.index];
        switch (u.action) {
        case FxWatchAction::Keep:
            break;
        case FxWatchAction::Rebind:
            w.fx  = u.newFx;
            w.gen = FxWatch::kForcePush;  // the change may be the only one it gets
            ApplyFxWatchToEngine(w);
            w.editor->InsertMoved(w.track, u.newFx);
            break;
        case FxWatchAction::Close:
            if (fTransportCtl.fEngine && w.slot >= 0)
                fTransportCtl.fEngine->SetFxWatch(w.slot, kInvalidTrackId, false, -1);
            w.editor->AskToClose();
            break;
        }
    }
    // Erase in reverse, so the indices the (parallel) update list carries stay
    // valid for the entries that were kept.
    for (size_t i = updates.size(); i > 0; --i)
        if (updates[i - 1].action == FxWatchAction::Close)
            fFxWatches.erase(fFxWatches.begin() + (long)(updates[i - 1].index));
    UpdatePulse();
}

// Ask every open native editor for the values it has written but not yet
// committed, and apply them here, now. Blocking on purpose: this runs on the
// save path, where "the file matches what you heard" matters more than a few
// milliseconds of UI stall (the editors answer from their own looper and the
// timeout is short).
void MainWindow::FlushFxEditors() {
    for (size_t i = fFxWatches.size(); i > 0; --i) {
        const FxEntry& w = fFxWatches[i - 1];
        if (!w.editor || !w.editor->Alive()) continue;
        BMessage flush(kMsgLv2UiFlush);
        BMessage reply;
        // Both timeouts, not just the delivery one: with no reply timeout the
        // default is B_INFINITE_TIMEOUT, and an editor that is slow to answer
        // (its looper may be inside ~Lv2UiWindow joining its own thread) would
        // block this window while it holds its lock. The save would hang rather
        // than lose the last gesture.
        if (static_cast<EditorHandle*>(w.editor.get())->msgr.SendMessage(
                &flush, &reply, 200000, 200000) != B_OK) continue;
        std::vector<SetFxParamCommand::SlotValue> vals;
        for (int32 k = 0; ; k++) {
            int32 slot = 0;
            float v = 0.0f;
            if (reply.FindInt32("slot", k, &slot) != B_OK) break;
            if (reply.FindFloat("val", k, &v) != B_OK) break;
            vals.push_back({ slot, v });
        }
        if (vals.empty()) continue;
        const bool master = (w.track == kMasterFxTarget);
        if (fStack->Execute(std::make_unique<SetFxParamCommand>(
                w.track, master, w.fx, std::move(vals)), *fProject)) {
            // A flush happens before a save/render; the panel's copy is stale
            // from this moment on (same rule as the commit path).
            PushChainToFxWindow(w.track);
        }
    }

    // ...and the GENERIC panel, which this loop does not reach: it folds wheel
    // notches into one undo step behind its own 400 ms timer, and its commit is
    // an async post to this window -- which a caller cannot wait for, because
    // it renders as soon as this returns. So the panel is asked for the chain
    // itself and the answer is applied HERE, synchronously, exactly as
    // kMsgApplyFx would have applied it. Bounded both ways, like the editor
    // flush above: a panel that cannot answer within 200 ms forfeits the edit
    // rather than hanging the save.
    if (fFxMsgr.IsValid()) {
        BMessage flush(kMsgFxPanelFlush);
        BMessage reply;
        if (fFxMsgr.SendMessage(&flush, &reply, 200000, 200000) == B_OK
            && reply.what == kMsgFxPanelFlush) {
            bool pending = false;
            int64 tid = 0;
            reply.FindBool("pending", &pending);
            reply.FindInt64("track", &tid);
            const bool master = ((TrackId)tid == kMasterFxTarget);
            if (pending && (master || fProject->FindTrack((TrackId)tid))) {
                fStack->Execute(std::make_unique<SetFxCommand>(
                    (TrackId)tid, master, DecodeFxChain(reply)), *fProject);
                SyncFxToEngine();
                fTimeline->Invalidate();
            }
        }
    }
}

// Close every native editor and forget every watch: their insert addresses
// (track id, index) belong to the project that is going away, and the loaded
// project can reuse the same ids for entirely different effects.
void MainWindow::CloseFxEditors() {
    for (FxEntry& w : fFxWatches) {
        if (fTransportCtl.fEngine && w.slot >= 0)
            fTransportCtl.fEngine->SetFxWatch(w.slot, kInvalidTrackId, false, -1);
        if (w.editor && w.editor->Alive()) w.editor->AskToClose();
    }
    fFxWatches.clear();
    UpdatePulse();
}



// Reconcile idle live-monitoring with the current arming. When idle (not
// playing / recording) and at least one MIDI track is armed with a MIDI input,
// run a monitor-only engine so the player hears themselves before pressing
// record. Rebuilt on every change (cheap) to pick up new arming / inputs.






// The directory portion of a path (empty if none), for bundling takes.
static std::string DirOfPath(const char* path) {
    std::string p(path ? path : "");
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

bool MainWindow::SaveTo(const char* path) {
    // A knob turned in a native editor reaches the audio immediately but the
    // MODEL only after its debounce goes quiet. Saving inside that window would
    // write the value from before the gesture -- to disk, silently. Ask every
    // open editor for its pending values first, so what is saved is what is
    // playing.
    FlushFxEditors();
    if (!ProjectIO::Save(*fProject, path)) {
        std::fprintf(stderr, "MainWindow: save failed: %s\n", path);
        ReportError("Save Project",
                    std::string("Could not write:\n") + path +
                    "\n\nThe project on disk is unchanged, and your work is "
                    "still here.");
        return false;
    }
    fRecCtl.fTakeDir = DirOfPath(path);   // new takes land beside the project
    fLastDir = fRecCtl.fTakeDir;
    // The document records it: path, clean, recovery copy done, recent list.
    fDoc.NoteSaved(path);
    RebuildRecentMenu();
    UpdateTitle();
    return true;
}

// Decode every soundfont the loaded project references, here on the UI thread.
//
// This has to happen before the engine is built: MakeInstrument only LOOKS UP
// the SoundfontCache, and Engine::Load runs again at every loop-record seam,
// where a decode would stall the audio thread. A track whose file has moved
// falls back to the synth voice, which is audible rather than silent — say so
// once, listing the tracks, instead of failing the whole project open.
void MainWindow::PrimeSoundfonts() {
    std::string missing;
    int nMissing = 0;
    for (const Track& t : fProject->Tracks()) {
        if (t.type != TrackType::Midi || !t.instrument.UsesSoundfont()) continue;
        if (t.instrument.path.empty()) continue;
        std::string err;
        if (SoundfontCache::Instance().Load(t.instrument.path,
                                            t.instrument.sf2Preset, &err))
            continue;
        if (nMissing < 8) {
            missing += "\n  " + t.name + ": " + t.instrument.path;
            if (!err.empty()) missing += "  (" + err + ")";
        }
        nMissing++;
    }
    if (nMissing == 0) return;
    if (nMissing > 8) {
        char more[64];
        std::snprintf(more, sizeof more, "\n  ...and %d more", nMissing - 8);
        missing += more;
    }
    const std::string text =
        "Some instruments could not be loaded. Those tracks will play the "
        "built-in synth voice until the files are available again."
        + missing;
    BAlert* a = new BAlert("Soundfonts", text.c_str(), "OK", nullptr, nullptr,
                           B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    a->SetShortcut(0, B_ESCAPE);
    a->Go(nullptr);   // async: don't block the load
}

void MainWindow::LoadFrom(const char* path, bool asRecovery) {
    // Ask first, before stopping anything or touching the open editors: a
    // Cancel must leave the session exactly as it was. (The Save answer goes
    // through SaveTo, which flushes the editors itself.)
    if (!ConfirmDiscardChanges()) return;
    fTransportCtl.StopPlayback();
    fRecCtl.StopRecording();
    // The editors that are open belong to the project being replaced: their
    // (track id, insert index) addresses are reused by whatever loads, so an
    // editor left open would quietly write its old insert's values into a
    // different plugin's descriptor -- an undoable edit, saved to disk.
    // Flush first, so a gesture still in flight lands in the project it
    // belongs to rather than being dropped with the window that made it.
    FlushFxEditors();
    CloseFxEditors();
    if (!ProjectIO::Load(*fProject, path)) {
        std::fprintf(stderr, "MainWindow: load failed: %s\n", path);
        ReportError("Open Project",
                    std::string("Could not read:\n") + path +
                    "\n\nThe file may be truncated, or not a Haiku DAW "
                    "project. The session you had is untouched.");
        return;
    }
    fStack->Clear();          // history from the previous project is invalid
    if (asRecovery) {
        fDoc.NoteRecovered(); // unsaved, nameless work (see ProjectDocument)
        fRecCtl.fTakeDir.clear();
    } else {
        fDoc.NoteLoaded(path);
        fRecCtl.fTakeDir = DirOfPath(path);
        fLastDir = fRecCtl.fTakeDir;
        RebuildRecentMenu();
    }
    UpdateTitle();
    CollectMissingMedia();    // gone media: one dialog, Skip or Locate…
    PrimeSoundfonts();        // decode MIDI-track soundfonts BEFORE the engine
    RebuildPeaks();           // waveform envelopes for the loaded clips
    fMaster->SetValue((int32)(fProject->masterGain * 100.0f));   // sync slider
    fTimeline->SetProject(fProject);
    fTimeline->SetPlayhead(fProject->transport.playhead);
    UpdateTimeReadout(fProject->transport.playhead);
    fTimeline->Invalidate();
}

void MainWindow::UpdateTitle() {
    const std::string title = fDoc.Title();
    if (title == fTitleShown) return;   // the poll runs often; only set on change
    fTitleShown = title;
    SetTitle(title.c_str());
}

void MainWindow::ReportError(const char* title, const std::string& detail) {
    // Asynchronous (like the About box): a report must never block the looper,
    // and nothing here decides anything -- it is only ever a message. M1 gives
    // it the themed look; the stock alert is the honest placeholder.
    BAlert* a = new BAlert(title, detail.c_str(), "OK", nullptr, nullptr,
                           B_WIDTH_AS_USUAL, B_STOP_ALERT);
    a->SetShortcut(0, B_ESCAPE);
    a->Go(nullptr);
}


// Missing media, seen once after a load. One dialog lists what is gone; Skip
// leaves those clips silent (which is what they have always been), Locate…
// walks the files one at a time and applies the whole repair as one undo step.
void MainWindow::CollectMissingMedia() {
    std::vector<RelinkEntry> missing;
    for (const Track& t : fProject->Tracks())
        for (const Clip& c : t.clips) {
            if (c.sourcePath.empty()) continue;
            BEntry e(c.sourcePath.c_str());
            if (!e.Exists()) missing.push_back(RelinkEntry{ t.id, c.id, "" });
        }
    if (missing.empty()) return;

    std::string list = "These media files are missing:\n\n";
    const size_t kShown = 8;
    for (size_t i = 0; i < missing.size(); i++) {
        if (i == kShown) { list += "  ...and more\n"; break; }
        const Track* t = fProject->FindTrack(missing[i].track);
        const Clip* c = t ? t->FindClip(missing[i].clip) : nullptr;
        if (!c) continue;
        const std::string& p = c->sourcePath;
        const size_t slash = p.find_last_of('/');
        list += "  " + (slash == std::string::npos ? p : p.substr(slash + 1))
              + "\n";
    }
    list += "\nLocate them, or skip and leave those clips silent.";

    BAlert* a = new BAlert("Missing Media", list.c_str(), "Skip",
                           "Locate" B_UTF8_ELLIPSIS);
    a->SetShortcut(0, B_ESCAPE);
    if (a->Go() == 1) StartRelinkWalk(std::move(missing));
}

void MainWindow::StartRelinkWalk(std::vector<RelinkEntry> missing) {
    fRelinkQueue = std::move(missing);
    if (!fRelinkPanel) {
        BMessenger to(this);
        fRelinkPanel = new BFilePanel(B_OPEN_PANEL, &to, NULL, 0, false,
                                      new BMessage(MSG_RELINK_REF));
    }
    RelinkNext();
}

// Ask for the next file that still needs a replacement. A cancelled panel
// posts nothing and simply ends the walk; whatever was chosen is kept.
void MainWindow::RelinkNext() {
    for (const RelinkEntry& e : fRelinkQueue) {
        if (!e.newPath.empty()) continue;      // already replaced
        const Track* t = fProject->FindTrack(e.track);
        const Clip* c = t ? t->FindClip(e.clip) : nullptr;
        if (!c) continue;                      // the clip is gone: nothing to ask
        // Start the panel where the file used to live -- that is where a moved
        // file most likely went.
        const size_t slash = c->sourcePath.find_last_of('/');
        if (slash != std::string::npos) {
            BPath dir(c->sourcePath.substr(0, slash).c_str());
            fRelinkPanel->SetPanelDirectory(dir.Path());
        }
        fRelinkPanel->Show();
        return;
    }
    FinishRelink();
}

void MainWindow::FinishRelink() {
    std::vector<RelinkEntry> picked;
    for (const RelinkEntry& e : fRelinkQueue)
        if (!e.newPath.empty()) picked.push_back(e);
    fRelinkQueue.clear();
    if (picked.empty()) return;
    fStack->Execute(std::make_unique<RelinkMediaCommand>(std::move(picked)),
                    *fProject);
    RebuildPeaks();        // the found files' waveforms
    fTimeline->Invalidate();
    UpdateTitle();         // a repair is an edit: the project is dirty now
}

// File > New: a fresh, empty session. The sample rate stays -- it belongs to
// the session (the device), not to the project's content -- and everything
// else goes. Same "the engine rebuilds at the next play" stance as LoadFrom.
void MainWindow::NewProject() {
    if (!ConfirmDiscardChanges()) return;
    fTransportCtl.StopPlayback();
    fRecCtl.StopRecording();
    FlushFxEditors();
    CloseFxEditors();
    const double rate = fProject->sampleRate;
    *fProject = Project{};
    fProject->sampleRate = rate;
    fProject->tempoMap.sampleRate = rate;
    fStack->Clear();
    fDoc.NoteNew();               // no path, and nothing to lose
    fRecCtl.fTakeDir.clear();
    fRecCtl.fRecTracks.clear();           // arms addressed the old project's tracks
    UpdateTitle();
    RebuildPeaks();
    fMaster->SetValue((int32)(fProject->masterGain * 100.0f));
    fTimeline->SetProject(fProject);   // same object, new content
    fTimeline->SetPlayhead(0);
    UpdateTimeReadout(0);
    fTimeline->Invalidate();
}

// Record a project in the recent list, refresh the menu, persist it (the list
// is not re-derivable, so it survives even an unclean end of the session).
void MainWindow::RememberProject(const std::string& path) {
    if (path.empty()) return;
    fDoc.Remember(path);
    RebuildRecentMenu();
    SaveSettings();
}

void MainWindow::ForgetRecent(const std::string& path) {
    fDoc.Forget(path);
    RebuildRecentMenu();
    SaveSettings();
}

// Rebuilt, not built once: the list changes on every save and open. Labels are
// the file's base name (the full path would not fit a menu); the message
// carries the path.
void MainWindow::RebuildRecentMenu() {
    if (!fRecentMenu) return;
    while (fRecentMenu->CountItems() > 0)
        delete fRecentMenu->RemoveItem((int32)0);
    if (fDoc.Recent().empty()) {
        BMenuItem* none = new BMenuItem("(none)", nullptr);
        none->SetEnabled(false);   // an empty submenu is a dead end
        fRecentMenu->AddItem(none);
        return;
    }
    for (const std::string& path : fDoc.Recent()) {
        BMenuItem* item = new BMenuItem(
            ProjectDocument::DisplayName(path).c_str(),
                                        new BMessage(MSG_OPEN_RECENT));
        item->Message()->AddString("path", path.c_str());
        fRecentMenu->AddItem(item);
    }
}

// Ask about unsaved changes before an action that would drop them (Quit, Open,
// New). True = the caller may proceed: nothing was dirty, the user saved, or
// the user explicitly discarded. A project with no path yet gets the save
// panel and a refusal -- the caller is retried after the panel lands (M0.2
// gives Save a silent path; this is the smallest correct answer until then).
bool MainWindow::ConfirmDiscardChanges() {
    if (!fStack->IsDirty()) return true;
    BAlert* a = new BAlert("Unsaved Changes",
        "This project has unsaved changes.", "Cancel", "Discard", "Save",
        B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    a->SetShortcut(0, B_ESCAPE);
    const int32 choice = a->Go();   // synchronous, like the recovery prompt
    if (choice == 1) {              // Discard: the user authorised losing it
        fDoc.RemoveRecoveryFile();
        return true;
    }
    if (choice == 2) {              // Save (SaveTo clears the recovery itself)
        if (!fDoc.HasPath()) {
            PostMessage(MSG_SAVE);
            return false;
        }
        return SaveTo(fDoc.Path().c_str());    // a failed save does not proceed
    }
    return false;                   // Cancel (Escape)
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

void MainWindow::ImportMidi(const char* path, Frame at) {
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
        if (st.notes.empty() && st.events.empty())
            continue;                               // skip conductor/empty tracks
        MidiClip clip;
        clip.startFrame = at < 0 ? 0 : at;   // notes stay clip-relative
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
        for (const SmfEvent& se : st.events) {      // CC/PB/PC/pressure
            MidiClipEvent me;
            me.type       = se.type;
            me.startFrame = (Frame)llround(se.tick * perTick);
            me.data       = se.data;
            me.value      = se.value;
            clip.events.push_back(me);
            if (me.startFrame + 1 > maxEnd)         // keep events in the window
                maxEnd = me.startFrame + 1;
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
        for (const MidiClipEvent& e : t.CollectEvents()) {   // CC/PB/PC/pressure
            SmfEvent se;
            se.type  = e.type;
            se.tick  = (uint32_t)llround((e.startFrame / fpb) * division);
            se.data  = e.data;
            se.value = e.value;
            st.events.push_back(se);
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










// Render one track in isolation (its clips/notes through its fader + effect
// chain) to a stereo WAV — the basis for Freeze. Routing/sends are stripped so
// only the track's own output is baked; solo/mute are cleared so it's audible.
bool RenderTrackToWav(const Project& src, TrackId id,
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

// --- off-looper export ------------------------------------------------------

// The options dialog. It never touches the model: its choices come back as
// kMsgExportOptions, and THAT handler opens the file panel -- so the format is
// settled before a destination is asked for, and a cancelled dialog asks for
// nothing.
void MainWindow::OpenExportWindow(bool stems) {
    BRect r(200, 200, 200 + 380, 200 + 400);
    ExportChoices c = fExportChoices;
    c.stems = stems;
    (new ExportWindow(r, c, stems, BMessenger(this)))->Show();
}

// Start a bounce. Runs on the looper, and in this order for a reason: stop the
// transport (nothing else may be rendering), flush the editors (so the model
// holds the value the user just heard -- the reason the old synchronous path
// flushed), and only then snapshot. The worker renders the COPY, so editing
// may continue while it runs and the model is never read across threads.


// The pulse saw the worker finish (fRender.fExportRunning went false). Close the bar,
// reap the thread, drop the snapshot and say what happened. Runs on the looper.




// Resolve ~/config/settings/HaikuDAW/settings, creating the dir if needed.
// The crash-recovery autosave file (settings dir). A leftover after startup
// means the last session didn't exit cleanly.


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
    if (!ProjectDocument::SettingsPath(p)) return;
    BFile f(p.Path(), B_READ_ONLY);
    if (f.InitCheck() != B_OK) return;
    off_t sz = 0;
    if (f.GetSize(&sz) != B_OK || sz <= 0 || sz > 65536) return;
    std::string text;
    text.resize((size_t)sz);
    if (f.Read(&text[0], (size_t)sz) != (ssize_t)sz) return;

    AppSettings s;
    s.bufferFrames = (int)fTransportCtl.fBufferFrames;
    s.countInBars  = fRecCtl.fCountInBars;
    s.metronome    = fTransportCtl.fMetronome;
    s.monitorInput = fTransportCtl.fMonitorInput;
    s.exportBitDepth  = fExportChoices.bitDepth;
    s.exportDither    = fExportChoices.dither;
    s.exportSampleRate = fExportChoices.sampleRate;
    s.exportNormalize = fExportChoices.normalize;
    s.exportTargetLufs = fExportChoices.targetLufs;
    s.exportTruePeakCeil = fExportChoices.truePeak;
    s.exportLimiter   = fExportChoices.limiter;
    s.exportRange     = fExportChoices.range;
    s.exportStems     = fExportChoices.stems ? 1 : 0;
    if (!s.Deserialize(text)) return;

    // The panes (M1.4). The width goes on as the inspector's preferred size
    // and the split weights keep it fixed while the timeline takes the rest.
    if (fPaneSplit != nullptr) {
        const float w = s.inspectorWidth > 80.0f ? s.inspectorWidth
                                                 : InspectorWidth();
        fInspector->SetExplicitPreferredSize(BSize(w, B_SIZE_UNSET));
        fPaneSplit->SetItemWeight(0, 0.0f, true);
        fPaneSplit->SetItemWeight(1, 1.0f, true);
        SetInspectorShown(s.inspectorVisible);
    }
    if (fRootSplit != nullptr && fDock != nullptr) {
        const float h = s.bottomHeight > 80.0f ? s.bottomHeight : Themed(260.0f);
        fDock->SetExplicitPreferredSize(BSize(B_SIZE_UNSET, h));
        fRootSplit->SetItemCollapsed(1, !s.bottomVisible);
        if (fDockItem != nullptr) fDockItem->SetMarked(s.bottomVisible);
    }

    fTransportCtl.fBufferFrames = (size_t)s.bufferFrames;
    fRecCtl.fCountInBars  = s.countInBars;
    fTransportCtl.fMetronome    = s.metronome;
    fTransportCtl.fMonitorInput = s.monitorInput;
    if (fTimeline) fTimeline->SetMonitorInput(fTransportCtl.fMonitorInput);
    fLastDir      = s.lastDir;
    fDoc.SetRecent(s.recentProjects);
    RebuildRecentMenu();
    // The export dialog reopens on the last choices, not on its defaults.
    fExportChoices.bitDepth   = s.exportBitDepth;
    fExportChoices.dither     = s.exportDither;
    fExportChoices.sampleRate = s.exportSampleRate;
    fExportChoices.normalize  = s.exportNormalize;
    fExportChoices.targetLufs = s.exportTargetLufs;
    fExportChoices.truePeak   = s.exportTruePeakCeil;
    fExportChoices.limiter    = s.exportLimiter;
    fExportChoices.range      = s.exportRange;
    fExportChoices.stems      = s.exportStems != 0;
    MarkRadio(fBufMenu, "frames", (int32)fTransportCtl.fBufferFrames);
    MarkRadio(fCountInMenu, "bars", fRecCtl.fCountInBars);
    if (fMetItem)   fMetItem->SetMarked(fTransportCtl.fMetronome);
    if (fMonInItem) fMonInItem->SetMarked(fTransportCtl.fMonitorInput);
    // Restore the window frame (clamped to something sane).
    if (s.winR - s.winL > 320 && s.winB - s.winT > 240) {
        MoveTo(s.winL, s.winT);
        ResizeTo(s.winR - s.winL, s.winB - s.winT);
    }
}

void MainWindow::SaveSettings() {
    AppSettings s;
    s.bufferFrames = (int)fTransportCtl.fBufferFrames;
    s.countInBars  = fRecCtl.fCountInBars;
    s.metronome    = fTransportCtl.fMetronome;
    s.monitorInput = fTransportCtl.fMonitorInput;
    s.lastDir      = fLastDir;
    s.recentProjects = fDoc.Recent();
    s.exportBitDepth  = fExportChoices.bitDepth;
    s.exportDither    = fExportChoices.dither;
    s.exportSampleRate = fExportChoices.sampleRate;
    s.exportNormalize = fExportChoices.normalize;
    s.exportTargetLufs = fExportChoices.targetLufs;
    s.exportTruePeakCeil = fExportChoices.truePeak;
    s.exportLimiter   = fExportChoices.limiter;
    s.exportRange     = fExportChoices.range;
    s.exportStems     = fExportChoices.stems ? 1 : 0;
    const BRect fr = BWindow::Frame();
    s.winL = fr.left; s.winT = fr.top; s.winR = fr.right; s.winB = fr.bottom;
    // The panes as the user left them (M1.4).
    s.inspectorVisible = fInspectorShown;
    if (fInspector != nullptr && fInspector->Bounds().Width() > 1.0f)
        s.inspectorWidth = fInspector->Bounds().Width();
    if (fRootSplit != nullptr) {
        s.bottomVisible = !fRootSplit->IsItemCollapsed(1);
        if (fDock != nullptr && fDock->Bounds().Height() > 1.0f)
            s.bottomHeight = fDock->Bounds().Height();
    }

    BPath p;
    if (!ProjectDocument::SettingsPath(p)) return;
    BFile f(p.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    if (f.InitCheck() != B_OK) return;
    const std::string t = s.Serialize();
    f.Write(t.data(), t.size());
}

void MainWindow::UpdateTimeReadout(Frame playhead) {
    const double rate = fTransportCtl.fEngine ? fTransportCtl.fEngine->OutputRate() : fProject->sampleRate;
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

void MainWindow::PushRollPlayhead(Frame ph) {
    if (!fRollMsgr.IsValid()) return;
    BMessage m(kMsgRollPlayhead);
    m.AddInt64("ph", (int64)ph);
    fRollMsgr.SendMessage(&m);
}

void MainWindow::PushTrackPeaks() {
    if (!fTransportCtl.fEngine) return;
    std::map<TrackId, std::pair<float, float>> tp;
    for (const Track& t : fProject->Tracks())
        tp[t.id] = { fTransportCtl.fEngine->TrackPeakL(t.id), fTransportCtl.fEngine->TrackPeakR(t.id) };
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
        m.AddFloat("mpl", fTransportCtl.fEngine->PeakL());
        m.AddFloat("mpr", fTransportCtl.fEngine->PeakR());
        fMixerMsgr.SendMessage(&m);
    }
}

// --- native editor watches -------------------------------------------------

void MainWindow::ApplyFxWatchToEngine(const FxEntry& w) {
    if (!fTransportCtl.fEngine || w.slot < 0) return;
    fTransportCtl.fEngine->SetFxWatch(w.slot, w.track, w.track == kMasterFxTarget, w.fx);
}

// After the engine object itself is replaced (play, record, loop wrap, and
// every monitor rebuild): the watches lived in the old one. Without this an
// open editor silently stops hearing about automation the first time the user
// presses play -- which is exactly when it matters.
void MainWindow::ReapplyFxWatches() {
    for (size_t i = fFxWatches.size(); i > 0; --i) {
        FxEntry& w = fFxWatches[i - 1];
        if (!w.editor || !w.editor->Alive()) {
            if (fTransportCtl.fEngine && w.slot >= 0)
                fTransportCtl.fEngine->SetFxWatch(w.slot, kInvalidTrackId, false, -1);
            fFxWatches.erase(fFxWatches.begin() + (long)(i - 1));
            continue;
        }
        w.gen = FxWatch::kForcePush;   // a fresh engine's counter must not collide
        ApplyFxWatchToEngine(w);
    }
}

// Hand each editor its insert's live values. The engine only bumps a slot's
// generation when a value actually changed, so a still insert costs one atomic
// load per pulse.
void MainWindow::PushFxParams() {
    for (size_t i = fFxWatches.size(); i > 0; --i) {
        FxEntry& w = fFxWatches[i - 1];
        if (!w.editor || !w.editor->Alive()) {
            if (fTransportCtl.fEngine && w.slot >= 0)
                fTransportCtl.fEngine->SetFxWatch(w.slot, kInvalidTrackId, false, -1);
            fFxWatches.erase(fFxWatches.begin() + (long)(i - 1));
            continue;
        }
        if (!fTransportCtl.fEngine) {
            // Nothing has played yet, so there is no engine to publish from --
            // but the MODEL still changes (the generic parameter panel commits
            // on mouse-up), and an editor left out of step with it would show
            // one value while the project says another. Publish the model.
            PushFxParamsFromModel(w);
            continue;
        }
        if (w.slot < 0) {
            // Registered while every slot was taken. Now that one may have
            // freed, take it: the alternative is an editor that never follows
            // automation again, with the claim blocking a close-and-reopen.
            const int freed = FxWatchFreeSlot(FxWatchSnapshot(),
                                              Engine::kWatchSlots);
            if (freed < 0) continue;
            w.slot = freed;
            w.gen  = FxWatch::kForcePush;   // it has never had a frame
            ApplyFxWatchToEngine(w);
        }
        float vals[Engine::kWatchMax];
        uint32_t gen = 0;
        const int n = fTransportCtl.fEngine->WatchedFxParams(w.slot, vals, Engine::kWatchMax,
                                               &gen);
        if (!FxWatchFrameIsNew(n, gen, w.gen)) continue;
        w.gen = gen;
        w.editor->SendFrame(vals, n);
    }
}

// Push the model's values for one watched insert, for when there is no engine
// at all (nothing has played yet). Only what actually changed is sent, so a
// still editor costs one vector compare per pulse.
void MainWindow::PushFxParamsFromModel(FxEntry& w) {
    const std::vector<EffectDesc>* chain = nullptr;
    if (w.track == kMasterFxTarget) {
        chain = &fProject->masterFx;
    } else if (const Track* t = fProject->FindTrack(w.track)) {
        chain = &t->fx;
    }
    if (!chain || w.fx < 0 || w.fx >= (int)chain->size()) return;
    const std::vector<float>& params = (*chain)[(size_t)w.fx].params;
    if (params == w.pushed) return;          // nothing has moved
    w.pushed = params;
    w.editor->SendFrame(params.data(), (int)params.size());
}

// Publish and push once, right now, for the stopped transport: no audio block
// is coming, so nothing else would tell the editors what a panel drag did.
void MainWindow::PublishFxParamsNow() {
    if (!fTransportCtl.fEngine) return;
    fTransportCtl.fEngine->PublishFxWatchNow();
    PushFxParams();
}

// Open the plugin's own editor for the insert at `fx` of `tid` (master sentinel
// allowed). Everything is resolved against the MODEL, because the caller may be
// an editor view holding a stale snapshot of the chain -- and resolves by URI,
// so what opens is the plugin actually at that index now, not the one the
// caller thought was there.
bool MainWindow::OpenNativeEditor(TrackId tid, int fx) {
#ifdef DAW_HAVE_LV2
    const bool master = (tid == kMasterFxTarget);
    Track* t = master ? nullptr : fProject->FindTrack(tid);
    std::vector<EffectDesc>* chain = master ? &fProject->masterFx
                                            : (t ? &t->fx : nullptr);
    if (!chain || fx < 0 || fx >= (int)chain->size()) return false;
    const EffectDesc& d = (*chain)[(size_t)fx];
    if (d.type != EffectType::Lv2 || !Lv2UiWindow::HasNativeUi(d.pluginName))
        return false;

    // Two copies of one plugin in a chain cannot be told apart by anything the
    // model carries: an editor opened on one of them would follow the index to
    // the other the moment the chain changed, and would be driving a plugin it
    // is not showing. Refuse rather than open it wrong -- the generic parameter
    // list is still there, and it edits by index against the model, which is
    // exactly right for whichever insert the user picked.
    int copies = 0;
    for (const EffectDesc& e : *chain)
        if (e.pluginName == d.pluginName) copies++;
    if (copies > 1) {
        std::fprintf(stderr, "daw: '%s' is in this chain %d times; its own "
                     "editor cannot tell the copies apart, so it is not opened "
                     "(use the parameter list)\n", d.pluginName.c_str(), copies);
        return false;
    }

    // The title names the track: two tracks can hold the same plugin, and with
    // both editors open the plugin's name alone would not say which is which.
    std::string title = EffectDisplayName(d);
    title += master ? "  -  Master" : ("  -  " + t->name);

    BRect uw(160, 160, 160 + 960, 160 + 680);
    Lv2UiWindow::Open(uw, d.pluginName, title, d.params, tid, fx,
                      BMessenger(this));
    return true;
#else
    (void)tid; (void)fx;
    return false;
#endif
}

void MainWindow::PushFxMeters() {
    if (!fTransportCtl.fEngine || !fFxMsgr.IsValid()) return;
    BMessage m(kMsgFxMeter);
    float gr[16];
    for (int i = 0; i < 16; i++) gr[i] = fTransportCtl.fEngine->MeterGrDb(i);
    m.AddData("gr", B_FLOAT_TYPE, gr, sizeof(gr));
    float spec[256]; int specFx = -1;
    const int n = fTransportCtl.fEngine->MeterSpectrum(spec, 256, &specFx);
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
    // Both Cmd-Q and the window's close button arrive here. Ask before stopping
    // anything, so a Cancel leaves the session exactly as it was.
    if (!ConfirmDiscardChanges())
        return false;               // stay: the user has not decided yet
    fTransportCtl.StopPlayback();
    fRecCtl.StopRecording();
    SaveSettings();
    // Clean exit (nothing was unsaved, or an explicit Save/Discard above): the
    // recovery file has nothing left to rescue, so next launch does not offer
    // it. A Cancel never reaches this line.
    fDoc.RemoveRecoveryFile();
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}

} // namespace daw
