#include "MainWindow.h"

#include "widgets/DawControlLook.h"   // InstallControlLookForMode
#include "widgets/DawSegments.h"
#include "widgets/DawTextField.h"

#include "TimelineView.h"
#include "InspectorView.h"
#include "TransportBar.h"
#include "MeterView.h"
#include "ShortcutsWindow.h"   // View > Keyboard Shortcuts (M3.2)
#include "EffectsWindow.h"
#include "SendsWindow.h"
#include "InstrumentWindow.h"
#include "../synth/SampleBank.h"
#include "PianoRoll.h"
#include "SampleBrowser.h"
#include "MixerWindow.h"
#include "MarkersWindow.h"   // the markers/locations list (M2.7)
#include "PluginBrowser.h"
#include "ExportWindow.h"
#include "ExportProgressWindow.h"
#include "Version.h"   // DAW_VERSION_STRING (generated from CMake)
#ifdef DAW_HAVE_LV2
#include "Lv2UiWindow.h"
#include "../plugin/Lv2Host.h"     // preset listing/saving for an insert panel
#endif
#include "../storage/BfsAttr.h"
#include "../app/AppSettings.h"
#include "RenameWindow.h"
#include "UiMetrics.h"
#include "widgets/DawButton.h"      // the kit (M1.3)
#include "widgets/DawSlider.h"
#include "widgets/DawTextField.h"

#include "../engine/DeviceLatency.h"
#include "../engine/WavSource.h"
#include "../engine/AudioFormats.h"   // the import factory + format availability
#include "../engine/WavWriter.h"
#include "../engine/Exporter.h"
#include "../engine/Resampler.h"
#include "../model/ProjectIO.h"
#include "../model/Commands.h"
#include "../model/KeyMap.h"            // TimelineTakesKey (the key-routing gate)
#include "../model/RegionOps.h"
#include "../model/ProjectTimebase.h"   // ScaleProjectTimebase (a template's rate)
#include "../model/SmfIO.h"
#include "../model/MidiOps.h"
#include "../model/RecordPlan.h"
#include "../model/TakeNames.h"

#include <GroupView.h>
#include <LayoutBuilder.h>
#include <SplitView.h>
#include <SpaceLayoutItem.h>
#include <StringView.h>
#include <Alert.h>
#include <Application.h>
#include <OS.h>   // system_time() for MIDI event timestamping
#include <Button.h>
#include <File.h>
#include <FindDirectory.h>
#include <kernel/image.h>   // B_APP_IMAGE_SYMBOL (the template lookup, M3.7)
#include <Entry.h>
#include <Directory.h>   // create_directory (stems folder)
#include <FilePanel.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <Screen.h>
#include <MessageRunner.h>
#include <Path.h>
#include <Slider.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>   // a text field's inner view can hold the focus too

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace daw {

// The tracks an insert's external-sidechain picker offers: every track except
// the one the editor belongs to. A self-key is deliberately not offered —
// "key this insert from the track it is on" has no order the hosts could read
// (the source has to render before the consumer), and the model/hosts treat it
// as unrouted, so offering it would be a click that silently does nothing.
// kMasterFxTarget names no track, so the master chain's picker offers them all.
static std::vector<std::pair<TrackId, std::string>>
SidechainCandidates(const Project& p, TrackId except) {
    std::vector<std::pair<TrackId, std::string>> v;
    for (const Track& t : p.Tracks())
        if (t.id != except) v.push_back({t.id, t.name});
    return v;
}

enum {
    MSG_PLAY  = 'play',
    MSG_STOP  = 'stop',
    MSG_REC   = 'rec ',
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
    MSG_POP_OUT_EDITOR   = 'poed',   // dock the MIDI editor out into a window
};

// How long a job/error message stands on the status line before the transport
// state takes the row back (see SetTransportStatus). Long enough to read and
// look away, short enough that a stale "Exported mix.wav" is not still there
// minutes later.
static const bigtime_t kStatusHoldUs = 4 * 1000 * 1000;

// Sentinel "track id" the effects editor uses to target the master FX chain.
static const TrackId kMasterFxTarget = ~(TrackId)0;

// A pane's title strip (the dock's, M1.4): a fixed-height bar in the panel
// colour with a hairline under it, laid out by a horizontal group. BGroupView
// would do the layout but never draws, and the line is what separates the
// strip from the editor below it.
class PaneHeader : public BView, public ThemeAware {
public:
    // The strip's colour is cached, so it is re-taken on a mode switch (T1).
    void ApplyTheme() override { SetViewColor(ColHeader()); }

    explicit PaneHeader(const char* name)
        : BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE) {
        ApplyTheme();
        BGroupLayout* g = new BGroupLayout(B_HORIZONTAL, Themed(4.0f));
        SetLayout(g);
        g->SetInsets(Themed(10.0f), Themed(3.0f), Themed(4.0f), Themed(4.0f));
        SetExplicitMinSize(BSize(B_SIZE_UNSET, Themed(26.0f)));
        SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, Themed(26.0f)));
    }
    void Draw(BRect) override {
        const BRect b = Bounds();
        SetHighColor(ColGrid());
        StrokeLine(BPoint(b.left, b.bottom), BPoint(b.right, b.bottom));
    }
};

// A header button sized to its label and the strip, never stretched by the
// group it sits in.
static DawButton* HeaderButton(const char* name, const char* label,
                               uint32 what) {
    DawButton* b = new DawButton(name, label, new BMessage(what));
    float w = 0.0f, h = 0.0f;
    b->GetPreferredSize(&w, &h);
    const BSize sz(std::max(w, Themed(22.0f)), Themed(18.0f));
    b->SetExplicitMinSize(sz);
    b->SetExplicitMaxSize(sz);
    return b;
}

// Show a file panel titled for what it is doing ("Export Mix", not the stock
// "<app>: Save"), with a save panel's name field prefilled when there is an
// obvious name to offer. The panels are kept and reused, so both are set on
// every showing.
static void ShowPanel(BFilePanel* panel, const char* title,
                      const std::string& saveText = std::string()) {
    if (BWindow* w = panel->Window()) {
        if (w->Lock()) {
            w->SetTitle(title);
            w->Unlock();
        }
    }
    if (!saveText.empty()) panel->SetSaveText(saveText.c_str());
    panel->Show();
    // A file panel is a stock Haiku window: its background and its list read
    // the system colours, which the app no longer rewrites (T1), so the DAW's
    // colours go on explicitly (a no-op in System mode). Twice: the panel is
    // still laying itself out when Show() returns, so the second pass runs a
    // moment later on the main window's looper.
    ThemeStockWindow(panel->Window());
    BMessageRunner::StartSending(panel->Messenger(),
                                 new BMessage(MSG_THEME_STOCK), 250000, 1);
}

// Snapshot the mixer strip state from the model (used to open the mixer and to
// refresh it live when the model changes elsewhere).
//
// `tv` is the timeline, asked only for the automation-lane MODE (a view state,
// not a model one): the strip's Auto button shows and cycles it, and a lamp lit
// from anything other than the real mode would be a lie. Null is allowed -- the
// tests build the payload without a timeline -- and then no mode travels.
static std::vector<MixerStripInfo> BuildMixerStrips(const Project& p,
                                                    const TimelineView* tv = nullptr) {
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
        s.autoMode = tv ? tv->AutoModeFor(t.id) : 0;
        // The strip draws a label and a bypass dot per insert. Naming an insert
        // is done HERE, on the main thread, because EffectDisplayName resolves an
        // LV2 URI through the host -- which the mixer's own looper must not do.
        for (const EffectDesc& d : t.fx)
            s.fx.push_back(MixerInsertInfo{ EffectDisplayName(d), d.bypassed });
        // The strip extras (M3.4): the trim, the polarity, the solo group and
        // one entry per aux send — the destination's NAME, resolved here on
        // the main thread, never an id the mixer would have to look up.
        s.trim        = t.trim;
        s.phaseInvert = t.phaseInvert;
        s.soloGroup   = t.soloGroup;
        for (const Send& send : t.sends) {
            MixerSendInfo si;
            si.level = send.level;
            if (const Track* dest = p.FindTrack(send.dest))
                si.label = dest->name.substr(0, 4);
            else
                si.label = "?";
            s.sends.push_back(std::move(si));
        }
        s.hasInput = t.input.kind != InputSource::kNone;
        if (t.output == kInvalidTrackId) s.outLabel = "Mst";
        else if (const Track* bt = p.FindTrack(t.output))
            s.outLabel = bt->name.substr(0, 6);
        else s.outLabel = "Bus";
        strips.push_back(std::move(s));
    }
    return strips;
}

// The ONE encoder for kMsgMixStrips. The mixer's decoder reads its own field
// names ("g", "mu", "sn", ...) and this message is sent from two places: the
// refresh funnel, and View > Mixer asking an already-open mixer to update.
// Those two encodings drifted -- the View path still wrote "gain"/"mute"/"arm"
// and never wrote a name, a send count or an output label -- so opening the
// mixer a second time replaced its strips with default ones. Both go through
// here now, and anything added to MixerStripInfo is added once.
static void AddMixerStripsPayload(BMessage& m, const Project& p,
                                  const TimelineView* tv = nullptr) {
    for (const MixerStripInfo& s : BuildMixerStrips(p, tv)) {
        m.AddInt64("tid", (int64)s.trackId);
        m.AddString("nm", s.name.c_str());
        m.AddFloat("g", s.gain);  m.AddFloat("p", s.pan);
        m.AddBool("mu", s.muted); m.AddBool("so", s.soloed);
        m.AddBool("ar", s.armed); m.AddBool("mo", s.inputMonitor);
        m.AddInt32("ci", s.colorIndex); m.AddInt32("ty", s.type);
        // Count first, then this strip's inserts appended to the one flat run
        // the mixer slices back apart in strip order.
        m.AddInt32("fx", (int32)s.fx.size());
        for (const MixerInsertInfo& ins : s.fx) {
            m.AddString("fxn", ins.name.c_str());
            m.AddBool("fxb", ins.bypassed);
        }
        m.AddInt32("sn", (int32)s.sends.size());
        // The send labels/levels travel the same way the inserts do: one flat
        // run across all strips, sliced by "sn".
        for (const MixerSendInfo& si : s.sends) {
            m.AddString("sdn", si.label.c_str());
            m.AddFloat("slv", si.level);
        }
        m.AddFloat("tr", s.trim);
        m.AddBool("ph", s.phaseInvert);
        m.AddInt32("sg", s.soloGroup);
        m.AddBool("hi", s.hasInput);    m.AddString("ol", s.outLabel.c_str());
        m.AddInt32("am", s.autoMode);   // the Auto button's lamp
    }
    m.AddFloat("mg", p.masterGain);
}

// The posted note list (PianoRoll -> here) is decoded by PianoRoll.h's
// ParseRollNotes: one definition of the wire shape, next to the message ids it
// belongs to (the roll decodes MainWindow's region push with the same code).

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
    // "New" opens the New Project dialog, so it carries the same ellipsis as
    // Save As…/Preferences… (a menu item that opens a window announces it).
    fileMenu->AddItem(new ThemedMenuItem("New" B_UTF8_ELLIPSIS,
                                         new BMessage(MSG_NEW_PROJECT), 'N'));
    fileMenu->AddItem(new ThemedMenuItem("Open" B_UTF8_ELLIPSIS, new BMessage(MSG_OPEN), 'O'));
    fRecentMenu = new BMenu("Open Recent");
    fileMenu->AddItem(new ThemedMenuItem(fRecentMenu));
    // Save has no ellipsis: once the project has a path it writes straight to
    // it (Save As is the way to move it). Both ask when there is no path yet.
    fileMenu->AddItem(new ThemedMenuItem("Save", new BMessage(MSG_SAVE), 'S'));
    fileMenu->AddItem(new ThemedMenuItem("Save As" B_UTF8_ELLIPSIS,
                                    new BMessage(MSG_SAVE_AS), 'S', B_SHIFT_KEY));
    fileMenu->AddItem(new ThemedMenuItem("Import Audio" B_UTF8_ELLIPSIS, new BMessage(MSG_IMPORT)));
    fileMenu->AddItem(new ThemedMenuItem("Import MIDI" B_UTF8_ELLIPSIS, new BMessage(MSG_IMPORT_MIDI)));
    fileMenu->AddItem(new ThemedMenuItem("Export WAV" B_UTF8_ELLIPSIS, new BMessage(MSG_EXPORT)));
    fileMenu->AddItem(new ThemedMenuItem("Export Stems" B_UTF8_ELLIPSIS, new BMessage(MSG_EXPORT_STEMS)));
    fileMenu->AddItem(new ThemedMenuItem("Export MIDI" B_UTF8_ELLIPSIS, new BMessage(MSG_EXPORT_MIDI)));
    fileMenu->AddSeparatorItem();
    // Project Settings… (M3.6): the project's own rate, tempo, meter and notes.
    fileMenu->AddItem(new ThemedMenuItem("Project Settings" B_UTF8_ELLIPSIS,
                                         new BMessage(MSG_PROJECT_SETTINGS)));
    // Preferences… (M3.5): the window that now carries what used to be the
    // Audio menu's radio submenus and View's Dark Mode toggle.
    fileMenu->AddItem(new ThemedMenuItem("Preferences" B_UTF8_ELLIPSIS,
                                         new BMessage(MSG_PREFERENCES)));
    fileMenu->AddSeparatorItem();
    // Close is the window's own quit request, so the unsaved-changes prompt
    // applies exactly as it does to the title-bar button and Cmd-Q.
    fileMenu->AddItem(new ThemedMenuItem("Close", new BMessage(MSG_CLOSE), 'W'));
    fileMenu->AddItem(new ThemedMenuItem("Quit", new BMessage(B_QUIT_REQUESTED), 'Q'));
    menuBar->AddItem(new ThemedMenuItem(fileMenu));
    // The full Edit menu (M2.4). Undo/Redo carry the stack's own names
    // ("Undo Move Clip") and are disabled on an empty stack; the range
    // operations act on the time range, falling back to the clip selection.
    BMenu* editMenu = new BMenu("Edit");
    fUndoItem = new ThemedMenuItem("Undo", new BMessage(MSG_EDIT_UNDO), 'Z');
    fRedoItem = new ThemedMenuItem("Redo", new BMessage(MSG_EDIT_REDO), 'Z',
                                   B_SHIFT_KEY);
    fUndoItem->SetEnabled(false);
    fRedoItem->SetEnabled(false);
    editMenu->AddItem(fUndoItem);
    editMenu->AddItem(fRedoItem);
    editMenu->AddSeparatorItem();
    fCutItem = new ThemedMenuItem("Cut", new BMessage(MSG_EDIT_CUT), 'X');
    fCopyItem = new ThemedMenuItem("Copy", new BMessage(MSG_EDIT_COPY), 'C');
    fPasteItem = new ThemedMenuItem("Paste", new BMessage(MSG_EDIT_PASTE), 'V');
    // No shortcut on the label: Del is handled by the timeline itself (a
    // non-printable menu shortcut is not worth the portability risk), and the
    // menu item is there to be seen and clicked.
    fDeleteItem = new ThemedMenuItem("Delete", new BMessage(MSG_EDIT_DELETE));
    fDeleteItem->SetEnabled(false);
    fSelectAllItem = new ThemedMenuItem("Select All", new BMessage(MSG_EDIT_SELECT_ALL),
                                        'A');
    fDupItem = new ThemedMenuItem("Duplicate", new BMessage(MSG_EDIT_DUPLICATE), 'D');
    fDupItem->SetEnabled(false);
    editMenu->AddItem(fCutItem);
    editMenu->AddItem(fCopyItem);
    editMenu->AddItem(fPasteItem);
    editMenu->AddItem(fDeleteItem);
    editMenu->AddItem(fSelectAllItem);
    editMenu->AddItem(fDupItem);
    editMenu->AddSeparatorItem();
    fSilenceItem = new ThemedMenuItem("Insert Silence",
                                      new BMessage(MSG_EDIT_INSERT_SIL), 'I',
                                      B_SHIFT_KEY);
    fSilenceItem->SetEnabled(false);
    editMenu->AddItem(fSilenceItem);
    menuBar->AddItem(new ThemedMenuItem(editMenu));
    BMenu* trackMenu = new BMenu("Track");
    trackMenu->AddItem(new ThemedMenuItem("New Audio Track", new BMessage(MSG_NEW_AUDIO)));
    trackMenu->AddItem(new ThemedMenuItem("New MIDI Track", new BMessage(MSG_NEW_MIDI)));
    trackMenu->AddItem(new ThemedMenuItem("New Bus", new BMessage(MSG_NEW_BUS)));
    trackMenu->AddSeparatorItem();
    // Duplicate Track (M2.6): the whole track, clips and all, right below the
    // source. Disabled until a track is selected (UpdateEditMenu keeps it).
    fDupTrackItem = new ThemedMenuItem("Duplicate Track",
                                       new BMessage(MSG_DUPLICATE_TRACK));
    fDupTrackItem->SetEnabled(false);
    trackMenu->AddItem(fDupTrackItem);
    menuBar->AddItem(new ThemedMenuItem(trackMenu));
    BMenu* helpMenu = new BMenu("Help");
    helpMenu->AddItem(new ThemedMenuItem("About Haiku DAW" B_UTF8_ELLIPSIS,
                                    new BMessage(MSG_ABOUT)));
    menuBar->AddItem(new ThemedMenuItem(helpMenu));

    BMenu* viewMenu = new BMenu("View");
    viewMenu->AddItem(new ThemedMenuItem("Mixer", new BMessage(MSG_MIXER)));
    // The markers list (M2.7): a locations window of its own, single-instance
    // like the mixer.
    viewMenu->AddItem(new ThemedMenuItem("Markers", new BMessage(MSG_MARKERS),
                                         'M'));
    viewMenu->AddItem(new ThemedMenuItem("Master Effects" B_UTF8_ELLIPSIS,
                                    new BMessage(MSG_MASTER_FX)));
    viewMenu->AddSeparatorItem();
    fDimItem = new ThemedMenuItem("Monitor: Dim", new BMessage(MSG_MON_DIM));
    viewMenu->AddItem(fDimItem);
    fMonoItem = new ThemedMenuItem("Monitor: Mono", new BMessage(MSG_MON_MONO));
    viewMenu->AddItem(fMonoItem);
    viewMenu->AddSeparatorItem();
    // The panes (M1.4): the inspector toggles with I, the dock with J.
    fInspectorItem = new ThemedMenuItem("Inspector", new BMessage(MSG_TOGGLE_INSPECTOR), 'I');
    fInspectorItem->SetMarked(true);
    viewMenu->AddItem(fInspectorItem);
    fDockItem = new ThemedMenuItem("Editor & Browsers", new BMessage(MSG_TOGGLE_DOCK), 'J');
    viewMenu->AddItem(fDockItem);
    // (Dark Mode moved into the Preferences window (M3.5), which is where the
    // theme package promised it would land; MSG_THEME_MODE is unchanged and
    // the window posts it.)
    viewMenu->AddSeparatorItem();
    viewMenu->AddItem(new ThemedMenuItem("Zoom to Fit", new BMessage(MSG_ZOOMFIT), 'F'));
    fFollowItem = new ThemedMenuItem("Follow Playhead", new BMessage(MSG_FOLLOW));
    fFollowItem->SetMarked(true);   // chase on by default
    viewMenu->AddItem(fFollowItem);
    // The marker lane (M2.7): a flag strip between the ruler and the lanes.
    fMarkerLaneItem = new ThemedMenuItem("Marker Lane",
                                         new BMessage(MSG_MARKER_LANE));
    viewMenu->AddItem(fMarkerLaneItem);
    viewMenu->AddItem(new ThemedMenuItem("Keyboard Shortcuts" B_UTF8_ELLIPSIS,
                                    new BMessage(MSG_SHORTCUTS)));
    viewMenu->AddItem(new ThemedMenuItem("Sample Browser" B_UTF8_ELLIPSIS,
                                    new BMessage(MSG_BROWSER)));
    menuBar->AddItem(new ThemedMenuItem(viewMenu));

    // Audio: the quick toggles only (M3.5). Buffer size, count-in and input
    // monitoring are preferences now and live in the Preferences window; what
    // is left here is what a user flips while working, and the metronome is
    // the one that belongs to audio.
    BMenu* audioMenu = new BMenu("Audio");
    fMetItem = new ThemedMenuItem("Metronome", new BMessage(MSG_METRONOME));
    fMetItem->SetMarked(fTransportCtl.fMetronome);
    audioMenu->AddItem(fMetItem);
    menuBar->AddItem(new ThemedMenuItem(audioMenu));

    // --- Transport bar (below the menu, M3.1) ---
    // The bar draws its own buttons, the LCD well and the status line; the
    // widgets below are its children, placed by LayoutTransportBar (their
    // design offsets here are only the starting geometry).
    BRect barRect(0, 0, bounds.right, Themed(kTransportH));
    TransportActions actions;
    actions.play    = MSG_PLAY;
    actions.stop    = MSG_STOP;
    actions.rec     = MSG_REC;
    actions.zoomOut = MSG_ZOOM_OUT;
    actions.zoomIn  = MSG_ZOOM_IN;
    actions.rtz     = kMsgTransportRtz;
    actions.rewind  = kMsgTransportRewind;
    actions.forward = kMsgTransportForward;
    actions.loop    = kMsgTransportLoop;
    actions.click   = kMsgClickToggle;   // the same action the C key posts
    actions.punch   = kMsgTransportPunch;
    fTransport = new TransportBar(barRect, BMessenger(this), actions);
    BView* bar = fTransport;

    // "Vol" label + master volume slider (0..150% -> gain 0..1.5). Row B --
    // the row under the buttons -- so the transport row keeps its width for
    // the LCD and the tempo field.
    fVolLbl = new BStringView(BRect(Themed(602), Themed(35), Themed(632),
                                    barRect.bottom - Themed(4)),
                              "vollbl", "Vol");
    bar->AddChild(fVolLbl);
    // NOTE (package 52, item G): this slider does NOT show its value, and it is
    // the one control in the app that G names which could not be given one. A
    // BSlider that has value text reserves a text line ABOVE its bar (the bar's
    // frame moves down by a font height), and this row is 20 px tall in a 54 px
    // bar whose rows above it are full: the text would push the trough and its
    // handle out of the view entirely. Widening the bar's rows is the transport
    // bar's own layout (M3.1/M3.3), not this package's; the mixer's master strip
    // -- the same fader -- has shown its level in dB since M3.4, and that is
    // where a master level is read.
    fMaster = new BSlider(BRect(Themed(634), Themed(33), Themed(760),
                                barRect.bottom - Themed(3)),
                          "master", NULL, new BMessage(MSG_MASTER),
                          0, 150, B_HORIZONTAL);
    fMaster->SetModificationMessage(new BMessage(MSG_MASTER));
    fMaster->SetValue((int32)(fProject->masterGain * 100.0f));
    bar->AddChild(fMaster);

    // "BPM" label + tempo field (affects grid/snap + metronome on the next
    // Play). Row A, right of the LCD.
    fBpmLbl = new BStringView(BRect(Themed(602), Themed(10), Themed(636),
                                    Themed(30)),
                                          "bpmlbl", "BPM");
    bar->AddChild(fBpmLbl);
    char bpm[16];
    std::snprintf(bpm, sizeof(bpm), "%.0f", fProject->tempoBPM);
    // A kit text field, so the well behind the digits is the theme's rather
    // than a stock white box in the dark look (T1).
    fTempo = new DawTextField(BRect(Themed(638), Themed(9), Themed(704),
                                    Themed(29)),
                              "tempo", NULL, bpm, new BMessage(MSG_TEMPO));
    fTempo->SetDivider(0.0f);
    bar->AddChild(fTempo);

    // Loudness readout (momentary / short-term LUFS + true peak dBTP), in the
    // transport row left of the tempo field.
    fLoudView = new BStringView(BRect(Themed(722), Themed(10), Themed(858),
                                      Themed(28)),
                                "loud", "M --  S --  TP --");
    {
        // A size down from the bar's labels: the readout has to hold three
        // signed figures in the room between the LCD and the tempo field.
        BFont small(be_plain_font);
        small.SetSize(Themed(10.5f));
        fLoudView->SetFont(&small);
    }
    bar->AddChild(fLoudView);

    // Master output meter, pinned to the right of the transport bar; it spans
    // both rows, so it reads like the console's master meter.
    fMeter = new MeterView(BRect(bounds.right - Themed(130), Themed(4),
                                 bounds.right - Themed(6),
                                 Themed(kTransportH) - Themed(4)));
    bar->AddChild(fMeter);

    // --- The panes (M1.4) ---
    // Top row: the inspector (resizable, collapsible, toggled with I) beside
    // the timeline. Bottom: the dock, which hosts the MIDI editor and the
    // browsers and starts collapsed. A window-level layout drives both, so
    // resizing the window is the layout's business now, not FrameResized's.
    fPaneSplit = new BSplitView(B_HORIZONTAL, 0.0f);
    fInspector = new InspectorView(BRect(0, 0, InspectorWidth(), 100),
                                   project, stack);
    fTimeline = new TimelineView(BRect(0, 0, 400, 100), project, stack);
    fTimeline->SetPeaks(peaks);
    // Both panes can be made narrow (the timeline culls to its visible rect,
    // the inspector stacks its rows), and SAYING so matters: a layout view
    // without an explicit minimum reports its current frame as one, so the
    // timeline's width became the window's floor -- the window could not be
    // made narrower again once it had been wide, and on a small screen its
    // right end (the dock's own Pop out and x) sat off the screen entirely.
    fInspector->SetExplicitMinSize(BSize(Themed(120.0f), Themed(80.0f)));
    // ...and the timeline's is the arrange tool strip (M2.3) plus a little: the
    // strip is drawn inside the timeline, so a narrower pane would clip its own
    // buttons. The height keeps the tool strip + ruler + one lane usable.
    fTimeline->SetExplicitMinSize(
        BSize(std::max(Themed(240.0f), ArrToolStripWidth() + Themed(8.0f)),
              Themed(80.0f)));
    fPaneSplit->AddChild(fInspector, 0.0f);   // keeps its width...
    fPaneSplit->AddChild(fTimeline, 1.0f);    // ...the timeline takes the rest
    fPaneSplit->SetCollapsible(0, true);

    // The dock is a title strip over a body, the way the bottom editor panel
    // reads in other DAWs: the strip names what is open and carries the pane's
    // own buttons, the body is the editor or, until a region is opened, a hint
    // saying how to open one. No tab bar while there is only one thing to
    // show -- a lone unlabelled tab was all the old BTabView added.
    fDock = new BGroupView("dock", B_VERTICAL, 0.0f);
    {
        PaneHeader* head = new PaneHeader("dockhead");
        // The page switch lives in the strip (UI_GUIDELINES §4: no tab bar for
        // this) -- the pages are Editor, Samples and Plugins, and the title
        // beside it names what the current one holds.
        static const char* const kPages[] = { "Editor", "Samples", "Plugins" };
        fDockSegments = new DawSegments("dockpages", kPages, 3,
                                        new BMessage(MSG_DOCK_PAGE));
        fDockSegments->SetSelected(fDockPage);
        // Exactly as wide as its labels need: the strip's other controls (the
        // title, Pop out, the x) come first in the layout's list of things that
        // must fit, and a segmented control that stretched pushed them off the
        // right edge entirely.
        {
            float sw = 0.0f, sh = 0.0f;
            fDockSegments->GetPreferredSize(&sw, &sh);
            fDockSegments->SetExplicitMinSize(BSize(sw, sh));
            fDockSegments->SetExplicitMaxSize(BSize(sw, sh));
        }
        head->GetLayout()->AddView(fDockSegments);
        fDockTitle = new BStringView("edtitle", "Editor");
        BFont bold(be_bold_font);
        bold.SetSize(Themed(11.5f));
        fDockTitle->SetFont(&bold);
        fDockTitle->SetTruncation(B_TRUNCATE_END);
        fDockTitle->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
        head->GetLayout()->AddView(fDockTitle);
        fDockPop = HeaderButton("popout", "Pop out", MSG_POP_OUT_EDITOR);
        fDockPop->SetEnabled(false);          // nothing to pop out yet
        head->GetLayout()->AddView(fDockPop);
        head->GetLayout()->AddView(
            HeaderButton("dockclose", "\xC3\x97", MSG_TOGGLE_DOCK));   // x
        fDock->GetLayout()->AddView(head);
    }
    fEditorPane = new BGroupView(B_VERTICAL, 0.0f);
    {
        fDockHint = new BStringView("dockhint",
            "Double-click a MIDI region to edit it here");
        fDockHint->SetAlignment(B_ALIGN_CENTER);
        fDockHint->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
        // Glue either side centres it (a BStringView stretched to the body
        // draws its text at the bottom), and the group is what the roll swaps
        // with, so the glue goes when the hint does.
        BGroupView* empty = new BGroupView("dockempty", B_VERTICAL, 0.0f);
        empty->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
        empty->GroupLayout()->AddView(fDockHint);
        empty->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
        fDockEmpty = empty;
        fEditorPane->GroupLayout()->AddView(empty, 1.0f);
    }
    fDock->GetLayout()->AddView(fEditorPane);
    // Room for a usable editor: a pane dragged smaller than this is a header
    // and a sliver, which is the look this replaces. Hiding it is J or the x.
    fDock->SetExplicitMinSize(BSize(B_SIZE_UNSET, Themed(160.0f)));

    // Thin dividers with a few pixels to grab (DawControlLook draws them).
    fRootSplit = new BSplitView(B_VERTICAL, 0.0f);
    fRootSplit->SetSplitterSize(Themed(5.0f));
    fPaneSplit->SetSplitterSize(Themed(5.0f));
    fRootSplit->AddChild(fPaneSplit, 1.0f);   // the dock joins on demand

    BLayoutBuilder::Group<>(this, B_VERTICAL, 0.0f)
        .Add(menuBar)
        .Add(bar)
        .Add(fRootSplit)
        .End();

    // Every colour the bar and the dock keep is taken in one place, so a mode
    // switch and the constructor cannot drift apart (T1).
    ApplyTheme();
    // Restore persisted preferences + window layout (after the menus exist).
    LoadSettings();
    // A restored window frame resizes the bar before the user touches anything,
    // so the layout has to be applied once here as well as on every resize.
    LayoutTransportBar();
    // The bar's readouts start on the real state, not on a placeholder: the
    // position in the current format, the tempo/meter line, and the lamps.
    UpdateTimeReadout(fProject->transport.playhead);
    UpdateTransportReadouts();
    // Below this the bar has nothing left to hide: the transport controls, the
    // time readout and the meter need the room, and a window that clipped them
    // would be showing a fault rather than a small window.
    SetSizeLimits(620.0f, 32767.0f, 300.0f, 32767.0f);
    // Autosave for crash recovery at the preference's interval (M3.5; 30 s
    // unless the user chose otherwise, and off when they chose 0). LoadSettings
    // above has already put the interval in place; check for a leftover once
    // the looper runs.
    StartAutosave(fPrefsAutosaveSeconds);
    fTitlePoll = new BMessageRunner(BMessenger(this), new BMessage(MSG_TITLE),
                                    500LL * 1000);        // 2 Hz dirty marker
    UpdateTitle();
    PostMessage(MSG_RECOVER);
}

MainWindow::~MainWindow() {
    // The peak loader only touches files and its own list, but it must not
    // outlive the window that drains it (M4.4).
    fPeakLoader.Stop();
    // The export AND region workers render a snapshot taken on this thread,
    // so both have to stop before anything they touch goes away. The cancel
    // flags are polled between blocks, so this waits at most one block (and
    // none at all when nothing is running); a joinable thread left behind
    // would terminate the process at destruction.
    fRender.Stop();
    delete fPulse;
    delete fAutosave;
    delete fTitlePoll;
    delete fAuditionIdle;   // the audition's idle stopper (package 51)
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
        // The timeline takes the map's keys unless a TEXT FIELD is being typed
        // into -- the one thing that holds them. A control that merely has the
        // focus keeps only the keys the map does not bind: clicking the master
        // volume slider or a browser's list used to swallow R/S/M/Home/,/. and
        // the arrows, because any focus held everything but Space. The rule is
        // the kit-free TimelineTakesKey(); Tab still belongs to BWindow's own
        // focus navigation, and Cmd shortcuts keep flowing to the menus.
        BView* focus = CurrentFocus();
        int32 mods = 0;
        message->FindInt32("modifiers", &mods);
        const char* bytes = nullptr;
        const bool haveBytes = message->FindString("bytes", &bytes) == B_OK
                            && bytes != nullptr;
        const bool isTab = haveBytes && bytes[0] == B_TAB;
        // A focused KIT BUTTON owns Space and Return -- the Haiku rule (Space
        // presses it, Return is the window's default) and since package 49
        // every button is B_NAVIGABLE, so the user can tab to one. The map's
        // Space is the transport's play/stop and its Return returns to zero,
        // but only where the keyboard is not explicitly on a button.
        if (haveBytes && (bytes[0] == B_SPACE || bytes[0] == B_RETURN)
            && dynamic_cast<BButton*>(focus) != nullptr) {
            BWindow::DispatchMessage(message, handler);
            return;
        }
        // ... except when the DOCKED MIDI editor holds the focus: the roll is
        // custom drawn and carries its own editor keys (its tool palette, the
        // grid menu, the nudge arrows, Del), deliberately OUTSIDE the arrange's
        // one key map (model/KeyMap.h says why). They go to the roll, exactly
        // as its Edit-menu shortcuts do (FocusedRoll), and this runs BEFORE the
        // timeline's rule below -- otherwise the arrange's own bindings for
        // those keys would take them and the editor's tools would be dead
        // while docked. Keys the roll does not use (Space, R, Home ...) are
        // untouched and keep going wherever the map sends them.
        if (!(mods & B_COMMAND_KEY) && haveBytes) {
            PianoRollView* docked = FocusedRoll();
            if (docked != nullptr && RollEditorKey(bytes[0])) {
                docked->KeyDown(bytes, 1);
                return;
            }
        }
        // Space needs no special case here: it is bound in the map (the
        // transport's toggle), so TimelineTakesKey keeps it and the text-field
        // rule above is the only thing that can hold it back.
        const bool textFocus = dynamic_cast<BTextControl*>(focus) != nullptr
                            || dynamic_cast<BTextView*>(focus) != nullptr;
        const bool mapTakes = haveBytes
                           && TimelineTakesKey(bytes[0], (uint32_t)mods,
                                              textFocus);
        // An unbound key is still the focused control's (a list's Up/Down, a
        // slider's own keys); with nothing focused there is nobody else to give
        // it to, so it goes to the timeline as it always did.
        const bool held = textFocus
                       || (!mapTakes && focus != nullptr && focus != fTimeline);
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
        case MSG_PLAY:
            fTransportCtl.StartPlayback();
            UpdateTransportReadouts();   // the Play lamp + the status line
            break;
        case MSG_STOP:
            fTransportCtl.StopPlayback();
            fRecCtl.StopRecording();
            UpdateTransportReadouts();
            break;
        case MSG_REC:
        case kMsgTransportRecord:   // the R key (M3.2), same as the Rec button
            // Toggle: Rec starts, Rec again (or Stop) finishes the take.
            if (fRecCtl.fRecMode) fRecCtl.StopRecording();
            else          fRecCtl.StartRecording();
            UpdateTransportReadouts();
            break;
        case kMsgMonitorRefresh:   // arming / input changed in the timeline
            fRecCtl.UpdateIdleMonitor();
            break;
        case kMsgTrackSelected: {  // point the inspector at the clicked track
            int64 tid = 0;
            msg->FindInt64("track", &tid);
            SelectTrack((TrackId)tid);
            break;
        }
        case kMsgUiRefresh: {      // a track edit: keep the panes + mixer consistent
            if (fInspector) fInspector->Invalidate();
            if (fTimeline)  fTimeline->Invalidate();
            // A track that was DELETED takes its editors with it: the sends and
            // instrument windows hold a whole-snapshot copy addressed by track
            // id, and one left up would post that snapshot back for a track the
            // model no longer has (dropped), or -- worse, if the id were ever
            // reused -- for a different track (package 52).
            CloseEditorsForGoneTracks();
            PushMixerStrips();     // an open mixer's strip state (one encoder)
            // ... and an open markers window, whose list is model data too. A
            // marker edit posts this same message, so one funnel keeps the
            // list honest (a list that misses an edit is stale content).
            PushMarkers();
            // ... and the MIDI editor: anything that posts a refresh may have
            // been the model edit a roll's snapshot is now too old to commit
            // (package 51).
            RefreshRolls();
            break;
        }
        case kMsgCycleAuto:        // inspector Auto button -> cycle timeline mode
            if (fInspector && fTimeline)
                fTimeline->CycleAuto(fInspector->SelectedTrack());
            break;
        case kMsgRollOpened: {     // a piano roll opened; remember it for the playhead
            BMessenger m;
            if (msg->FindMessenger("m", &m) == B_OK) {
                // One popped-out editor at a time, the fresh-messenger rule the
                // other single-instance windows follow: the window that was
                // registered before this one would stop receiving pushes (only
                // one messenger is kept) and would then have every later commit
                // refused, silently. Closing it keeps one roll, always synced.
                if (fRollMsgr.IsValid() && fRollMsgr != m)
                    fRollMsgr.SendMessage(B_QUIT_REQUESTED);
                fRollMsgr = m;
            }
            // Which region it opened on: the window names it, because the push
            // below has to say what the roll is looking at (package 51).
            int64 tid = 0, cid = 0;
            if (msg->FindInt64("track", &tid) == B_OK) fRollTrack = (TrackId)tid;
            if (msg->FindInt64("clip", &cid) == B_OK)  fRollClip  = (ClipId)cid;
            // A roll that just opened must be told the region's state and the
            // follow policy before anything else reaches it (package 51).
            RefreshRolls(true);
            PushRollFollow();
            break;
        }
        case kMsgSeek:
        case kMsgTransportRange: {   // M4.2: the loop range changed under a roll
            const Frame ph = fProject->transport.playhead;
            UpdateTimeReadout(ph);
            UpdateTransportReadouts();   // the Loop lamp follows the model
            if (fTransportCtl.fPlaying)        // restart from the new position
                fTransportCtl.StartPlayback();
            break;
        }
        // A view's own status message (package 50: the timeline's loop toggle
        // says which range it adopted, or that it had none to adopt). The
        // status line keeps it until the transport state changes, like every
        // other job/error message (UpdateTransportStatus).
        case kMsgTransportStatus: {
            const char* s = nullptr;
            if (msg->FindString("s", &s) == B_OK && s) SetTransportStatus(s);
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
        // Count-in and input monitoring are preferences now (M3.5) and the
        // Preferences window is where they are chosen; these ids are kept and
        // land on the same funnel, so anything that still posts them (a test,
        // a future key) behaves exactly like the window.
        case MSG_COUNTIN: {
            int32 bars = 0;
            msg->FindInt32("bars", &bars);
            PrefsView v = CurrentPrefs();
            v.countInBars = bars;
            ApplyPrefs(v);
            break;
        }
        case MSG_MONITOR_IN: {
            PrefsView v = CurrentPrefs();
            v.monitorInput = !v.monitorInput;
            ApplyPrefs(v);
            break;
        }
        case MSG_METRONOME:
        case kMsgClickToggle:   // the C key (M3.2) and the bar's Click button
            fTransportCtl.fMetronome = !fTransportCtl.fMetronome;
            if (fMetItem) fMetItem->SetMarked(fTransportCtl.fMetronome);
            if (fTransportCtl.fEngine) fTransportCtl.fEngine->SetMetronome(fTransportCtl.fMetronome);
            UpdateTransportReadouts();
            break;
        case kMsgTransportLayout:   // the bar's width changed (see TransportBar)
            LayoutTransportBar();
            break;
        // --- the transport bar's own controls (M3.1) ------------------------
        // Return to zero, rewind and fast-forward run exactly what the
        // keyboard map runs (TimelineView owns the playhead math); loop toggles
        // through the timeline's own ToggleLoop, which owns the "adopt the
        // last range / the markers" rule.
        case kMsgTransportRtz:
            if (fTimeline) fTimeline->ReturnToZero();
            break;
        case kMsgTransportRewind:  NudgePlayheadBar(-1); break;
        case kMsgTransportForward: NudgePlayheadBar(+1); break;
        case kMsgTransportLoop:
            if (fTimeline) fTimeline->ToggleLoop();   // posts kMsgTransportRange
            UpdateTransportReadouts();
            break;
        case kMsgTransportPunch:
            fProject->transport.punchEnabled = !fProject->transport.punchEnabled;
            UpdateTransportReadouts();
            break;
        case kMsgTimeCycle:   // the big display was clicked: next format
            fTimeMode = NextTimeMode(fTimeMode);
            UpdateTimeReadout(fProject->transport.playhead);
            break;
        case kMsgTimeEntry: {
            // What was typed in the display, in the format it was typed in
            // (M3.1). A position that cannot be read changes nothing and says
            // so in the status line: a plausible-but-wrong seek is worse than
            // a refusal.
            const char* text = nullptr;
            if (msg->FindString("text", &text) != B_OK || text == nullptr) break;
            fProject->tempoMap.sampleRate = fProject->sampleRate;
            const double rate = fTransportCtl.fEngine
                              ? fTransportCtl.fEngine->OutputRate()
                              : fProject->sampleRate;
            Frame where = 0;
            if (ParseTimePosition(fTimeMode, text, rate, fProject->tempoMap, &where)) {
                if (fTimeline) fTimeline->SeekTo(where);
                SetTransportStatus("Position set");
            } else {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "Cannot read '%s' as %s (%s)",
                              text, TimeModeTag(fTimeMode), TimeModeHint(fTimeMode));
                SetTransportStatus(buf);
            }
            break;
        }
        case kMsgMeterClipReset:
            // The master meter was clicked. Clear the latched flags where they
            // live (the engine, or the recorder during a take); the fetch_and
            // keeps a clip that happened since the last poll.
            if (fTransportCtl.fEngine)
                fTransportCtl.fEngine->ClearClipFlags(kMeterClipBoth);
            if (fRecCtl.fRecorder)
                fRecCtl.fRecorder->ClearClipFlags(kMeterClipBoth);
            break;
        case MSG_TOGGLE_INSPECTOR:
            SetInspectorShown(!fInspectorShown);
            break;
        case kMsgOpenEditor: {
            int64 track = 0, clip = 0;
            msg->FindInt64("track", &track);
            msg->FindInt64("clip", &clip);
            OpenDockedEditor((TrackId)track, (ClipId)clip);
            break;
        }
        case MSG_POP_OUT_EDITOR:
            PopOutDockPage();
            break;
        case B_COLORS_UPDATED:
            // The user changed the Appearance while the app is running. Haiku
            // sends this to the windows, so the main window is where the DAW
            // follows it: new base colours, new tokens, every cached view
            // colour re-taken — and only in System mode, which is the mode
            // that is supposed to look like the user's system. Nothing is
            // written back (T1).
            if (ActiveThemeMode() == ThemeMode::System) {
                ReadSystemBaseColors();
                ApplyThemeToAllWindows();
            }
            break;

        case MSG_DOCK_PAGE: {
            // The segmented control names the segment it selected as int32
            // "index" (DawSegments::Invoke). A message without it -- anything
            // that is not that control -- leaves the page alone rather than
            // silently meaning Editor (T2 review).
            int32 index = 0;
            if (msg->FindInt32("index", &index) != B_OK) break;
            SetDockPage(index);
            break;
        }
        case MSG_THEME_STOCK:
            // A stock window (a file panel) finished building its views after
            // Show() returned: one more pass over them.
            ThemeStockWindows();
            break;

        case kMsgPluginNoTrack:
            // A pick in the dock's Plugins page with no track selected: the
            // page cannot add an insert to a chain nobody named, so it says
            // what it needs (see PluginBrowserView::PostChoice).
            SetTransportStatus("Select a track first");
            break;

        case MSG_THEME_MODE:
            // Toggle the look. It was the View menu's Dark Mode item; that
            // item moved into the Preferences window (M3.5), which posts its
            // whole state as kMsgPrefsChanged instead (ApplyPrefs sets the
            // mode from the "dark" field). This id is kept for the functional
            // test, which is what drives the switch now.
            SetThemeMode(ActiveThemeMode() == ThemeMode::Dark ? ThemeMode::System
                                                              : ThemeMode::Dark);
            break;

        case MSG_PROJECT_SETTINGS:      // File > Project Settings… (M3.6)
            ShowProjectSettingsWindow();
            break;
        case kMsgProjSettingsChanged: {
            // Apply was pressed: the differences are one edit (see
            // ApplyProjectSettings).
            ProjectSettingsView v = CurrentProjectSettings();
            double d = 0.0;
            int32 i = 0;
            bool b = false;
            const char* s = nullptr;
            if (msg->FindDouble("rate", &d) == B_OK)  v.sampleRate = d;
            if (msg->FindDouble("tempo", &d) == B_OK) v.tempoBPM = d;
            if (msg->FindInt32("mnum", &i) == B_OK)   v.meterNum = i;
            if (msg->FindInt32("mdenom", &i) == B_OK) v.meterDenom = i;
            if (msg->FindString("notes", &s) == B_OK && s) v.notes = s;
            if (msg->FindBool("resample", &b) == B_OK) v.resampleMedia = b;
            ApplyProjectSettings(v);
            break;
        }
        case MSG_PREFERENCES:
            ShowPreferencesWindow();
            break;
        case kMsgPrefsChanged: {
            // One message carries the whole set; missing fields keep what the
            // window last showed (a partial push cannot blank a preference).
            PrefsView v = CurrentPrefs();
            int32 i = 0;
            float f = 0.0f;
            bool b = false;
            const char* s = nullptr;
            if (msg->FindInt32("buffer", &i)  == B_OK) v.bufferFrames = i;
            if (msg->FindInt32("defrate", &i) == B_OK) v.defaultSampleRate = i;
            if (msg->FindInt32("countin", &i) == B_OK) v.countInBars = i;
            if (msg->FindInt32("metsnd", &i)  == B_OK) v.metronomeSound = i;
            if (msg->FindFloat("metlvl", &f)  == B_OK) v.metronomeLevel = f;
            if (msg->FindBool("monin", &b)    == B_OK) v.monitorInput = b;
            if (msg->FindBool("preview", &b)  == B_OK) v.previewNotes = b;
            if (msg->FindBool("dark", &b)     == B_OK) v.darkMode = b;
            if (msg->FindFloat("uiscale", &f) == B_OK) v.uiScale = f;
            if (msg->FindString("projdir", &s) == B_OK && s) v.projectDir = s;
            if (msg->FindInt32("autosv", &i)  == B_OK) v.autosaveSeconds = i;
            ApplyPrefs(v);
            break;
        }

        case MSG_TOGGLE_DOCK:
            SetDockShown(fDock == nullptr || fDock->Parent() == nullptr);
            break;
        case MSG_FOLLOW: {
            const bool on = !(fFollowItem && fFollowItem->IsMarked());
            if (fFollowItem) fFollowItem->SetMarked(on);
            fTimeline->SetFollow(on);
            // One policy, every view (package 51): the roll's follow used to be
            // a hard-coded true the menu could not switch off.
            PushRollFollow();
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
            if (msg->FindInt32("container", &v) == B_OK
                && AudioFileFormatCanWrite((AudioFileFormat)v))
                fExportChoices.container = (AudioFileFormat)v;
            if (msg->FindFloat("vquality", &f) == B_OK)
                fExportChoices.vorbisQuality = f;
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
                ShowPanel(fStemsPanel, "Export Stems");
            } else {
                if (!fExportPanel) {
                    BMessenger to(this);
                    fExportPanel = new BFilePanel(B_SAVE_PANEL, &to, NULL, 0, false,
                                                  new BMessage(MSG_EXPORT_REF));
                }
                // The prefilled name carries the container's own extension, so
                // a FLAC bounce is not offered as "mix.wav".
                ShowPanel(fExportPanel, "Export Mix",
                          ProjectDocument::DisplayName(fDoc.Path()) + "."
                          + AudioFileFormatExtension(fExportChoices.container));
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
            // the bar never disappears before the outcome is known. The same
            // bar serves the region jobs (M4.4), so both flags are raised --
            // whichever job is running is the one that reads its own.
            fRender.fExportCancel.store(true);
            fRender.fRegionCancel.store(true);
            break;
        case MSG_IMPORT_MIDI:
            if (!fMidiImportPanel) {
                BMessenger to(this);
                fMidiImportPanel = new BFilePanel(B_OPEN_PANEL, &to, NULL, 0,
                    false, new BMessage(MSG_IMPORT_MIDI_REF));
            }
            ShowPanel(fMidiImportPanel, "Import MIDI");
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
            ShowPanel(fMidiExportPanel, "Export MIDI", ProjectDocument::DisplayName(fDoc.Path()) + ".mid");
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
            // A track-list edit refreshes the console (package 52): a new track
            // had no strip, so it could not be mixed at all until something else
            // happened to refresh the rack.
            PostMessage(kMsgUiRefresh);
            break;
        }
        case MSG_MASTER_FX:
            // One window (T2): the master chain, whole. (Its sidechain
            // candidates are ShowFxWindow's business now.)
            ShowFxWindow(kMasterFxTarget, -1);
            break;
        case kMsgFxWinOpen: {   // an effects editor opened: meter its track
            int64 tid = 0; BMessenger m;
            msg->FindInt64("track", &tid);
            msg->FindMessenger("msgr", &m);
            fFxMsgr = m;
            fFxTrack = (TrackId)tid;
            // Which project generation it is addressed to (package 52): the
            // window is closed when the project is replaced, because its whole
            // chain is committed by track id and a loaded file reuses those ids.
            fFxScope = fScope.Current();
            if (fTransportCtl.fEngine) fTransportCtl.fEngine->SetMeterFocus((TrackId)tid);
            break;
        }
        case kMsgFxWinClosed: {
            int64 tid = 0;
            msg->FindInt64("track", &tid);
            if (fFxTrack == (TrackId)tid) {
                fFxMsgr = BMessenger();
                fFxTrack = kInvalidTrackId;
                fFxScope = 0;
                if (fTransportCtl.fEngine) fTransportCtl.fEngine->SetMeterFocus(kInvalidTrackId);
            }
            break;
        }
        case MSG_MIXER: {
            std::vector<MixerStripInfo> strips = BuildMixerStrips(*fProject, fTimeline);
            // One mixer (M1.4): asking again brings the open one forward with
            // the current strips rather than stacking a second rack.
            if (fMixerMsgr.IsValid()) {
                if (PushMixerStrips()) {
                    BMessage act(kMsgMixerActivate);   // bring it to the front
                    fMixerMsgr.SendMessage(&act);
                    break;
                }
                // The window is gone (its messenger is dead): fall through and
                // make a new one.
            }
            const float ww = 24 + (strips.size() + 1) * (96 + 4);   // + master
            // Tall enough for the strip, including the insert block: four insert
            // rows cost ~60 px that the old single "FX n" button did not.
            BRect wr(120, 90, 120 + ww, 90 + 620);
            MixerWindow* mx = new MixerWindow(wr, strips, fProject->masterGain,
                                              BMessenger(this));
            fMixerMsgr = BMessenger(mx);
            fMixerScope = fScope.Current();   // which project it is addressed to
            // The rack is a snapshot of the track layout: record what it was
            // given, so PushTrackPeaks can tell a stale layout from a current
            // one without rebuilding the payload to compare.
            fMixerLayout = TrackLayoutOf(*fProject);
            mx->Show();   // fits itself to the screen (see MixerWindow::FitToScreen)
            break;
        }
        case MSG_MARKERS:      // View > Markers (M2.7)
            ShowMarkersWindow();
            break;
        case MSG_MARKER_LANE:  // View > Marker Lane: a view preference
            fTimeline->SetMarkerLane(!fTimeline->MarkerLaneOn());
            if (fMarkerLaneItem)
                fMarkerLaneItem->SetMarked(fTimeline->MarkerLaneOn());
            SaveSettings();    // it survives the quit, like the panes
            break;
        case MSG_DUPLICATE_TRACK:   // Track > Duplicate Track (M2.6)
            fTimeline->DuplicateSelectedTrack();
            // The duplicate is a new track: the console needs its strip
            // (package 52). DuplicateSelectedTrack posts its own refresh for the
            // panes; this is the console's copy of the same rule.
            PostMessage(kMsgUiRefresh);
            break;
        case kMsgMarkerJump: {      // the markers window: locate
            int64 fr = 0;
            msg->FindInt64("frame", &fr);
            fTimeline->SeekTo((Frame)fr);
            break;
        }
        case kMsgMarkerAdd:         // the markers window: add at the playhead
            fTimeline->AddMarkerAt(fProject->transport.playhead);
            break;
        case kMsgMarkerDelete: {    // the markers window: remove one
            int64 fr = 0;
            const char* name = nullptr;
            msg->FindInt64("frame", &fr);
            if (msg->FindString("name", &name) == B_OK && name)
                fTimeline->RemoveMarkerAt((Frame)fr, name);
            break;
        }
        case kMsgMixArm: {
            int64 tid = 0; msg->FindInt64("track", &tid);
            if (Track* t = fProject->FindTrack((TrackId)tid)) t->armed = !t->armed;
            fRecCtl.UpdateIdleMonitor();
            if (fInspector) fInspector->Invalidate();
            fTimeline->Invalidate();
            break;
        }
        case kMsgMixMon: {
            int64 tid = 0; msg->FindInt64("track", &tid);
            if (Track* t = fProject->FindTrack((TrackId)tid))
                t->inputMonitor = !t->inputMonitor;
            fRecCtl.UpdateIdleMonitor();
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
            // The same single window the inspector's slot list opens (T2), so
            // the two strips behave the same -- and with it the sidechain
            // picker's candidates, which ShowFxWindow snapshots for the track.
            ShowFxWindow((TrackId)tid, slot);
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
            ShowDockBrowser(kDockPlugins, (TrackId)tid);
            break;
        }
        case kMsgMixSends: {
            int64 tid = 0; msg->FindInt64("track", &tid);
            ShowSendsWindow((TrackId)tid);
            break;
        }
        case kMsgMixInst: {
            int64 tid = 0; msg->FindInt64("track", &tid);
            ShowInstrumentWindow((TrackId)tid);
            break;
        }
        // The strip's routing pair and its automation mode (package 52). The
        // strip holds a snapshot, not the model, so it names the track and this
        // side does the work -- through the INSPECTOR's own pickers, so the two
        // places a track's input can be chosen cannot disagree.
        case kMsgMixInput: {
            int64 tid = 0; float x = 0, y = 0;
            msg->FindInt64("track", &tid);
            msg->FindFloat("x", &x); msg->FindFloat("y", &y);
            if (fInspector && fProject->FindTrack((TrackId)tid))
                fInspector->PickInput((TrackId)tid, BPoint(x, y));
            break;
        }
        case kMsgMixOutput: {
            int64 tid = 0; float x = 0, y = 0;
            msg->FindInt64("track", &tid);
            msg->FindFloat("x", &x); msg->FindFloat("y", &y);
            if (fInspector && fProject->FindTrack((TrackId)tid))
                fInspector->PickOutput((TrackId)tid, BPoint(x, y));
            break;
        }
        case kMsgMixAuto: {
            // The strip's Auto button: cycle the track's automation-lane mode
            // (the timeline owns it) and push the strips back, so the lamp
            // follows the mode that is now set.
            int64 tid = 0; msg->FindInt64("track", &tid);
            if (fProject->FindTrack((TrackId)tid) != nullptr) {
                fTimeline->CycleAuto((TrackId)tid);
                PostMessage(kMsgUiRefresh);
            }
            break;
        }
        case kMsgMixSelect: {
            // A strip click in the mixer selects the track: the inspector AND
            // the dock's plugin page follow it, through the same call the
            // timeline's own selection uses (T2 review: the page stayed on the
            // old track, so "add to <name>" and the inspector disagreed).
            int64 tid = 0; msg->FindInt64("track", &tid);
            SelectTrack((TrackId)tid);
            break;
        }
        case kMsgApplyMaster:
            // Through the stack, exactly like the transport bar's own master
            // slider (MSG_MASTER below): a drag's posts coalesce into one undo
            // step, Cmd-Z takes it back, and the edit marks the project DIRTY --
            // the old direct write did none of those, so a session whose only
            // change was the master level reported clean and File > Close threw
            // the level away without a prompt (package 52).
            {
                float g = 1.0f;
                if (msg->FindFloat("gain", &g) == B_OK) {
                    fStack->Execute(std::make_unique<SetMasterGainCommand>(g),
                                    *fProject);
                    UpdateTitle();
                }
            }
            if (fMaster) fMaster->SetValue((int32)(fProject->masterGain * 100.0f));
            break;
        case kMsgApplyNotes: {
            // A PianoRoll posts one region's edited note list (clip-relative).
            // Undoable via SetMidiClipNotesCommand (one step per gesture).
            //
            // Package 51: the message names the stack serial its snapshot was
            // based on ("base"). A roll that missed a change (an undo landed
            // while its gesture was in flight) has a snapshot the model no
            // longer matches, and writing it would silently replace that
            // change — the stale-list bug. Such a commit is refused, and the
            // roll is pushed the truth instead (RefreshRolls below).
            int64 tid = 0, cid = 0, base = 0;
            msg->FindInt64("track", &tid);
            msg->FindInt64("clip", &cid);
            // The base the roll committed against. The declaration above has
            // always been here and the field was never read, so every commit
            // recorded base 0 -- a serial no stack ever has -- and the gate's
            // staleness check compared against nothing.
            msg->FindInt64("base", &base);
            // A hosted roll always names its base (it is pushed the moment it
            // opens); one that never arrived — a test harness building a view
            // itself — has none, and keeps the pre-51 behaviour.
            if (!RollCommitOk(msg, (TrackId)tid, (ClipId)cid)) {
                RefreshRolls(true);
                break;
            }
            Track* tr = fProject->FindTrack((TrackId)tid);
            if (tr && tr->FindMidiClip((ClipId)cid)) {
                if (fStack->Execute(std::make_unique<SetMidiClipNotesCommand>(
                        (TrackId)tid, (ClipId)cid, ParseRollNotes(msg)),
                        *fProject))
                    RollCommitAccepted(fRollGate, (uint64_t)base,
                                       fStack->CurrentSerial(), (TrackId)tid,
                                       (ClipId)cid);
                fTimeline->Invalidate();
            }
            RefreshRolls(true);   // the roll re-reads what the model now has
            break;
        }
        case kMsgApplyMidiOp: {
            // A PianoRoll ran a named transform (quantize / humanize / legato /
            // transpose / velocity) on its snapshot and posted the result. The
            // command only records it -- see MidiOps.h for why the parameters
            // do not travel -- and names the undo step after the transform.
            int64 tid = 0, cid = 0, base = 0; int32 op = 0;
            msg->FindInt64("track", &tid);
            msg->FindInt64("clip", &cid);
            msg->FindInt32("op", &op);
            if (!RollCommitOk(msg, (TrackId)tid, (ClipId)cid)) {
                RefreshRolls(true);   // the same gate as above
                break;
            }
            Track* tr = fProject->FindTrack((TrackId)tid);
            if (tr && tr->FindMidiClip((ClipId)cid)) {
                if (fStack->Execute(std::make_unique<ApplyMidiOpCommand>(
                        (TrackId)tid, (ClipId)cid, ParseRollNotes(msg),
                        MidiOpName((MidiOp)op)), *fProject))
                    RollCommitAccepted(fRollGate, (uint64_t)base,
                                       fStack->CurrentSerial(), (TrackId)tid,
                                       (ClipId)cid);
                fTimeline->Invalidate();
            }
            RefreshRolls(true);
            break;
        }
        case kMsgApplyEvents: {
            // The piano roll's CC/bend lane posts one region's event list.
            // Undoable via SetMidiClipEventsCommand, which leaves notes alone.
            int64 tid = 0, cid = 0, base = 0;
            msg->FindInt64("track", &tid);
            msg->FindInt64("clip", &cid);
            // The base the roll committed against. The declaration above has
            // always been here and the field was never read, so every commit
            // recorded base 0 -- a serial no stack ever has -- and the gate's
            // staleness check compared against nothing.
            msg->FindInt64("base", &base);
            if (!RollCommitOk(msg, (TrackId)tid, (ClipId)cid)) {
                RefreshRolls(true);
                break;
            }
            Track* tr = fProject->FindTrack((TrackId)tid);
            if (tr && tr->FindMidiClip((ClipId)cid)) {
                // Every field is validated by ParseRollEvents: a partial or
                // out-of-range event would land as a CC0 at frame 0 — invisible
                // in the editor yet still saved and exported.
                if (fStack->Execute(std::make_unique<SetMidiClipEventsCommand>(
                        (TrackId)tid, (ClipId)cid, ParseRollEvents(msg)),
                        *fProject))
                    RollCommitAccepted(fRollGate, (uint64_t)base,
                                       fStack->CurrentSerial(), (TrackId)tid,
                                       (ClipId)cid);
                fTimeline->Invalidate();
            }
            RefreshRolls(true);
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
                PostMessage(kMsgUiRefresh);   // an open markers window follows
            }
            break;
        }
        case kMsgMixSend: {
            // One aux send's level (M3.4). The strip's knob writes the same
            // list the sends editor does — SetSendsCommand — so there is ONE
            // path from the UI to the model and one place the guarantee lives.
            int64 tid = 0; int32 index = -1; float level = 1.0f;
            msg->FindInt64("track", &tid);
            msg->FindInt32("index", &index);
            msg->FindFloat("level", &level);
            if (Track* t = fProject->FindTrack((TrackId)tid)) {
                if (index >= 0 && index < (int32)t->sends.size()) {
                    std::vector<Send> sends = t->sends;
                    sends[(size_t)index].level = level;
                    fStack->Execute(std::make_unique<SetSendsCommand>((TrackId)tid,
                                                                     std::move(sends)),
                                    *fProject);
                    UpdateTitle();
                    // A send's level is read from the graph's own send tables,
                    // which are built with the graph — the same "applies on
                    // the next Play" the sends editor's edits have had since
                    // A3. Rebuilding per knob move would rebuild a 320-clip
                    // project's graph dozens of times per gesture.
                    fTimeline->Invalidate();
                }
            }
            break;
        }
        case kMsgApplyMix: {
            int64 tid = 0; float g = 1, p = 0; bool mu = false, so = false;
            float trim = 1.0f; bool phase = false; int32 sgroup = 0;
            msg->FindInt64("track", &tid);
            msg->FindFloat("gain", &g);
            msg->FindFloat("pan", &p);
            msg->FindBool("mute", &mu);
            msg->FindBool("solo", &so);
            msg->FindFloat("trim", &trim);
            msg->FindBool("phase", &phase);
            msg->FindInt32("sgroup", &sgroup);
            if (Track* t = fProject->FindTrack((TrackId)tid)) {
                // One macro, so the whole strip -- gain, pan, mute, solo, the
                // trim, the polarity, the solo group and the mute-group
                // cascade -- is one undo step. Unchanged fields get no
                // sub-command at all.
                auto macro = std::make_unique<MacroCommand>("Mixer Strip");
                if (t->gain   != g)  macro->Add(std::make_unique<SetTrackGainCommand>((TrackId)tid, g));
                if (t->pan    != p)  macro->Add(std::make_unique<SetTrackPanCommand>((TrackId)tid, p));
                if (t->muted  != mu) macro->Add(std::make_unique<SetTrackMuteCommand>((TrackId)tid, mu));
                if (t->soloed != so) macro->Add(std::make_unique<SetTrackSoloCommand>((TrackId)tid, so));
                // The M3.4 fields are sent as their OWN fields, and only acted
                // on when the message actually carries them: a poster that
                // knows only gain/pan/mute/solo (an older editor, a test) must
                // not silently reset a trim it never had an opinion about.
                if (msg->HasFloat("trim") && t->trim != trim)
                    macro->Add(std::make_unique<SetTrackTrimCommand>((TrackId)tid, trim));
                if (msg->HasBool("phase") && t->phaseInvert != phase)
                    macro->Add(std::make_unique<SetTrackPhaseCommand>((TrackId)tid, phase));
                if (msg->HasInt32("sgroup") && t->soloGroup != (int)sgroup)
                    macro->Add(std::make_unique<SetTrackSoloGroupCommand>((TrackId)tid, (int)sgroup));
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
                // Refresh the other strips/panes so the change shows live. Both
                // group rules cascade in the model (SetTrackMuteCommand and
                // SetTrackSoloCommand both write every member), so BOTH have to
                // bring the refresh with them -- with only the mute-group test,
                // soloing one member of a solo group left the other members'
                // S lamps stale in an open mixer.
                if (t->muteGroup > 0 || t->soloGroup > 0)
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
            // A region drag or split posts ONLY this message, so it is also
            // where an open roll hears that its clip moved or changed shape
            // (package 51: the roll held the open-time start and length).
            RefreshRolls(true);
            break;
        case kMsgRollAudition:   // the MIDI editor's clicks (package 51)
            HandleRollAudition(msg);
            break;
        case MSG_AUDITION_IDLE:  // the clicks stopped: release the idle engine
            delete fAuditionIdle;
            fAuditionIdle = nullptr;
            if (fAuditionHeld > 0) {
                // A key is still down in the editor: stopping now would reset
                // the voice mid-note. Look again later.
                StartAuditionIdleTimer();
                break;
            }
            StopAuditionEngine();
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
        case kMsgShowFx: {
            // A view asking for the chain editor (the inspector's slot list):
            // the window owns the single effects window, so the view asks.
            int64 tid = 0; int32 fx = -1;
            msg->FindInt64("track", &tid);
            msg->FindInt32("focus", &fx);
            ShowFxWindow((TrackId)tid, fx);
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
            ShowFxWindow((TrackId)tid, fx);
            break;
        }
        case kMsgFxPresetPrompt: {
            // An insert panel asking for a preset name: the prompt for it is
            // opened here (windows belong to this thread) and answers with
            // kMsgSaveFxPreset below.
            int64 tid = 0, fx = 0;
            msg->FindInt64("track", &tid);
            msg->FindInt64("aux", &fx);
            BMessenger self(this);
            (new RenameWindow(BRect(120, 120, 420, 164), (TrackId)tid, "",
                              self, kMsgSaveFxPreset, fx, "Save Preset"))->Show();
            break;
        }
        case kMsgSaveFxPreset: {
            // An insert panel's "Save Preset..." prompt answered. The prompt
            // (RenameWindow) knows only the track and the insert index it was
            // opened for; the model and the live instance are here.
            int64 tid = 0, fx = -1;
            msg->FindInt64("track", &tid);
            msg->FindInt64("aux", &fx);   // RenameWindow echoes the insert index
            BString name;
            msg->FindString("name", &name);
            SaveFxPreset((TrackId)tid, (int)fx, name.String());
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
        case kMsgFxLiveMix: {   // live wet/dry drag preview (package 52)
            // The mix slider is the one row in that panel that used to wait for
            // the release: the engine holds each insert's wet/dry in its own
            // atomic rather than a numbered param slot, so the param preview
            // could not reach it. Same shape as kMsgFxLive, one field.
            int64 tid = 0; int32 fx = 0; float mix = 1.0f;
            msg->FindInt64("track", &tid);
            msg->FindInt32("fx", &fx);
            msg->FindFloat("mix", &mix);
            if (fTransportCtl.fEngine)
                fTransportCtl.fEngine->SetFxMixLive((TrackId)tid,
                    (TrackId)tid == kMasterFxTarget, fx, mix);
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
            // "Fit" means fit what is focused: with the docked MIDI editor
            // holding the focus, F fits the REGION (package 51) — the View menu's
            // shortcut is claimed by the menu bar before any view sees the key.
            if (PianoRollView* roll = FocusedRoll()) { roll->ZoomToFitRegion(); break; }
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
            BAlert* a = new ThemedAlertWindow("About Haiku DAW", text, "OK", nullptr,
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
                BAlert* a = new ThemedAlertWindow("Recover",
                    "Unsaved work from a previous session was found. Recover it?",
                    "Discard", "Recover");
                if (a->Go() == 1) LoadFrom(p.Path(), /*asRecovery*/ true);
                else              fDoc.RemoveRecoveryFile();
            }
            break;
        }
        case MSG_TITLE:
            UpdateTitle();   // cheap; SetTitle only fires on an actual change
            ExpireTransportStatus();   // a job message older than its hold goes
            // The same 2 Hz poll is the backstop for a roll edit the window
            // never hears about (the timeline commits straight from a gesture):
            // with the serial unchanged this is a comparison and nothing else
            // (package 51). RefreshRolls also carries the follow push, so a
            // roll opened before the menu item existed still agrees with it.
            RefreshRolls();
            break;
        case MSG_BROWSER:
            // The docked page is the browser's home now (T2); Pop out in the
            // dock's strip gives the window back.
            ShowDockBrowser(kDockSamples);
            break;
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
            // The decode runs on a worker thread now (M4.4): this only starts
            // the job. Applying its result — and refusing to apply a cancelled
            // one — happens in the pulse.
            int64 tid = 0, cid = 0;
            if (msg->FindInt64("track", &tid) != B_OK) break;
            msg->FindInt64("clip", &cid);
            const RenderJobs::RegionOp op =
                  msg->what == kMsgRegionNormalize ? RenderJobs::RegionOp::Normalize
                : msg->what == kMsgRegionReverse ? RenderJobs::RegionOp::Reverse
                                                 : RenderJobs::RegionOp::StripSilence;
            fRender.StartRegionJob(op, (TrackId)tid, (ClipId)cid);
            break;
        }
        case kMsgFreezeTrack: {
            int64 tid = 0; bool freeze = true;
            if (msg->FindInt64("track", &tid) != B_OK) break;
            msg->FindBool("freeze", &freeze);
            if (freeze) fRender.StartRegionJob(RenderJobs::RegionOp::Freeze,
                                               (TrackId)tid);
            else        fRender.FreezeTrack((TrackId)tid, false);   // a restore
            break;
        }
        case MSG_SHORTCUTS: {
            // One window, and it shows what the one key table says (M3.2): the
            // hand-written list that used to live here is gone, so the help
            // cannot describe a binding the behaviour does not have.
            if (fKeysMsgr.IsValid()) {
                fKeysMsgr.SendMessage(kMsgShortcutsActivate);
                break;
            }
            ShortcutsWindow* w = new ShortcutsWindow();
            fKeysMsgr = BMessenger(w);
            w->Show();
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
        case MSG_EDIT_UNDO:
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
                UpdateEditMenu();
                UpdateTitle();
                // ... and the MIDI editor: the undo rewrote the region its
                // snapshot shows (package 51 — this was the stale-list bug).
                RefreshRolls(true);
            }
            break;
        case MSG_EDIT_REDO:
            if (fStack->CanRedo()) {
                fStack->Redo(*fProject);
                ValidateFxWatch();
                fTransportCtl.ReloadActiveEngine();   // brings the engine to the model's state
                PublishFxParamsNow();   // ... and only then republish
                PushChainToFxWindow(fFxTrack);
                fTimeline->Invalidate();
                UpdateEditMenu();
                UpdateTitle();
                RefreshRolls(true);
            }
            break;
        // The Edit menu (M2.4): the timeline owns the range and the clipboard.
        // With the DOCKED piano roll focused they belong to the roll instead
        // (package 51): the roll is a view of this window, so the menu bar's
        // shortcuts get first refusal, and without this a Cmd-C in the editor
        // silently copied the arrangement's clips. (A popped-out roll is its
        // own window with no menu bar: its KeyDown handles them itself.)
        case MSG_EDIT_PASTE:
            if (PianoRollView* roll = FocusedRoll()) { roll->PasteClipboard(); break; }
            fTimeline->PasteAtPlayhead();
            UpdateEditMenu();
            break;
        case MSG_EDIT_CUT:
            if (PianoRollView* roll = FocusedRoll()) {
                roll->CopySelection();
                roll->DeleteSelection();
                break;
            }
            fTimeline->RangeCut();
            UpdateEditMenu();
            break;
        case MSG_EDIT_COPY:
            if (PianoRollView* roll = FocusedRoll()) { roll->CopySelection(); break; }
            fTimeline->RangeCopy();
            UpdateEditMenu();
            break;
        // (Edit > Delete carries no shortcut — Del is the view's own key — so
        // with the docked roll focused the key already reaches the roll and
        // this handler stays the timeline's.)
        case MSG_EDIT_DELETE:
            fTimeline->RangeDelete();
            UpdateEditMenu();
            break;
        case MSG_EDIT_SELECT_ALL:
            if (PianoRollView* roll = FocusedRoll()) { roll->SelectAll(); break; }
            fTimeline->SelectAllClips();
            UpdateEditMenu();
            break;
        case MSG_EDIT_DUPLICATE:
            if (PianoRollView* roll = FocusedRoll()) { roll->DuplicateSelection(); break; }
            fTimeline->DuplicateSelection();
            UpdateEditMenu();
            break;
        case MSG_EDIT_INSERT_SIL:
            fTimeline->RangeInsertSilence();
            UpdateEditMenu();
            break;
        case MSG_BUFFER: {
            int32 frames = 512;
            msg->FindInt32("frames", &frames);
            // Buffer size is a preference (M3.5) and lives in the window; the
            // id still works and applies at the next Play, as it always did.
            PrefsView v = CurrentPrefs();
            v.bufferFrames = frames;
            ApplyPrefs(v);
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
            UpdateTransportInfo();     // the readout line names the tempo too
            // Rebuild the running engine so the metronome + tempo-synced effects
            // (delay) pick up the new tempo live; stopped, it applies next Play.
            fTransportCtl.ReloadActiveEngine();
            break;
        }
        case MSG_NEW_PROJECT:          // File > New: the dialog (M3.7)
            ShowNewProjectWindow();
            break;
        case kMsgNewProjectChosen: {
            const char* id = nullptr;
            const char* path = nullptr;
            msg->FindString("id", &id);
            msg->FindString("path", &path);
            NewProjectFrom(id != nullptr ? id : "empty",
                           path != nullptr ? path : "");
            break;
        }
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
                // The default project folder (M3.5): where a first save
                // starts. After the first save the panel remembers the folder
                // the user actually used, which is the better answer.
                if (!fPrefsProjectDir.empty())
                    fSavePanel->SetPanelDirectory(fPrefsProjectDir.c_str());
            }
            ShowPanel(fSavePanel, "Save Project", ProjectDocument::DisplayName(fDoc.Path()) + ".dawproj");
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
            ShowPanel(fOpenPanel, "Open Project");
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
            ShowPanel(fImportPanel, "Import Audio");
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
            // Is a Stop's meter decay still running? (package 52)
            const bool decaying = system_time() < fMeterDecayUntil;
            if (fMeterDecayUntil != 0 && !decaying) {
                // The window has expired: clear it and re-evaluate, or the
                // runner that the decay kept alive is never asked again and
                // the 60 Hz tick spins for the rest of the session.
                fMeterDecayUntil = 0;
                UpdatePulse();
            }
            // The transport bar's own readouts first (M3.1): the lamps, the
            // status line and the tempo/meter/DSP/xrun line, all change-guarded
            // inside the bar, so a tick that changed nothing costs no repaint.
            UpdateTransportReadouts();
            // A rebuild runs on the engine's worker thread now (M4.1): its
            // outcome -- failure included -- is picked up here, where the UI may
            // be touched, instead of blocking the window on the load.
            fTransportCtl.PollEngineLoad();
            // Waveform envelopes arrive the same way (M4.4): the loader keeps its
            // own copy and hands them over here, under this window's lock — the
            // same lock the timeline draws the map under, so nothing in the
            // drawing code needed to learn about the worker.
            {
                const size_t got = fPeakLoader.Drain(*fPeaks);
                if (got > 0 && fTimeline) fTimeline->Invalidate();
            }
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
            // A region job reports through the same pulse and the same bar
            // (M4.4). Its model edits land in FinishRegionJob, on this thread.
            if (!fRender.RegionJobHandled()) {
                if (fRender.RegionJobRunning()) {
                    const float f =
                        fRender.fRegionProgress.load(std::memory_order_relaxed);
                    if (f >= 0.0f && fRender.fRegionProgMsgr.IsValid()) {
                        BMessage pm(kMsgExportProgress);
                        pm.AddFloat("f", f);
                        fRender.fRegionProgMsgr.SendMessage(&pm);
                    }
                } else {
                    fRender.FinishRegionJob();
                }
            }
            // Before any branch: an open native editor is fed on every pulse,
            // including while stopped (see PushFxParams).
            PushFxParams();
            // The meter decay window (package 52): the transport is stopped but
            // the pulse is still running, so the bars stay at zero and the peak
            // holds FALL instead of freezing at the last playing value. One
            // clear per tick is what makes the hold a moving thing.
            if (decaying && !fTransportCtl.fPlaying && !fRecCtl.fRecMode
                && !fTransportCtl.fMonitoring) {
                ClearMeters();
                break;
            }
            if (fTransportCtl.fEngine && fTransportCtl.fMonitoring && !fTransportCtl.fPlaying && !fRecCtl.fRecMode) {
                // Idle live-monitoring: apply live gain/pan/mute edits, then
                // drive the meters (no playhead / transport).
                fTransportCtl.fEngine->UpdateMix(*fProject);
                if (fTransportCtl.fEngine->FxLatencyMoved())
                    fTransportCtl.ReloadActiveEngine();   // M4.6, see below
                PushMeter(false);
                PushTrackPeaks();
                break;
            }
            if (fTransportCtl.fEngine && (fTransportCtl.fPlaying || fRecCtl.fRecMode)) {                fTransportCtl.fEngine->UpdateMix(*fProject);   // live gain/pan/mute/solo
                // M4.6: an insert whose plugin moved its own latency port (a
                // 4K EQ 2 toggling oversampling) needs PDC re-solved, because
                // the delay lines that align it were sized from the value the
                // graph was built with. The poll runs here -- off the RT thread
                // -- and a true answer rebuilds IN PLACE: the graph keeps
                // playing while the builder re-settles every effect and solves
                // the new delays, and the swap lands at a block boundary. The
                // pulse is the right place because it is the only periodic
                // caller, and it already touches the engine's atomics.
                if (fTransportCtl.fEngine->FxLatencyMoved())
                    fTransportCtl.ReloadActiveEngine();
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
                    //
                    // The record engine is built WITHOUT a loop on purpose
                    // (TransportController::StartRecordEngine), so this seam is
                    // still the window's, exactly as it was before M4.2: the
                    // take layout is measured from the moment the engine rolls
                    // (RecordPlan), and re-rolling per pass is what keeps the
                    // passes aligned with the capture. Playback loops inside the
                    // engine; recording still re-aligns here.
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
                            fTimeline->SetLiveAudio(fRecCtl.fRecorder.get(),
                                                    fProject->sampleRate,
                                                    fRecCtl.fLiveTakeFor);
                        if (midiCap) {
                            std::map<TrackId, std::vector<MidiNote>> live;
                            for (TrackId id : fRecCtl.fMidiRecTracks)
                                live[id] = fRecCtl.fMidiRecs[id].SnapshotNotes(ph);
                            fTimeline->SetLiveMidiNotes(std::move(live));
                        }
                    }
                    PushMeter(capturing);
                    PushTrackPeaks();
                    PushFxMeters();
                    break;
                }

                // The playback loop is the ENGINE's (M4.2): the graph walks the
                // range it was built with, splitting the callback at the loop
                // end so the jump is sample-exact, while the disk threads
                // produce the loop start's audio in playback order behind it. No
                // rebuild, no device gap — which is what this branch used to do
                // (StartPlayback per pass, a whole graph each time). All the
                // window has to do is show where the transport is, including
                // when it has just wrapped.
                fTimeline->SetPlayhead(ph);
                PushRollPlayhead(ph);
                UpdateTimeReadout(ph);
                PushMeter(false);
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
    float W = fTransport->Bounds().right + 1.0f;
    // The bar can be laid out wider than the window: the pane layout's own
    // minimum is the sum of its children's current sizes (a plain BView
    // reports exactly that), so fPaneSplit can demand more width than there is
    // and the group hands it to every child. Everything this method pins to
    // the bar's right edge would then sit outside the window -- the master
    // meter was invisible for exactly that reason (M3.3). Clamp to the window.
    if (W > Bounds().Width()) W = Bounds().Width();

    // Two rows (M3.1): the transport row A, the status row B. The bar draws
    // the buttons, the LCD well and the status text; everything here is a
    // child widget, and everything is placed FROM the right so nothing can
    // overlap. The design offsets are the defaults: meter 6 from the right
    // edge and 124 wide, the cluster may not cross x 602 (that is where the
    // design put it, and where the LCD's own right edge leaves the gap).
    const float kGap     = Themed(12.0f);
    const float kMeterW  = Themed(124.0f);
    const float kLoudW   = Themed(136.0f);
    const float kFieldW  = Themed(66.0f);    // the tempo field
    const float kBpmLblW = Themed(34.0f);
    const float kVolLblW = Themed(30.0f);
    const float kSliderW = Themed(126.0f);   // the master volume
    const float kClusterL = Themed(602.0f);  // the right cluster's own left edge

    if (fMeter) {
        // Placed AND sized: the meter is built before the theme scale is known
        // on a HiDPI system (the tests override the scale after the window
        // exists), so a pin that only moved it would leave it at its design
        // size in a 150% bar. It spans both rows.
        fMeter->MoveTo(W - Themed(6.0f) - kMeterW, Themed(4.0f));
        fMeter->ResizeTo(kMeterW, Themed(kTransportH) - Themed(8.0f));
    }

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

    // Row A, right to left: the tempo field (with its label), then the
    // loudness readout. Each is placed only when what is left of it still
    // clears the LCD's own right edge -- a readout that does not fit is
    // dropped rather than drawn over the position.
    const float kLcdR = Themed(590.0f);
    float aRight = rightEdge;
    const bool showBpm = aRight - kFieldW - kBpmLblW >= kClusterL;
    if (showBpm) {
        fTempo->MoveTo(aRight - kFieldW, Themed(9.0f));
        fTempo->ResizeTo(kFieldW, Themed(20.0f));
        fBpmLbl->MoveTo(aRight - kFieldW - kBpmLblW, Themed(10.0f));
    }
    setVisible(fBpmShown, showBpm, { fBpmLbl, fTempo });
    aRight -= showBpm ? (kFieldW + kBpmLblW + kGap) : 0.0f;

    const bool showLoud = showBpm && aRight - kLoudW >= kClusterL
                       && (W - Themed(6.0f) - kMeterW) >= kLcdR + kGap;
    if (fLoudView && showLoud) {
        fLoudView->MoveTo(aRight - kLoudW, Themed(10.0f));
        fLoudView->ResizeTo(kLoudW, Themed(18.0f));
    }
    setVisible(fLoudShown, showLoud, { fLoudView });

    // Row B, right to left: the master volume. The mixer has the same fader,
    // which is what makes it droppable on a narrow window.
    const bool showVol = rightEdge - kSliderW - kVolLblW >= kClusterL;
    if (showVol) {
        fMaster->MoveTo(rightEdge - kSliderW, Themed(33.0f));
        fMaster->ResizeTo(kSliderW, Themed(20.0f));
        fVolLbl->MoveTo(rightEdge - kSliderW - kVolLblW, Themed(35.0f));
    }
    setVisible(fVolShown, showVol, { fVolLbl, fMaster });

    // The LCD needs a legible width, and the meter must not cover it: below
    // that the whole display goes rather than a squeezed fragment of it (the
    // transport buttons and the status line keep working).
    const bool showLcd = (W - Themed(6.0f) - kMeterW) >= kLcdR + kGap;
    fTransport->SetLcdVisible(showLcd);
}

// Show or hide the inspector pane (View > Inspector, key I). Collapsing the
// split item is what actually reclaims the width; the view is hidden too, so
// its 60 Hz meter updates stop painting into a collapsed column.
void MainWindow::SetInspectorShown(bool shown) {
    fInspectorShown = shown;
    // The item LEAVES the split when hidden: a collapsed BSplitView item keeps
    // its preferred size in this Haiku, which left an empty strip where the
    // inspector used to be. Removing it hands the width back to the timeline,
    // and re-adding puts it back where it was.
    if (fPaneSplit != nullptr && fInspector != nullptr) {
        if (shown && fInspector->Parent() == nullptr)
            fPaneSplit->AddChild(0, fInspector, 0.0f);
        else if (!shown && fInspector->Parent() != nullptr)
            fInspector->RemoveSelf();
    }
    if (fInspectorItem != nullptr) fInspectorItem->SetMarked(shown);
    if (fTimeline != nullptr) fTimeline->Invalidate();
}

// Show or hide the docked bottom pane, the same way and for the same reason.
void MainWindow::SetDockShown(bool shown) {
    if (fRootSplit == nullptr || fDock == nullptr) return;
    if (shown && fDock->Parent() == nullptr) {
        fRootSplit->AddChild(fDock, 0.0f);
        // Not collapsible: a collapsed item keeps its preferred size here (see
        // above), and dragging the pane away is what J and the x are for.
        fRootSplit->SetCollapsible(fRootSplit->CountChildren() - 1, false);
    } else if (!shown && fDock->Parent() != nullptr) {
        fDock->RemoveSelf();
    }
    if (fDockItem != nullptr) fDockItem->SetMarked(shown);
}

// Show the effects window on a track's chain (T2, single instance).
//
// The window survives being pointed at a different chain: it is told the new
// chain, the track and the focus slot, shows itself and activates. Creating a
// second one is what happened before -- four call sites, four windows, and no
// way to tell which chain was in which.
void MainWindow::ShowFxWindow(TrackId track, int focusSlot) {
    const std::vector<EffectDesc>* chain = nullptr;
    std::string trackName;
    if (track == kMasterFxTarget) {
        chain = &fProject->masterFx;
        trackName = "Master";
    } else if (Track* t = fProject->FindTrack(track)) {
        chain = &t->fx;
        trackName = t->name;
    } else {
        return;
    }

    // The sidechain picker's candidates for THIS track (the same list the
    // window opened with): a retarget changes which source would be a self-key,
    // so both paths carry the fresh snapshot.
    const std::vector<std::pair<TrackId, std::string>> keySources =
        SidechainCandidates(*fProject, track);

    const std::string title = EffectsWindow::WindowTitleFor(*chain, focusSlot,
                                                            trackName.c_str());
    if (fFxMsgr.IsValid()) {
        BMessage retarget(kMsgFxRetarget);
        retarget.AddInt64("track", (int64)track);
        retarget.AddInt32("focus", focusSlot);
        retarget.AddString("title", title.c_str());
        for (const auto& ks : keySources) {
            retarget.AddInt64("keysrc", (int64)ks.first);
            retarget.AddString("keysrcname", ks.second.c_str());
        }
        EncodeFxChain(retarget, *chain);
        // A window that closed between the check and here answers with an
        // error, and the fall-through opens a fresh one.
        if (fFxMsgr.SendMessage(&retarget) == B_OK)
            return;
        fFxMsgr = BMessenger();
        fFxTrack = kInvalidTrackId;
    }

    BRect wr(200, 150, 680, 770);
    (new EffectsWindow(wr, *chain, track, BMessenger(this), focusSlot,
                       trackName.c_str(), keySources))->Show();
}

// --- the sends and instrument windows (package 52) -------------------------
//
// One window per editor, carrying the track it is pointed at — the shape the
// effects window has had since T2, and for the same reason: each of these
// windows holds a WHOLE-SNAPSHOT copy of the list it edits and commits that
// copy back, so two windows on one track silently overwrite each other's edits
// (the last one to touch a control won, and the intervening edit was gone).
//
// So: a second ask for the SAME track activates the window; an ask for another
// track RETARGETS it (fresh snapshot, title naming the track); and a track that
// is gone closes it (CloseEditorsForGoneTracks).

void MainWindow::ShowSendsWindow(TrackId tid) {
    Track* t = fProject->FindTrack(tid);
    if (!t) return;
    std::vector<std::pair<TrackId, std::string>> buses;
    for (const Track& bt : fProject->Tracks())
        if (bt.type == TrackType::Bus && bt.id != tid)
            buses.push_back({bt.id, bt.name});

    if (fSendsMsgr.IsValid()) {
        // The same track: push the CURRENT list, not just an activate. The
        // window holds a snapshot and commits its whole list on any edit, and
        // the model can have moved under it since (an undo, the inspector, a
        // mixer knob) -- "no new state needed" was a stale-list bug waiting
        // for its second editor. Retarget() shows and activates the window,
        // so the push is also what brings it forward.
        BMessage r(kMsgSendsRetarget);
        r.AddInt64("track", (int64)tid);
        r.AddString("name", t->name.c_str());
        for (const Send& s : t->sends) {
            r.AddInt64("sd", (int64)s.dest);
            r.AddFloat("sl", s.level);
            r.AddInt32("sp", s.preFader ? 1 : 0);
        }
        for (const auto& b : buses) {
            r.AddInt64("bid", (int64)b.first);
            r.AddString("bname", b.second.c_str());
        }
        if (fSendsMsgr.SendMessage(&r) == B_OK) {
            fSendsTrack = tid;
            return;
        }
        fSendsMsgr = BMessenger();   // its messenger is dead: make a new one
        fSendsTrack = kInvalidTrackId;
    }

    SendsWindow* w = new SendsWindow(BRect(200, 150, 540, 470), t->sends, buses,
                                     tid, BMessenger(this), t->name.c_str());
    fSendsMsgr = BMessenger(w);
    fSendsTrack = tid;
    fSendsScope = fScope.Current();
    w->Show();
}

void MainWindow::ShowInstrumentWindow(TrackId tid) {
    Track* t = fProject->FindTrack(tid);
    if (!t) return;
    // A MIDI track is the only thing with a voice to edit: the instrument slot
    // belongs to the MIDI path (the inspector only offers its button there, and
    // the mixer's strip only draws it for a MIDI track). Anything else is a
    // message from a stale snapshot.
    if (t->type != TrackType::Midi) return;
    if (fInstMsgr.IsValid()) {
        // Same rule as the sends window above: push the current descriptor
        // rather than only activating, because the window commits its whole
        // descriptor and the model may have moved since it was opened.
        BMessage r(kMsgInstRetarget);
        r.AddInt64("track", (int64)tid);
        r.AddString("name", t->name.c_str());
        // The descriptor travels in kMsgApplyInstrument's own field layout: one
        // encoder shape for the two directions, so they cannot drift.
        r.AddInt32("type", (int32)t->instrument.type);
        r.AddInt32("wave", t->instrument.synth.waveform);
        r.AddFloat("a", t->instrument.synth.attack);
        r.AddFloat("d", t->instrument.synth.decay);
        r.AddFloat("s", t->instrument.synth.sustain);
        r.AddFloat("r", t->instrument.synth.release);
        r.AddString("path", t->instrument.path.c_str());
        r.AddInt32("preset", t->instrument.sf2Preset);
        if (fInstMsgr.SendMessage(&r) == B_OK) {
            fInstTrack = tid;
            return;
        }
        fInstMsgr = BMessenger();
        fInstTrack = kInvalidTrackId;
    }
    InstrumentWindow* w = new InstrumentWindow(BRect(200, 150, 480, 340),
                                              t->instrument, tid,
                                              BMessenger(this), t->name.c_str());
    fInstMsgr = BMessenger(w);
    fInstTrack = tid;
    fInstScope = fScope.Current();
    w->Show();
}

// An editor whose TRACK is gone cannot stay up: it holds a whole-snapshot copy
// addressed by that id, and its next commit would be dropped by the model
// guard at best. Runs from the kMsgUiRefresh funnel, which every track-list
// edit posts (item B), so a delete is noticed wherever it was made.
void MainWindow::CloseEditorsForGoneTracks() {
    if (fSendsMsgr.IsValid() && fSendsTrack != kInvalidTrackId
        && !fProject->FindTrack(fSendsTrack)) {
        fSendsMsgr.SendMessage(B_QUIT_REQUESTED);
        fSendsMsgr = BMessenger();
        fSendsTrack = kInvalidTrackId;
    }
    if (fInstMsgr.IsValid() && fInstTrack != kInvalidTrackId
        && !fProject->FindTrack(fInstTrack)) {
        fInstMsgr.SendMessage(B_QUIT_REQUESTED);
        fInstMsgr = BMessenger();
        fInstTrack = kInvalidTrackId;
    }
    // The effects window is the same shape (a whole chain committed whole), and
    // a chain addressed by a dead track id is exactly the stale address this
    // package is about.
    if (fFxMsgr.IsValid() && fFxTrack != kInvalidTrackId
        && fFxTrack != kMasterFxTarget && !fProject->FindTrack(fFxTrack)) {
        fFxMsgr.SendMessage(B_QUIT_REQUESTED);
        fFxMsgr = BMessenger();
        fFxTrack = kInvalidTrackId;
        if (fTransportCtl.fEngine) fTransportCtl.fEngine->SetMeterFocus(kInvalidTrackId);
    }
}

// The inspector's track, and with it the chain the dock's plugin page adds to// (T2). Both follow the same selection through this one call: the two used to
// be updated separately, so the mixer's strip click moved the inspector but
// left the page on the old track, and a new project left it on a track id that
// belonged to the old one (T2 review).
void MainWindow::SelectTrack(TrackId tid) {
    if (fInspector) fInspector->SetTrack(tid);
    fSelTrack = tid;
    // Re-point (and re-name) the plugin page through the same code the page
    // switch uses, so the two cannot disagree. Deliberately not a SHOW: the
    // page follows the selection even while the dock is hidden -- switching to
    // the page would be re-opening a dock the user just closed (T2 review).
    if (fDockPage == kDockPlugins && fDockPlugins != nullptr)
        SetDockPage(kDockPlugins);
}

// Show one of the dock's pages (T2). The body holds exactly one view: leaving
// the old one in place would keep its size (the same lesson as the split
// items), and a page that is not showing must not be metering or querying.
//
// Only the BODY: the dock itself is not shown here. A page can be (re)pointed
// while the dock is hidden, and a call that also showed the dock would reopen
// one the user closed every time the track selection moved (T2 review). The
// callers that mean "show me this page" say so themselves.
void MainWindow::SetDockPage(int page) {
    if (page < kDockEditor || page > kDockPlugins) return;
    fDockPage = page;

    // Every page leaves the body first; each branch below adds its own back.
    if (fDockRoll != nullptr)    fDockRoll->RemoveSelf();
    if (fDockEmpty != nullptr)   fDockEmpty->RemoveSelf();
    if (fDockSamples != nullptr) fDockSamples->RemoveSelf();
    if (fDockPlugins != nullptr) fDockPlugins->RemoveSelf();

    switch (page) {
        case kDockSamples:
            if (fDockSamples == nullptr)
                fDockSamples = new SampleBrowserView(BMessenger(this));
            fEditorPane->GroupLayout()->AddView(fDockSamples, 1.0f);
            fDockTitle->SetText("Samples");
            fDockPop->SetEnabled(true);     // into its own window
            break;
        case kDockPlugins:
            if (fDockPlugins == nullptr)
                fDockPlugins = new PluginBrowserView(fSelTrack,
                                                     BMessenger(fInspector),
                                                     BMessenger(this));
            else
                fDockPlugins->SetTrack(fSelTrack);
            fEditorPane->GroupLayout()->AddView(fDockPlugins, 1.0f);
            // Name the chain a pick will land in: the page adds to whichever
            // track the inspector is pointed at, and that is not otherwise
            // visible from the dock. With NO track selected the page cannot add
            // anything -- say what it needs instead of "add to no track
            // selected", which read as an error about a chain nobody named.
            {
                Track* t = fProject->FindTrack(fSelTrack);
                BString title("Plugins \xE2\x80\x94 ");
                if (t != nullptr) {
                    title << "add to ";
                    title << t->name.c_str();
                } else {
                    title << "select a track to add to";
                }
                fDockTitle->SetText(title.String());
            }
            fDockPop->SetEnabled(true);
            break;
        default:
            if (fDockRoll != nullptr) {
                fEditorPane->GroupLayout()->AddView(fDockRoll, 1.0f);
                fDockPop->SetEnabled(true);
            } else {
                fEditorPane->GroupLayout()->AddView(fDockEmpty, 1.0f);
                fDockPop->SetEnabled(false);
            }
            // Restore the title the editor page had before the switch.
            if (fDockRoll != nullptr) {
                Track* t = fProject->FindTrack(fDockTrack);
                BString title("Piano Roll \xE2\x80\x94 ");
                title << (t != nullptr ? t->name.c_str() : "region");
                fDockTitle->SetText(title.String());
            } else {
                fDockTitle->SetText("Editor");
            }
            break;
    }
    if (fDockSegments != nullptr) fDockSegments->SetSelected(page);
}

// A browser page, asked for by name (View > Sample Browser, the mixer strip's
// Add Effect): show the dock on it, pointed at the chain the caller meant.
// Which chain is the page's business: SetDockPage creates the plugin view with
// fSelTrack and re-points one that exists, so naming it here as well would be
// the same retarget twice -- three places setting the page and the track is
// how they drift (T2 review).
void MainWindow::ShowDockBrowser(int page, TrackId track) {
    if (page == kDockPlugins && track != kInvalidTrackId) fSelTrack = track;
    SetDockPage(page);
    SetDockShown(true);
}

// Open the MIDI editor in the docked bottom pane (M1.4). The pane is a plain
// group: replacing its second item is all "open another region" takes, and the
// old view goes away with it.
void MainWindow::OpenDockedEditor(TrackId track, ClipId clip) {
    Track* t = fProject->FindTrack(track);
    if (t == nullptr) return;
    const MidiClip* c = t->FindMidiClip(clip);
    if (c == nullptr) return;

    ClearDockedEditor();   // the previous roll goes
    // The roll fills the pane below the header row; the split gives it its
    // real size, so the frame here is only a hint.
    fDockRoll = new PianoRollView(BRect(0, 0, 600, Themed(200.0f)), track, clip,
                                  c->startFrame, c->lengthFrames, c->notes,
                                  c->events, fProject->tempoMap,
                                  fProject->sampleRate,
                                  fProject->transport.playhead,
                                  BMessenger(this));
    fDockTrack = track;
    fDockClip  = clip;
    // Through the page switch, which takes the browsed page out of the body
    // first: adding the roll here while Samples or Plugins was showing left
    // TWO bodies (they split the pane) with the strip still naming the browser
    // (T2 review).
    SetDockPage(kDockEditor);
    SetDockShown(true);
    fDockRoll->MakeFocus(true);
    PushRollPlayhead(fProject->transport.playhead);
    // The new view's snapshot starts from the model, but it still needs the
    // stack serial it is based on, and the follow policy (package 51).
    RefreshRolls(true);
    PushRollFollow();
}

// Pop out: the page that is showing becomes its own window, and the dock goes
// back to the editor page (its content stays alive, so switching back is free).
void MainWindow::PopOutDockPage() {
    if (fDockPage == kDockSamples) {
        BRect wr = BWindow::Frame();
        wr.OffsetBy(40, 40);
        wr.right = wr.left + 420; wr.bottom = wr.top + 380;
        (new SampleBrowser(wr, BMessenger(this)))->Show();
        SetDockPage(kDockEditor);
        SetDockShown(false);
        return;
    }
    if (fDockPage == kDockPlugins) {
        BRect wr = BWindow::Frame();
        wr.OffsetBy(60, 60);
        wr.right = wr.left + 460; wr.bottom = wr.top + 400;
        // The window's choice still lands in the inspector, exactly as the
        // docked page's does: one handler for "turn a pick into a command".
        (new PluginBrowser(wr, fSelTrack, BMessenger(fInspector),
                           BMessenger(this)))->Show();
        SetDockPage(kDockEditor);
        SetDockShown(false);
        return;
    }
    PopOutEditor();
}

// The docked editor becomes its own window. The window is the same PianoRoll
// the timeline used to open directly, so nothing about the editing paths
// changes; the dock just stops hosting it.
void MainWindow::PopOutEditor() {
    Track* t = fProject->FindTrack(fDockTrack);
    if (t == nullptr) return;
    const MidiClip* c = t->FindMidiClip(fDockClip);
    if (c == nullptr) return;
    BPoint sp = ConvertToScreen(BPoint(InspectorWidth() + Themed(40.0f),
                                       Themed(80.0f)));
    // The window opens at the theme's scale (item J): a 150 % session gets a
    // 150 % editor rather than a fixed 720x480.
    const float w = Themed(720.0f), hgt = Themed(480.0f);
    BRect wr(sp.x, sp.y, sp.x + w, sp.y + hgt);
    PianoRoll* roll = new PianoRoll(wr, fDockTrack, fDockClip, c->startFrame,
                                    c->lengthFrames, c->notes, c->events,
                                    fProject->tempoMap, fProject->sampleRate,
                                    fProject->transport.playhead,
                                    BMessenger(this), t->name.c_str());
    roll->Show();
    // The window pushes the playhead through a messenger; the docked view was
    // pushed to directly. Register it the same way the timeline used to, and
    // name the region it opened on so the window's state push (package 51) can
    // address it.
    BMessage m(kMsgRollOpened);
    m.AddMessenger("m", BMessenger(roll));
    m.AddInt64("track", (int64)fDockTrack);
    m.AddInt64("clip",  (int64)fDockClip);
    PostMessage(&m);

    ClearDockedEditor();
    SetDockShown(false);
}

// Empty the dock's body: the roll goes, the hint comes back, and the strip
// stops naming a region (Pop out has nothing to act on until the next one).
void MainWindow::ClearDockedEditor() {
    if (fDockRoll != nullptr) {
        // A key held in the editor goes with it: its note-off would never come,
        // so the view sends it before it is deleted (the counter decrements
        // through the same path a real mouse-up takes -- zeroing it here would
        // under-count a key the POPPED-OUT editor is still holding).
        fDockRoll->AuditionEnd();
        fDockRoll->RemoveSelf();
        delete fDockRoll;
        fDockRoll = nullptr;
    }
    fDockTrack = kInvalidTrackId;
    fDockClip  = kInvalidClipId;
    // The hint belongs to the editor page: emptying the roll while the SAMPLES
    // page is showing must not put the hint on top of it.
    if (fDockPage == kDockEditor) {
        if (fDockSamples != nullptr) fDockSamples->RemoveSelf();
        if (fDockPlugins != nullptr) fDockPlugins->RemoveSelf();
        if (fDockEmpty->Parent() == nullptr)
            fEditorPane->GroupLayout()->AddView(fDockEmpty, 1.0f);
        fDockTitle->SetText("Editor");
        fDockPop->SetEnabled(false);
    }
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

// Bars to zero: the transport stopped, so there is no signal and nothing is
// coming. The clip LAMPS are deliberately kept (a lamp that cleared itself when
// the transport stopped would hide the clip that just happened); the peak HOLDS
// fall on their own while the decay window below keeps the pulse alive.
void MainWindow::ClearMeters() {
    if (fMeter) fMeter->SetReadings(MeterReadings{});
    if (fTimeline) fTimeline->ClearTrackPeaks();
    if (fInspector) fInspector->SetMeter(0.0f, 0.0f);
    PushSilenceToMixer();
    UpdateLoudnessReadout(Loudness::kSilenceLufs, Loudness::kSilenceLufs,
                          Loudness::kSilenceDb);
}

void MainWindow::UpdatePulse() {
    // A native editor watching an insert keeps the pulse alive even when the
    // transport is stopped: the generic parameter panel can move a value with
    // everything idle, and that has to reach the plugin's own editor too.
    bool watching = false;
    for (const FxEntry& w : fFxWatches)
        if (w.editor && w.editor->Alive()) { watching = true; break; }
    // ... and so does the meter decay window: a peak hold is a moving thing
    // (PeakHold falls at a fixed dB per second), so freezing the tick at Stop
    // leaves the mains't meter showing a level that never comes down.
    const bool decaying = system_time() < fMeterDecayUntil;
    const bool need = fTransportCtl.fPlaying || fRecCtl.fRecMode || fTransportCtl.fMonitoring || watching
                   || decaying
                   || (fRecCtl.fRecorder && fRecCtl.fRecorder->IsRecording())
                   || !fRender.fExportHandled      // an export's progress + completion
                   || !fRender.RegionJobHandled()  // ... and a region job's (M4.4)
                   || fPeakLoader.HasWork();       // envelopes still arriving (M4.4)
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
// Hand an open effects panel the chain as the MODEL now has it.//
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
    // The running engine first: a plugin's own state lives in the instance the
    // audio thread is driving, and this is the one point every save, render and
    // autosave passes through. It goes in BEFORE the editors below, which
    // overwrite it for the inserts they are actually showing — the editor is
    // where the user's patch edits happen, so it wins where both exist.
    if (fTransportCtl.fEngine) fTransportCtl.fEngine->CaptureFxStates(*fProject);

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

        // The editor's own state, if it has one. Deliberately NOT a command:
        // this is the same kind of derived session data as the playhead — it is
        // the plugin's patch, captured from the object that holds it, and it has
        // no visible representation for a user to undo. Applied only to the
        // insert the editor is still showing: the URI check is the same one
        // kMsgFxParamCommit uses, and for the same reason (the commit is
        // asynchronous, so the insert at that index may be another plugin by
        // now — or another plugin may have been dropped in after it).
        bool tookState = false;
        BString state;
        if (reply.FindString("state", &state) == B_OK && state.Length() > 0) {
            const bool master = (w.track == kMasterFxTarget);
            Track* t = master ? nullptr : fProject->FindTrack(w.track);
            std::vector<EffectDesc>* chain =
                master ? &fProject->masterFx : (t ? &t->fx : nullptr);
            if (chain && w.fx >= 0 && w.fx < (int32)chain->size() &&
                (*chain)[(size_t)w.fx].pluginName == w.uri) {
                (*chain)[(size_t)w.fx].state = state.String();
                tookState = true;
            }
        }

        std::vector<SetFxParamCommand::SlotValue> vals;
        for (int32 k = 0; ; k++) {
            int32 slot = 0;
            float v = 0.0f;
            if (reply.FindInt32("slot", k, &slot) != B_OK) break;
            if (reply.FindFloat("val", k, &v) != B_OK) break;
            vals.push_back({ slot, v });
        }
        const bool master = (w.track == kMasterFxTarget);
        const bool applied = !vals.empty() &&
            fStack->Execute(std::make_unique<SetFxParamCommand>(
                w.track, master, w.fx, std::move(vals)), *fProject);
        // A flush happens before a save/render; the panel's copy is stale from
        // this moment on (same rule as the commit path) — for the captured state
        // as much as for the values: left alone, its next knob move would commit
        // the chain from before this capture and drop the patch again.
        if (applied || tookState) PushChainToFxWindow(w.track);
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

// The panel's "Save Preset...": the insert's current patch, named and written
// to the preset store. The patch is captured first (the same flush a save
// does), because it lives in the live instance — a preset saved from the model
// alone would be the last project save's patch, not the one playing.
void MainWindow::SaveFxPreset(TrackId tid, int fx, const char* name) {
#ifdef DAW_HAVE_LV2
    if (!name || !name[0]) return;
    const bool master = (tid == kMasterFxTarget);
    if (!master && !fProject->FindTrack(tid)) return;

    // Pull the state into the model the way SaveTo does. This rewrites the
    // chain vector the pointers below would come from, so they are taken after.
    FlushFxEditors();

    Track* t = master ? nullptr : fProject->FindTrack(tid);
    std::vector<EffectDesc>* chain =
        master ? &fProject->masterFx : (t ? &t->fx : nullptr);
    if (!chain || fx < 0 || fx >= (int32)chain->size()) return;
    const EffectDesc& d = (*chain)[(size_t)fx];
    if (d.type != EffectType::Lv2 || d.pluginName.empty()) {
        ReportError("Save Preset",
                    "Presets are only supported for LV2 plugins.");
        return;
    }
    if (!Lv2Host::Instance().SavePreset(d.pluginName, name, d.state, d.params))
        ReportError("Save Preset",
                    std::string("Could not write the preset \"") + name +
                    "\".\n\nCheck that the settings folder is writable.");
#else
    (void)tid; (void)fx; (void)name;
#endif
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
    BAlert* a = new ThemedAlertWindow("Soundfonts", text.c_str(), "OK", nullptr, nullptr,
                           B_WIDTH_AS_USUAL, B_WARNING_ALERT);
    a->SetShortcut(0, B_ESCAPE);
    a->Go(nullptr);   // async: don't block the load
}

void MainWindow::LoadFrom(const char* path, bool asRecovery) {
    // Ask first, before stopping anything or touching the open editors: a
    // Cancel must leave the session exactly as it was. (The Save answer goes
    // through SaveTo, which flushes the editors itself.)
    if (!ConfirmDiscardChanges()) return;
    // A render that outlives the project it was rendering must not land in the
    // next one: track and clip ids restart at 1 in a loaded file, so a freeze
    // finishing after this point would find the new project's track 1.
    fRender.CancelRegionJob();
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
    PrimeSoundfonts();        // decode MIDI-track soundfonts BEFORE the engine
    RebuildPeaks();           // waveform envelopes for the loaded clips
    ShowReplacedProject(fProject->transport.playhead);
    // After the views show the new project: the prompt is modal, and whatever
    // is behind it while it waits should be what it is asking about.
    CollectMissingMedia();    // gone media: one dialog, Skip or Locate…
}

// Point every view at the project that just replaced the old one (Open, New).
// Anything holding a track or clip id from before is reset rather than left to
// address whatever reuses that id: the inspector, the docked editor, the arms.
// --- the console's strip payload (package 52) ------------------------------

// Send the model's strips to the open mixer, and remember the track layout
// they described. ONE place builds and sends the payload: the refresh funnel,
// View > Mixer asking an already-open window to update, and the drift net in
// PushTrackPeaks all come through here, so none of them can forget to record
// what the rack was told. True = a mixer was there to take it.
bool MainWindow::PushMixerStrips() {
    if (!fMixerMsgr.IsValid()) return false;
    BMessage m(kMsgMixStrips);
    AddMixerStripsPayload(m, *fProject, fTimeline);
    if (fMixerMsgr.SendMessage(&m) != B_OK) {
        // Its messenger is dead: the window is gone, and the layout it had is
        // no longer anyone's concern.
        fMixerMsgr = BMessenger();
        fMixerLayout.clear();
        return false;
    }
    fMixerLayout = TrackLayoutOf(*fProject);
    return true;
}

// Push silence to the mixer: the meter tick stops with the transport, so
// without this the rack froze at its last playing value -- a console of bars
// and holds that never came down (package 52).
void MainWindow::PushSilenceToMixer() {
    if (!fMixerMsgr.IsValid()) return;
    BMessage m(kMsgMixPeaks);
    for (const Track& t : fProject->Tracks()) {
        m.AddInt64("tid", (int64)t.id);
        m.AddFloat("pl", 0.0f);
        m.AddFloat("pr", 0.0f);
    }
    m.AddFloat("mpl", 0.0f); m.AddFloat("mpr", 0.0f);
    m.AddFloat("mrl", 0.0f); m.AddFloat("mrr", 0.0f);
    fMixerMsgr.SendMessage(&m);
}

// A window whose content is addressed by TRACK ID belongs to the project it was
// opened against. A loaded file restarts track ids at 1 (see LoadFrom), so a
// strip click or a knob move on a panel left over from the old session lands on
// a DIFFERENT track in the new one -- and the effects panel commits its whole
// chain, which is a data-corrupting write saved as an undoable edit. The native
// LV2 editors already followed this rule (CloseFxEditors); the mixer, the
// generic effects panel and the ad-hoc sends/instrument windows did not.
//
// The rule is by GENERATION, not by window kind: each window records the
// project generation it was opened against, replacing the project bumps it
// (fScope.Bump), and everything stale goes. A window type added later cannot be
// forgotten here -- which is exactly how these four were.
void MainWindow::CloseTrackWindows() {
    fScope.Bump();
    // `dropPending` is for the effects panel only: it flushes a pending wheel
    // edit when it quits, and that edit belongs to the project being REPLACED
    // (its commit names a track id that now means something else). It is told to
    // drop the edit before the close request, so nothing posts a stale chain
    // into the new session.
    auto closeIfStale = [this](BMessenger& m, ProjectScope::Generation gen,
                               bool dropPending) {
        if (!m.IsValid()) return;
        if (fScope.Stale(gen)) {
            if (dropPending) m.SendMessage(kMsgFxDropPending);
            m.SendMessage(B_QUIT_REQUESTED);
        }
        // The record is cleared either way: the window is going, and a
        // messenger kept past this would be one more stale address.
        m = BMessenger();
    };
    closeIfStale(fMixerMsgr, fMixerScope, false);
    closeIfStale(fFxMsgr,    fFxScope,    true);
    closeIfStale(fSendsMsgr, fSendsScope, false);
    closeIfStale(fInstMsgr,  fInstScope,  false);
    // The addresses those windows were pointed at are meaningless now too.
    fFxTrack = kInvalidTrackId;
    fSendsTrack = kInvalidTrackId;
    fInstTrack = kInvalidTrackId;
    fMixerLayout.clear();
    if (fTransportCtl.fEngine) fTransportCtl.fEngine->SetMeterFocus(kInvalidTrackId);
    CloseFxEditors();   // the native editors: their own flush + watch rules
}

void MainWindow::ShowReplacedProject(Frame playhead) {
    // Every window addressed by track id goes first: the ids restart at 1 in
    // the project that just loaded, so a strip or a panel left up would edit
    // the wrong track the moment it was touched (package 52). Their native
    // counterparts (the LV2 editors) are closed by the same call.
    CloseTrackWindows();
    fRecCtl.fRecTracks.clear();
    fMaster->SetValue((int32)(fProject->masterGain * 100.0f));   // sync slider
    // The inspector AND the dock's plugin page (which adds to the track the
    // inspector shows): a track id from the project that just went away means
    // nothing here, and the new project may reuse it (T2 review).
    SelectTrack(kInvalidTrackId);
    if (fDockRoll != nullptr) ClearDockedEditor();
    // The popped-out editor holds the old project's ids too, and the new
    // project's ids RESTART at 1 -- so leaving it open could silently re-point
    // it at a different region. Close it (package 51: the fresh-messenger rule
    // the other single-instance windows follow).
    if (fRollMsgr.IsValid()) {
        fRollMsgr.SendMessage(B_QUIT_REQUESTED);
        fRollMsgr = BMessenger();
    }
    fRollTrack  = kInvalidTrackId;
    fRollClip   = kInvalidClipId;
    fRollSerial = 0;   // a load must not be skipped by a serial comparison
    StopAuditionEngine();   // ... and the clicks' engine, if it was theirs
    fTimeline->SetProject(fProject);   // same object, new content
    fTimeline->SetPlayhead(playhead);
    UpdateTimeReadout(playhead);
    fTimeline->Invalidate();
    UpdateIfNeeded();
}

void MainWindow::UpdateTitle() {
    // The same 2 Hz poll keeps the Edit menu honest: the timeline executes
    // commands straight from a gesture, with no message to this window, so
    // "Undo Move Clip" and the range entries' enabled state would otherwise go
    // stale until the next undo. Before the early return below -- the title is
    // usually unchanged, which must not skip the menu.
    UpdateEditMenu();
    const std::string title = fDoc.Title();
    if (title == fTitleShown) return;   // the poll runs often; only set on change
    fTitleShown = title;
    SetTitle(title.c_str());
}

// The Edit menu's labels and enabled states (M2.4). Undo/Redo read the stack's
// own names -- "Undo Move Clip", "Redo Split at Playhead" -- so the menu says
// what the shortcut will do rather than that something will happen. The range
// entries dim when there is nothing to act on; the timeline's edits reach the
// stack without a message to this window, which is why the 2 Hz title poll
// calls this too.
void MainWindow::UpdateEditMenu() {
    if (!fUndoItem) return;   // not built yet (early startup)
    const std::string u = fStack->UndoName();
    const std::string r = fStack->RedoName();
    const std::string ul = u.empty() ? std::string("Undo") : "Undo " + u;
    const std::string rl = r.empty() ? std::string("Redo") : "Redo " + r;
    if (fUndoItem->Label() == nullptr || ul != fUndoItem->Label()) {
        fUndoItem->SetLabel(ul.c_str());
        fUndoItem->SetEnabled(!u.empty());
    }
    if (fRedoItem->Label() == nullptr || rl != fRedoItem->Label()) {
        fRedoItem->SetLabel(rl.c_str());
        fRedoItem->SetEnabled(!r.empty());
    }
    // With the DOCKED MIDI editor holding the focus, its own selection and
    // clipboard are what these items act on (package 51): Haiku skips the
    // shortcut of a DISABLED item but still swallows the key, so an item greyed
    // because the arrangement has nothing selected would make Cmd-C do nothing
    // at all in the editor.
    PianoRollView* roll = FocusedRoll();
    const bool rollSel = roll != nullptr && roll->HasSelection();
    const bool rollClip = roll != nullptr && !roll->ClipboardEmpty();
    const bool hasSel = (fTimeline && fTimeline->SelectionCount() > 0) || rollSel;
    const bool hasRange = fTimeline && fTimeline->HasTimeRange();
    const bool hasClipboard = (fTimeline && fTimeline->ClipboardCount() > 0)
                           || rollClip;
    fCutItem->SetEnabled(hasSel || hasRange);
    fCopyItem->SetEnabled(hasSel || hasRange);
    fDeleteItem->SetEnabled(hasSel || hasRange);
    fDupItem->SetEnabled(hasSel || hasRange);
    fPasteItem->SetEnabled(hasClipboard);
    fSilenceItem->SetEnabled(hasRange);
    // Duplicate Track needs a selected track to duplicate (M2.6). The menu is
    // refreshed on the 2 Hz title poll as well as after edits, so a selection
    // made by clicking a lane in the timeline enables it.
    if (fDupTrackItem != nullptr)
        fDupTrackItem->SetEnabled(fTimeline != nullptr
                                  && fTimeline->SelectedTrack()
                                         != kInvalidTrackId);
}

void MainWindow::ReportError(const char* title, const std::string& detail) {
    // Asynchronous (like the About box): a report must never block the looper,
    // and nothing here decides anything -- it is only ever a message. M1 gives
    // it the themed look; the stock alert is the honest placeholder.
    BAlert* a = new ThemedAlertWindow(title, detail.c_str(), "OK", nullptr,
                                      nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT);
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

    BAlert* a = new ThemedAlertWindow("Missing Media", list.c_str(), "Skip",
                           "Locate" B_UTF8_ELLIPSIS, nullptr,
                           B_WIDTH_AS_USUAL, B_WARNING_ALERT);
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
        const std::string file = slash == std::string::npos
                               ? c->sourcePath : c->sourcePath.substr(slash + 1);
        ShowPanel(fRelinkPanel, ("Locate " + file).c_str());
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

// --- File > New (M3.7): the dialog, then the new session -------------------

// Where the app's own binary is (a build tree's `build/daw`), for the template
// lookup. Best effort: an empty answer drops that candidate, it never fails.
static std::string AppImageDir() {
    char buf[B_PATH_NAME_LENGTH];
    if (find_path(B_APP_IMAGE_SYMBOL, B_FIND_PATH_IMAGE_PATH, NULL, buf,
                  sizeof(buf)) != B_OK)
        return std::string();
    const std::string path(buf);
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// The installation's data directory (an installed app: the package puts the
// templates in data/haiku-daw/templates). Best effort, like the above.
static std::string InstalledDataDir() {
    char buf[B_PATH_NAME_LENGTH];
    if (find_path(B_APP_IMAGE_SYMBOL, B_FIND_PATH_DATA_DIRECTORY, NULL, buf,
                  sizeof(buf)) != B_OK)
        return std::string();
    return std::string(buf);
}

void MainWindow::ShowNewProjectWindow() {
    if (fNewProjectMsgr.IsValid()) {
        BMessage act(kMsgNewProjectActivate);
        fNewProjectMsgr.SendMessage(&act);
        return;
    }
    const std::vector<std::string> candidates =
        TemplateCandidates(AppImageDir(), InstalledDataDir());
    std::vector<TemplateInfo> templates =
        ScanTemplates(FindTemplatesDir(candidates));
    // The dialog always offers Empty. With a template directory that is the
    // shipped data/templates/empty.dawproj; without one it is the built-in row
    // (no file), which is also what makes a missing directory a non-event.
    bool haveEmpty = false;
    for (const TemplateInfo& t : templates)
        if (t.id == "empty") haveEmpty = true;
    if (!haveEmpty) templates.insert(templates.begin(), EmptyTemplate());

    NewProjectWindow* win = new NewProjectWindow(std::move(templates),
                                                 candidates, BMessenger(this));
    fNewProjectMsgr = BMessenger(win);
    win->Show();
}

// The session a New (or a template) starts. The sample rate is the
// default-sample-rate preference (M3.5) -- the rate belongs to the session
// and to what the user asked new projects to be at, not to the template's
// content, and the template's tracks, names and routing are what it brings.
// Same "the engine rebuilds at the next play" stance as LoadFrom.
void MainWindow::NewProjectFrom(const std::string& templateId,
                                const std::string& templatePath) {
    if (!ConfirmDiscardChanges()) return;
    // Same rule as LoadFrom: the project this could render into is about to be
    // replaced, and a new template's ids start over.
    fRender.CancelRegionJob();
    fTransportCtl.StopPlayback();
    fRecCtl.StopRecording();
    FlushFxEditors();
    CloseFxEditors();

    bool fromTemplate = !templatePath.empty() && templateId != "empty";
    if (fromTemplate) {
        TemplateInfo info;
        info.id = templateId;
        info.path = templatePath;
        Project loaded;
        if (LoadTemplate(info, loaded)) {
            *fProject = std::move(loaded);
        } else {
            // A template that cannot be read is reported and the user gets the
            // empty session they asked for the new project to be -- never a
            // half-loaded one.
            ReportError("New Project",
                        std::string("The template could not be read: ")
                        + templatePath + "\nStarting an empty project.");
            fromTemplate = false;
        }
    }
    if (!fromTemplate) *fProject = Project{};

    const double rate = fPrefsSampleRate > 0 ? (double)fPrefsSampleRate : 48000.0;
    // A template carries a timeline, and a timeline is frames: adopting the
    // preferred rate without moving them would re-interpret every clip start
    // and length at the new rate. AdoptProjectRate rescales first.
    AdoptProjectRate(*fProject, rate);
    fStack->Clear();
    fDoc.NoteNew();               // no path, and nothing to lose
    fRecCtl.fTakeDir.clear();
    UpdateTitle();
    RebuildPeaks();
    ShowReplacedProject(0);
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
        BMenuItem* none = new ThemedMenuItem("(none)", nullptr);
        none->SetEnabled(false);   // an empty submenu is a dead end
        fRecentMenu->AddItem(none);
        return;
    }
    for (const std::string& path : fDoc.Recent()) {
        BMenuItem* item = new ThemedMenuItem(
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
    BAlert* a = new ThemedAlertWindow("Unsaved Changes",
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
    // One factory for every format (WAV, AIFF/AIFFC, FLAC, Ogg Vorbis), chosen
    // by what the file's first bytes are rather than by its name. A format
    // this build has no library for is reported as exactly that, not as a
    // corrupt file.
    std::string why;
    std::unique_ptr<IAudioSource> src = OpenAudioSource(path, &why);
    if (!src) {
        std::fprintf(stderr, "MainWindow: cannot import '%s': %s\n", path,
                     why.c_str());
        ReportError("Import Audio", std::string("Cannot read \"")
                    + path + "\": " + why + ".");
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

    const double srcRate = src->FrameRate();
    const double ratio = srcRate > 0 ? fProject->sampleRate / srcRate : 1.0;
    Clip clip;
    clip.startFrame   = start < 0 ? 0 : start;
    clip.lengthFrames = (int64_t)llround(src->TotalFrames() * ratio);
    clip.sourceOffset = 0;
    clip.sourcePath   = path;
    fStack->Execute(std::make_unique<AddClipCommand>(tid, clip), *fProject);

    // Tag the file with its duration (BFS attribute) so the sample browser's
    // BQuery can find/sort it. BPM/Key are user-set in the browser.
    if (srcRate > 0) {
        EnsureDawIndexes(path);
        WriteAttrFloat(path, kAttrDuration, (float)(src->TotalFrames() / srcRate));
    }

    (*fPeaks)[path].Build(*src);   // waveform envelope (src cursor is at start)
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
    // M4.4: the envelopes are built by a worker, off this thread, and a cache
    // file (keyed by the source's mtime, size and bucket size) spares the second
    // open the whole scan. This used to open and read EVERY clip's file right
    // here, on the window thread, on every load — a project with a few hundred
    // clips froze the window for as long as all that audio takes to read, twice
    // over on the second open. What is left here is collecting the paths and
    // dropping the ones already drawn.
    fPeaks->clear();
    std::vector<std::string> paths;
    for (const Track& t : fProject->Tracks())
        for (const Clip& c : t.clips) {
            if (c.sourcePath.empty()) continue;
            if (std::find(paths.begin(), paths.end(), c.sourcePath) != paths.end())
                continue;
            paths.push_back(c.sourcePath);
        }
    fPeakLoader.Start(std::move(paths));
    UpdatePulse();   // the envelopes are drained by the pulse, so it must run
}

// A unique path for a rendered region/freeze file: in the take dir (beside the
// project) if known, else the working dir. Suffixed with a session counter so
// repeated ops don't collide.










// (The isolated-track render Freeze uses -- RenderTrackToWav -- lives in
// src/model/RegionJobs.cpp now: it runs on the region job's worker thread, and
// everything it needs was already host-testable in the model library.)

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
    s.previewNotes = fRollPreview;   // the MIDI editor's audition (package 51)
    s.exportContainer = (int)fExportChoices.container;
    s.exportBitDepth  = fExportChoices.bitDepth;
    s.exportVorbisQuality = fExportChoices.vorbisQuality;
    s.exportDither    = fExportChoices.dither;
    s.exportSampleRate = fExportChoices.sampleRate;
    s.exportNormalize = fExportChoices.normalize;
    s.exportTargetLufs = fExportChoices.targetLufs;
    s.exportTruePeakCeil = fExportChoices.truePeak;
    s.exportLimiter   = fExportChoices.limiter;
    s.exportRange     = fExportChoices.range;
    s.exportStems     = fExportChoices.stems ? 1 : 0;
    s.themeMode       = ActiveThemeMode();
    s.takeFormat      = fRecCtl.fTakeFormat;   // take depth (M5)
    s.previewNotes    = fRollPreview;          // the MIDI editor's audition (51)
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
        // The page first (it decides what the body holds), then whether the dock
        // is showing at all. Through SetDockPage, not by assigning fDockPage:
        // the restore used to leave the body on the editor hint (and the
        // highlighted segment inert, since a click on the current segment is a
        // no-op) until the user switched pages twice (T2 review).
        if (s.dockPage >= kDockEditor && s.dockPage <= kDockPlugins)
            SetDockPage(s.dockPage);
        SetDockShown(s.bottomVisible);
    }

    fTransportCtl.fBufferFrames = (size_t)s.bufferFrames;
    fRecCtl.fCountInBars  = s.countInBars;
    fRecCtl.fTakeFormat   = s.takeFormat;   // take depth (M5); the recorder
                                            // clamps an unknown value itself
    fTransportCtl.fMetronome    = s.metronome;
    fTransportCtl.fMonitorInput = s.monitorInput;
    fRollPreview = s.previewNotes;   // the MIDI editor's audition (package 51)
    if (fTimeline) fTimeline->SetMonitorInput(fTransportCtl.fMonitorInput);
    fLastDir      = s.lastDir;
    fDoc.SetRecent(s.recentProjects);
    RebuildRecentMenu();
    // The export dialog reopens on the last choices, not on its defaults. A
    // remembered container this build cannot write (a settings file carried
    // over from a build with FLAC, say) falls back to WAV rather than to a
    // bounce that could only fail.
    const AudioFileFormat savedContainer = (AudioFileFormat)s.exportContainer;
    fExportChoices.container  = AudioFileFormatCanWrite(savedContainer)
                                ? savedContainer : AudioFileFormat::Wav;
    fExportChoices.bitDepth   = s.exportBitDepth;
    fExportChoices.vorbisQuality = s.exportVorbisQuality;
    fExportChoices.dither     = s.exportDither;
    fExportChoices.sampleRate = s.exportSampleRate;
    fExportChoices.normalize  = s.exportNormalize;
    fExportChoices.targetLufs = s.exportTargetLufs;
    fExportChoices.truePeak   = s.exportTruePeakCeil;
    fExportChoices.limiter    = s.exportLimiter;
    fExportChoices.range      = s.exportRange;
    fExportChoices.stems      = s.exportStems != 0;
    if (fMetItem)   fMetItem->SetMarked(fTransportCtl.fMetronome);
    // The Preferences window's own keys (M3.5).
    fTransportCtl.fMetronomeSound = s.metronomeSound;
    fTransportCtl.fMetronomeGain  = s.metronomeLevel;
    fPrefsSampleRate      = s.defaultSampleRate;
    fPrefsProjectDir      = s.projectDir;
    // The UI SCALE is APPLIED here, not only read. main.cpp applies the stored
    // scale before the first window exists, so for a fresh launch the two
    // agree — but any other reload (the suite's ReloadSettings, and the
    // startup path in a process that does not go through main.cpp) left the
    // app REMEMBERING one scale and DRAWING at another. The visible symptom is
    // a Preferences row showing 125% over a 100% layout, and a change to the
    // same value being a no-op that never reaches the screen. One number, one
    // place: what was read is what is drawn.
    if (fPrefsUiScale != s.uiScale) {
        fPrefsUiScale = s.uiScale;
        SetThemeScaleOverride(s.uiScale);
        ApplyThemeToAllWindows();
        if (fTimeline) fTimeline->Invalidate();
    }
    fPrefsAutosaveSeconds = s.autosaveSeconds;
    StartAutosave(fPrefsAutosaveSeconds);   // 0 = off
    // The marker lane (M2.7): a view preference, restored with the panes.
    if (fTimeline) fTimeline->SetMarkerLane(s.markerLaneVisible);
    if (fMarkerLaneItem) fMarkerLaneItem->SetMarked(s.markerLaneVisible);
    // Restore the window frame: sane in both directions, and no larger than
    // the screen it is being restored on. A frame saved on a bigger display
    // (or by a test run) otherwise puts the window's own controls -- the dock's
    // Pop out and x, at the right end of its strip -- off the screen entirely.
    if (s.winR - s.winL > 320 && s.winB - s.winT > 240) {
        const BRect screen = BScreen(this).Frame();
        float w = std::min(s.winR - s.winL, screen.Width() - 20.0f);
        float h = std::min(s.winB - s.winT, screen.Height() - 40.0f);
        float x = std::max(8.0f, std::min(s.winL, screen.right - w));
        float y = std::max(8.0f, std::min(s.winT, screen.bottom - h));
        MoveTo(x, y);
        ResizeTo(w, h);
    }
}

void MainWindow::SaveSettings() {
    AppSettings s;
    s.bufferFrames = (int)fTransportCtl.fBufferFrames;
    s.countInBars  = fRecCtl.fCountInBars;
    s.metronome    = fTransportCtl.fMetronome;
    s.monitorInput = fTransportCtl.fMonitorInput;
    // The MIDI editor's audition (package 51). The other settings writers set
    // this; the one that runs at quit did not, so the preference was lost
    // every session, whatever the window said.
    s.previewNotes = fRollPreview;
    s.lastDir      = fLastDir;
    s.recentProjects = fDoc.Recent();
    s.exportContainer = (int)fExportChoices.container;
    s.exportBitDepth  = fExportChoices.bitDepth;
    s.exportVorbisQuality = fExportChoices.vorbisQuality;
    s.exportDither    = fExportChoices.dither;
    s.exportSampleRate = fExportChoices.sampleRate;
    s.exportNormalize = fExportChoices.normalize;
    s.exportTargetLufs = fExportChoices.targetLufs;
    s.exportTruePeakCeil = fExportChoices.truePeak;
    s.exportLimiter   = fExportChoices.limiter;
    s.exportRange     = fExportChoices.range;
    s.exportStems     = fExportChoices.stems ? 1 : 0;
    s.themeMode       = ActiveThemeMode();
    s.takeFormat      = fRecCtl.fTakeFormat;   // take depth (M5)
    // The Preferences window's keys (M3.5).
    s.defaultSampleRate = fPrefsSampleRate;
    s.metronomeSound    = fTransportCtl.fMetronomeSound;
    s.metronomeLevel    = fTransportCtl.fMetronomeGain;
    s.uiScale           = fPrefsUiScale;
    s.projectDir        = fPrefsProjectDir;
    s.autosaveSeconds   = fPrefsAutosaveSeconds;
    const BRect fr = BWindow::Frame();
    s.winL = fr.left; s.winT = fr.top; s.winR = fr.right; s.winB = fr.bottom;
    // The panes as the user left them (M1.4).
    s.inspectorVisible = fInspectorShown;
    if (fInspector != nullptr && fInspector->Bounds().Width() > 1.0f)
        s.inspectorWidth = fInspector->Bounds().Width();
    if (fRootSplit != nullptr && fDock != nullptr) {
        s.dockPage = fDockPage;
        s.bottomVisible = fDock->Parent() != nullptr;
        if (fDock->Bounds().Height() > 1.0f)
            s.bottomHeight = fDock->Bounds().Height();
    }
    if (fTimeline != nullptr) s.markerLaneVisible = fTimeline->MarkerLaneOn();

    BPath p;
    if (!ProjectDocument::SettingsPath(p)) return;
    BFile f(p.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    if (f.InitCheck() != B_OK) return;
    const std::string t = s.Serialize();
    f.Write(t.data(), t.size());
}

void MainWindow::ApplyTheme() {
    // What this window CACHED. Everything else it draws (the timeline, the
    // inspector, the meter, the transport glyphs) reads the tokens at draw
    // time and only needs the invalidate the walker does.
    //
    // The window's own top view is what shows where no pane covers it: a
    // BWindow has no background colour of its own to set, and the app may not
    // change the system panel colour it would otherwise use (T1) — the
    // layout's owner view is the seam that is left.
    if (BLayout* layout = GetLayout()) {
        if (BView* top = layout->Owner()) {
            top->SetViewColor(ColBackground());
            // SetViewColor tells the server the colour but does not repaint the
            // view, so the window's own background has to be asked for.
            top->Invalidate();
        }
    }

    const rgb_color chrome = ColChrome();
    // (The time display is the bar's own child now — TransportTimeView — and it
    // takes the LCD tokens in its own ApplyTheme, so a mode switch re-colours it
    // with the rest of the bar.)
    for (BStringView* label : { fVolLbl, fBpmLbl, fLoudView }) {
        if (label == nullptr) continue;
        label->SetViewColor(chrome);
        label->SetHighColor(ColText());
    }
    if (fMaster != nullptr) {
        fMaster->SetViewColor(chrome);
        fMaster->SetLowColor(chrome);
        fMaster->SetBarColor(ColLcd());
        rgb_color fill = ColAccent();
        fMaster->UseFillColor(true, &fill);
    }
    if (fDock != nullptr)       fDock->SetViewColor(ColBackground());
    if (fEditorPane != nullptr) fEditorPane->SetViewColor(ColBackground());
    if (fDockEmpty != nullptr)  fDockEmpty->SetViewColor(ColBackground());
    // The dock's two BStringViews need their VIEW colour re-taken, not just
    // their text colour: a BStringView copies its parent's view colour when it
    // is attached (StringView.cpp), so a mode switch left the strip's title and
    // the hint as boxes in the mode they were built in -- in the dark mode the
    // title drew light text on its own light box, i.e. the page's name was
    // unreadable. Colours match the parents set just above (the pane header and
    // the empty-state group).
    if (fDockTitle != nullptr) {
        fDockTitle->SetViewColor(ColHeader());
        fDockTitle->SetLowColor(ColHeader());
        fDockTitle->SetHighColor(ColText());
    }
    if (fDockHint != nullptr) {
        fDockHint->SetViewColor(ColBackground());
        fDockHint->SetLowColor(ColBackground());
        fDockHint->SetHighColor(ColTextDim());
    }
}

void MainWindow::SetThemeMode(ThemeMode mode) {
    SetActiveThemeMode(mode);
    if (mode == ThemeMode::System)
        ReadSystemBaseColors();
    InstallControlLookForMode(mode);
    ApplyThemeToAllWindows();
    SaveSettings();   // it is a preference: it survives the quit
}

// The big position (M3.1). The format is the window's own (fTimeMode): the
// display view cycles it by asking, and every position in every format comes
// from the kit-free TimeDisplay header, so what is shown and what a typed
// entry means cannot drift apart.
void MainWindow::UpdateTimeReadout(Frame playhead) {
    const double rate = fTransportCtl.fEngine ? fTransportCtl.fEngine->OutputRate() : fProject->sampleRate;
    fProject->tempoMap.sampleRate = fProject->sampleRate;
    if (!fTransport || fTransport->TimeView() == nullptr) return;
    const std::string text =
        FormatTimePosition(fTimeMode, playhead, rate, fProject->tempoMap);
    fTransport->TimeView()->SetPosition(text.c_str(), TimeModeTag(fTimeMode));
}

// The readout line under the position: the tempo and meter at the playhead,
// the audio callback's own load and the xruns (M3.1). The load figure is the
// engine's PEAK hold since the last poll, taken (and cleared) here: what
// matters is the worst block, not the average one.
void MainWindow::UpdateTransportInfo() {
    if (!fTransport) return;
    fProject->tempoMap.sampleRate = fProject->sampleRate;
    const Frame ph = fProject->transport.playhead;
    const double bpm = fProject->tempoMap.BpmAt(ph);
    int num = 4, den = 4;
    fProject->tempoMap.Meter(ph, &num, &den);

    uint64_t xruns = 0;
    if (fTransportCtl.fEngine) {
        // The PEAK since the last poll, taken (and cleared) here: an average
        // hides exactly the block that overran and clicked. Between polls the
        // figure is therefore "the worst block in the last ~16 ms", which is
        // what a DAW's load meter is for. A take that reads it twice in one
        // event (a state change right after a pulse) keeps the last value
        // instead of flashing 0.
        const bigtime_t peakUs = fTransportCtl.fEngine->TakePeakCallbackUs();
        if (peakUs > 0) fDspPercent = fTransportCtl.fEngine->LoadPercentFor(peakUs);
        xruns = fTransportCtl.fEngine->Xruns();
    }
    const float dsp = fDspPercent;
    // The take's own xrun (the recorder's ring overflowing) is a dropout too,
    // and it is latched for the take, so one is one.
    if (fRecCtl.fRecorder && fRecCtl.fRecorder->Xrun())
        xruns += 1;

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.1f BPM  %d/%d   DSP %.0f%%   XR %llu",
                  bpm, num, den, dsp, (unsigned long long)xruns);
    fTransport->SetInfo(buf);
}

// The lamps on the transport buttons. Every piece of state is somebody else's
// (the transport controller, the record controller, the project's transport) —
// this only mirrors it out to the bar.
void MainWindow::UpdateTransportButtons() {
    if (!fTransport) return;
    fTransport->SetPlaying(fTransportCtl.fPlaying);
    fTransport->SetRecording(fRecCtl.fRecMode);
    fTransport->SetLoop(fProject->transport.loopEnabled);
    fTransport->SetClick(fTransportCtl.fMetronome);
    fTransport->SetPunch(fProject->transport.punchEnabled);
}

// The status line: the transport's state, so it is never stale — a job or an
// error message replaces it until it EXPIRES (kStatusHoldUs; the 2 Hz title
// poll drops it, see ExpireTransportStatus) or the state changes.
void MainWindow::UpdateTransportStatus() {
    if (!fTransport) return;
    std::string s;
    if (fRecCtl.fRecMode) {
        if (fRecCtl.fCapturePending)      s = "Recording: count-in";
        else if (fRecCtl.fLoopRecord)     s = "Recording: loop takes";
        else if (!fRecCtl.fTakePath.empty()) {
            const size_t slash = fRecCtl.fTakePath.find_last_of('/');
            s = "Recording " + (slash == std::string::npos
                                ? fRecCtl.fTakePath
                                : fRecCtl.fTakePath.substr(slash + 1));
        } else s = "Recording";
    } else if (fTransportCtl.fPlaying) {
        s = fProject->transport.loopEnabled ? "Playing (loop)" : "Playing";
    } else if (fTransportCtl.fMonitoring) {
        s = "Monitoring input";
    } else {
        s = "Stopped";
    }
    if (s == fStatusState) return;   // the state has not changed: leave any
                                     // job/error message standing
    fStatusState = s;
    fStatusExpiryUs = 0;             // the state line replaced the message
    fTransport->SetStatus(s.c_str());
}

void MainWindow::SetTransportStatus(const char* text) {
    if (!fTransport) return;
    fTransport->SetStatus(text ? text : "");
    // A job/error message is transient: the transport state takes the row back
    // after a few seconds. The timer is the existing 2 Hz title poll's -- one
    // more runner for a deadline that a runner already visits would be a
    // second thing to keep alive for nothing.
    fStatusExpiryUs = system_time() + kStatusHoldUs;
}

// The message's hold is up: put the transport state back. Called from the 2 Hz
// title poll, so a message outlives its work by at most that poll's interval.
void MainWindow::ExpireTransportStatus() {
    if (fStatusExpiryUs == 0 || system_time() < fStatusExpiryUs) return;
    fStatusExpiryUs = 0;
    fStatusState.clear();   // force UpdateTransportStatus to set the state line
    UpdateTransportStatus();
}

// The bar view and one of its control rects, for the functional tests: a click
// has to land where the bar DREW the button, not at an offset a test guessed
// (the arrange-depth package learned that the hard way -- docs/HANDOFF.md).
BView* MainWindow::TransportBarView() const { return fTransport; }

BRect MainWindow::TransportButtonRect(int which) const {
    if (which < 0 || which >= (int)TransportButton::Count) return BRect();
    return ButtonRect((TransportButton)which);
}

// The three readouts the bar's own state drives, in one call: the lamps, the
// status line and the LCD's info line. Every path that changes one of them
// (the pulse, a button, a key, the menus) goes through here.
void MainWindow::UpdateTransportReadouts() {
    UpdateTransportButtons();
    UpdateTransportStatus();
    UpdateTransportInfo();
}

// Rewind / fast-forward (M3.1): the playhead to the previous / next bar, on
// the tempo map's own bar boundaries. The bar the playhead is IN is the anchor,
// so pressing rewind inside bar 3 lands on bar 2's downbeat rather than bar 3's.
void MainWindow::NudgePlayheadBar(int dir) {
    if (!fTimeline) return;
    fProject->tempoMap.sampleRate = fProject->sampleRate;
    const Frame ph = fProject->transport.playhead;
    int bar = 1, beat = 1;
    fProject->tempoMap.BarBeat(ph, &bar, &beat);
    if (dir < 0 && bar <= 1) {        // already in the first bar: the start
        fTimeline->ReturnToZero();
        return;
    }
    const int target = dir < 0 ? bar - 1 : bar + 1;
    const Frame f = FrameAtBarBeat(fProject->tempoMap, target, 1, 0);
    if (f < 0) return;
    fTimeline->SeekTo(f);
}

void MainWindow::PushRollPlayhead(Frame ph) {
    if (fDockRoll != nullptr) fDockRoll->SetPlayhead(ph);   // docked editor
    if (!fRollMsgr.IsValid()) return;
    BMessage m(kMsgRollPlayhead);
    m.AddInt64("ph", (int64)ph);
    fRollMsgr.SendMessage(&m);
}

// --- the MIDI editor's snapshot sync (package 51, items A/B) ---------------

// The region state ONE push carries. The note and event array shapes are the
// ones the apply messages use (PianoRoll.h: AddRollNotes/AddRollEvents), so the
// roll decodes a push with the same code the window decodes a commit with. A
// region that is gone is sent as "gone": the view cannot tell from an empty
// note list, and closing the editor is the honest answer (the dock clears, the
// popped-out window quits).
void MainWindow::AddRegionPayload(BMessage& m, TrackId track, ClipId clip,
                                  uint64_t serial) {
    m.AddInt64("track", (int64)track);
    m.AddInt64("clip", (int64)clip);
    m.AddInt64("serial", (int64)serial);
    const Track* t = fProject->FindTrack(track);
    const MidiClip* c = t != nullptr ? t->FindMidiClip(clip) : nullptr;
    if (c == nullptr) {
        m.AddBool("gone", true);
        return;
    }
    m.AddInt64("start", (int64)c->startFrame);
    m.AddInt64("len",   (int64)c->lengthFrames);
    AddRollNotes(&m, c->notes);
    AddRollEvents(&m, c->events);
}

// Push the region's CURRENT state to every open roll (the docked view and the
// popped-out window), so no roll can commit a list the model no longer has.
// Called after every stack change and from the 2 Hz poll; the serial gate is
// what keeps the poll free — an unchanged model does not rebuild the message.
//
// `force` is for the paths where the serial alone is not the whole story: a
// roll that just opened, and the refusal path (a stale commit means the roll
// needs the truth NOW).
void MainWindow::RefreshRolls(bool force) {
    if (fDockRoll == nullptr && !fRollMsgr.IsValid()) {
        fRollSerial = fStack->CurrentSerial();   // remember where we are
        return;
    }
    const uint64_t serial = fStack->CurrentSerial();
    if (!force && serial == fRollSerial) return;
    fRollSerial = serial;

    // The docked view is hosted here: a region that is gone empties the pane
    // (the same treatment every other view holding a dead id gets). A region
    // that only moved or changed is the view's own business (RollEdit.h
    // decides), so it is pushed the state and re-reads it.
    if (fDockRoll != nullptr) {
        const Track* t = fProject->FindTrack(fDockTrack);
        const MidiClip* c = t != nullptr ? t->FindMidiClip(fDockClip) : nullptr;
        if (c == nullptr) {
            ClearDockedEditor();
            fDockPushedSig = 0;
        } else {
            BMessage m(kMsgRollRegion);
            AddRegionPayload(m, fDockTrack, fDockClip, serial);
            fDockPushedSig = RollRegionSignature(c->startFrame, c->lengthFrames,
                                                 c->notes, c->events);
            fDockRoll->SyncRegion(fDockTrack, fDockClip, c->startFrame,
                                  c->lengthFrames, c->notes, c->events, serial,
                                  false);
        }
    }
    if (fRollMsgr.IsValid()) {
        const Track* t = fProject->FindTrack(fRollTrack);
        const MidiClip* c = t != nullptr ? t->FindMidiClip(fRollClip) : nullptr;
        BMessage m(kMsgRollRegion);
        AddRegionPayload(m, fRollTrack, fRollClip, serial);
        fPopPushedSig = c != nullptr
                            ? RollRegionSignature(c->startFrame, c->lengthFrames,
                                                  c->notes, c->events)
                            : 0;
        fRollMsgr.SendMessage(&m);
    }
}

// One roll's commit, checked in one place (the three apply handlers call it):
// the base must be the model's serial, the roll's own batched transition, or a
// serial whose movement did not touch THIS region (its fingerprint still
// matches what the roll was pushed). Anything else means the model changed
// under the roll, and the commit is refused -- never written on top.
bool MainWindow::RollCommitOk(const BMessage* msg, TrackId track, ClipId clip) {
    int64 base = 0;
    if (msg->FindInt64("base", &base) != B_OK)
        return true;   // not a hosted roll (a test harness builds its own)
    const uint64_t now = fStack->CurrentSerial();
    if (RollCommitAllowed((uint64_t)base, now, track, clip, fRollGate))
        return true;
    const Track* t = fProject->FindTrack(track);
    const MidiClip* c = t != nullptr ? t->FindMidiClip(clip) : nullptr;
    if (c == nullptr) return false;   // the region is gone: refuse + re-read
    const uint64_t sig = RollRegionSignature(c->startFrame, c->lengthFrames,
                                             c->notes, c->events);
    const uint64_t pushed = (track == fDockTrack && clip == fDockClip)
                                ? fDockPushedSig : fPopPushedSig;
    return pushed != 0 && sig == pushed;
}

PianoRollView* MainWindow::FocusedRoll() const {
    if (fDockRoll == nullptr) return nullptr;
    // Only the DOCKED view: a popped-out roll is its own window and never sees
    // this window's menu bar.
    return fDockRoll->IsFocus() ? fDockRoll : nullptr;
}

void MainWindow::PushRollFollow() {
    const bool on = !fFollowItem || fFollowItem->IsMarked();
    if (fDockRoll != nullptr) fDockRoll->SetFollow(on);
    if (!fRollMsgr.IsValid()) return;
    BMessage m(kMsgRollFollow);
    m.AddBool("on", on);
    fRollMsgr.SendMessage(&m);
}

// --- the audition (package 51, item D) -------------------------------------

// Every click (re)arms the idle timer: the note's release tail has rung out by
// the time it fires, and a click while it was pending cancels the stop. Notes
// still HELD keep it re-arming (MSG_AUDITION_IDLE's handler).
void MainWindow::StartAuditionIdleTimer() {
    delete fAuditionIdle;
    // 3 s: long enough that a slow one-finger arpeggio keeps one engine, short
    // enough that the device is not held by an editor nobody is clicking.
    fAuditionIdle = new BMessageRunner(BMessenger(this),
                                       new BMessage(MSG_AUDITION_IDLE),
                                       3000000, 1);
}

// The roll asks; this decides. A click sounds through the region's own track:
// its instrument, fader and FX, which is the note the region will play. The
// engine has to be RUNNING for that, so an idle transport gets the monitor-only
// engine the armed-track monitor uses (no clips, no playhead) — and it is put
// away again a few seconds after the last click, so a session that never plays
// does not hold the device open for the editor.
Engine* MainWindow::EnsureAuditionEngine(TrackId track) {
    if (!fRollPreview || track == kInvalidTrackId) return nullptr;
    Engine* e = fTransportCtl.EnsureEngine();
    // A running transport already has the device and a graph: aim the clicks at
    // the region's track and use it (the bus is there whenever the track has
    // notes, is armed/monitoring, or is this build's audition target).
    if (fTransportCtl.fPlaying || fRecCtl.fRecMode || fTransportCtl.fMonitoring) {
        if (e->AuditionTrack() != track) {
            e->SetAuditionTrack(track);
            // The monitor graph is the one that needs a rebuild to carry the
            // bus; a playback graph picks it up on its next rebuild, and until
            // then the tracks that already carry notes still sound.
            if (fTransportCtl.fMonitoring && !fTransportCtl.fPlaying
                && !fRecCtl.fRecMode)
                fRecCtl.UpdateIdleMonitor();
        }
        return e;
    }
    if (fAuditionUp && fAuditionTrack == track && e->PlayerRunning())
        return e;   // already up (a stopped engine would eat the click: the
                    // ring it drains is only read while the callback runs)
    e->SetBufferFrames(fTransportCtl.fBufferFrames);
    e->SetMonitorOnly(true);
    e->SetAuditionTrack(track);
    const Frame ph = fProject->transport.playhead;
    const Frame tenMin = (Frame)(fProject->sampleRate * 600.0);
    if (e->Load(*fProject, ph, ph + tenMin) != B_OK) {
        // No device (or nothing to build). The click is silent; an editor that
        // pops an alert for every click would be worse than a quiet one.
        e->SetMonitorOnly(false);
        fAuditionUp = false;
        return nullptr;
    }
    e->Start();
    fAuditionUp = true;
    fAuditionTrack = track;
    return e;
}

void MainWindow::StopAuditionEngine() {
    if (fAuditionIdle != nullptr) {   // whatever else, the timer goes
        delete fAuditionIdle;
        fAuditionIdle = nullptr;
    }
    if (!fAuditionUp) return;
    const bool wasUp = fAuditionUp;
    fAuditionUp = false;
    fAuditionTrack = kInvalidTrackId;
    fAuditionHeld = 0;
    if (!wasUp) return;
    // ONLY an engine the clicks own may be stopped: a play, a take and the
    // armed-track monitor are all the SAME engine object (M4.1 keeps one
    // player), and stopping it there would freeze the transport with its lamp
    // still lit and no way back except a manual Stop.
    if (fTransportCtl.fPlaying || fRecCtl.fRecMode || fTransportCtl.fMonitoring)
        return;
    if (fTransportCtl.fEngine) {
        fTransportCtl.fEngine->Stop();
        fTransportCtl.fEngine->SetMonitorOnly(false);
        fTransportCtl.fEngine->SetAuditionTrack(kInvalidTrackId);
    }
}

// The clicks themselves: one note-on/note-off at a time, straight into the
// engine's ring. Nothing here touches the model — an audition is not an edit.
void MainWindow::HandleRollAudition(const BMessage* msg) {
    if (!fRollPreview) return;
    int64 tid = 0; int32 pitch = 0, vel = 100; bool on = true;
    msg->FindInt64("track", &tid);
    msg->FindInt32("pitch", &pitch);
    msg->FindInt32("vel", &vel);
    msg->FindBool("on", &on);
    Engine* e = EnsureAuditionEngine((TrackId)tid);
    if (e == nullptr) return;
    e->AuditionNote((int)pitch, (int)vel, on);
    if (on)               fAuditionHeld++;
    else if (fAuditionHeld > 0) fAuditionHeld--;
    StartAuditionIdleTimer();
}

// The master meter's readings for one poll (M3.3).
//
// Peaks, RMS and correlation all come from the engine's per-block meter —
// except during a take, when the meter must show the INPUT: then the peaks are
// the recorder's and there is no RMS/correlation for that signal (the engine
// is mixing the project, not the input), so those read 0 and the RMS line and
// correlation indicator stay at rest.
void MainWindow::PushMeter(bool fromRecorder) {
    if (fMeter == nullptr) return;
    MeterReadings r;
    if (fromRecorder && fRecCtl.fRecorder) {
        r.peakL = fRecCtl.fRecorder->PeakL();
        r.peakR = fRecCtl.fRecorder->PeakR();
        const uint32_t bits = fRecCtl.fRecorder->ClipFlags();
        r.clipL = (bits & kMeterClipL) != 0;
        r.clipR = (bits & kMeterClipR) != 0;
    } else if (fTransportCtl.fEngine) {
        r.peakL = fTransportCtl.fEngine->PeakL();
        r.peakR = fTransportCtl.fEngine->PeakR();
        r.rmsL  = fTransportCtl.fEngine->RmsL();
        r.rmsR  = fTransportCtl.fEngine->RmsR();
        r.correlation = fTransportCtl.fEngine->Correlation();
        const uint32_t bits = fTransportCtl.fEngine->ClipFlags();
        r.clipL = (bits & kMeterClipL) != 0;
        r.clipR = (bits & kMeterClipR) != 0;
    }
    fMeter->SetReadings(r);
}

// The markers window's list (M2.7): a pure snapshot -- frame, name and the
// pre-formatted position (the transport's own bar:beat + min:sec readout), so
// the window needs neither the model nor the tempo map.
void MainWindow::PushMarkers() {
    if (!fMarkersMsgr.IsValid() || !fProject) return;
    BMessage m(kMsgMarkerList);
    fProject->tempoMap.sampleRate = fProject->sampleRate;
    for (const Marker& mk : fProject->markers) {
        const double sec  = fProject->sampleRate > 0
                          ? (double)mk.frame / fProject->sampleRate : 0.0;
        const int    mins = (int)(sec / 60.0);
        const double rem  = sec - mins * 60.0;
        int bar = 1, beat = 1;
        fProject->tempoMap.BarBeat(mk.frame, &bar, &beat);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%d.%d   %d:%06.3f", bar, beat, mins, rem);
        m.AddInt64("frame", (int64)mk.frame);
        m.AddString("name", mk.name.c_str());
        m.AddString("time", buf);
    }
    m.AddInt64("playhead", (int64)fProject->transport.playhead);
    fMarkersMsgr.SendMessage(&m);
}

// View > Markers: one window, like the mixer (M1.4's rule). Asking again
// brings the open one forward with the current list.
void MainWindow::ShowMarkersWindow() {
    std::vector<MarkerInfo> list;
    fProject->tempoMap.sampleRate = fProject->sampleRate;
    for (const Marker& mk : fProject->markers) {
        MarkerInfo info;
        info.frame = mk.frame;
        info.name  = mk.name;
        const double sec  = fProject->sampleRate > 0
                          ? (double)mk.frame / fProject->sampleRate : 0.0;
        const int    mins = (int)(sec / 60.0);
        const double rem  = sec - mins * 60.0;
        int bar = 1, beat = 1;
        fProject->tempoMap.BarBeat(mk.frame, &bar, &beat);
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%d.%d   %d:%06.3f", bar, beat, mins, rem);
        info.time = buf;
        list.push_back(std::move(info));
    }
    if (fMarkersMsgr.IsValid()) {
        PushMarkers();
        BMessage act(kMsgMarkerActivate);
        fMarkersMsgr.SendMessage(&act);
        return;
    }
    BRect wr(140, 120, 140 + 330, 120 + 320);
    MarkersWindow* win = new MarkersWindow(wr, std::move(list),
                                           fProject->transport.playhead,
                                           BMessenger(this));
    fMarkersMsgr = BMessenger(win);
    win->Show();
    // A list you just asked for is in front and ready for the keyboard: the
    // same thing the second ask does through kMsgMarkerActivate.
    win->Activate();
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

    // The strip-payload drift net (package 52). The console draws the model's
    // TRACK LAYOUT -- one strip per track, in the model's order -- and every
    // track-list edit posts kMsgUiRefresh to refresh it. This is the net under
    // those posts, on a tick that already runs: if the layout the rack was last
    // sent is not the model's (a track added, deleted or reordered by some path
    // that did not announce itself), the whole payload goes again. Cheap: a
    // vector compare against the ids recorded by PushMixerStrips.
    if (fMixerMsgr.IsValid()
        && !TrackLayoutMatches(fMixerLayout, TrackLayoutOf(*fProject)))
        PushMixerStrips();

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
        // The master's RMS, for the master strip's own RMS line. Per-TRACK RMS
        // does not exist in the engine (only the master runs the sliding
        // StereoMeter), so a track strip has no RMS line to draw.
        m.AddFloat("mrl", fTransportCtl.fEngine->RmsL());
        m.AddFloat("mrr", fTransportCtl.fEngine->RmsR());
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
    title += master ? " \xE2\x80\x94 Master" : (" \xE2\x80\x94 " + t->name);

    BRect uw(160, 160, 160 + 960, 160 + 680);
    Lv2UiWindow::Open(uw, d.pluginName, title, d.params, d.state, tid, fx,
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
    // No unit suffix: it pushed the true-peak figure out of the box, and TP is
    // dB by definition.
    std::snprintf(buf, sizeof(buf), "%s  %s  %s", m, s, tp);
    fLoudView->SetText(buf);
}

// --- the Preferences window (M3.5) -----------------------------------------
//
// One window, like the mixer and the markers window: asking again brings the
// open one forward with the current values. The window owns none of the state
// -- these five methods are the only place a preference is read or written.

// What the window shows, from wherever the field actually lives: the
// transport's own prefs in the controllers, the rest in the members beside
// fPrefsMsgr.
PrefsView MainWindow::CurrentPrefs() const {
    PrefsView v;
    v.bufferFrames      = (int)fTransportCtl.fBufferFrames;
    v.countInBars       = fRecCtl.fCountInBars;
    v.metronomeSound    = fTransportCtl.fMetronomeSound;
    v.metronomeLevel    = fTransportCtl.fMetronomeGain;
    v.monitorInput      = fTransportCtl.fMonitorInput;
    v.previewNotes      = fRollPreview;
    v.darkMode          = ActiveThemeMode() == ThemeMode::Dark;
    v.defaultSampleRate = fPrefsSampleRate;
    v.uiScale           = fPrefsUiScale;
    v.projectDir        = fPrefsProjectDir;
    v.autosaveSeconds   = fPrefsAutosaveSeconds;
    return v;
}

void MainWindow::PushPrefsToWindow() {
    if (!fPrefsMsgr.IsValid()) return;
    const PrefsView v = CurrentPrefs();
    BMessage m(kMsgPrefsValues);
    m.AddInt32("buffer", v.bufferFrames);
    m.AddInt32("defrate", v.defaultSampleRate);
    m.AddInt32("countin", v.countInBars);
    m.AddInt32("metsnd", v.metronomeSound);
    m.AddFloat("metlvl", v.metronomeLevel);
    m.AddBool("monin", v.monitorInput);
    m.AddBool("preview", v.previewNotes);
    m.AddBool("dark", v.darkMode);
    m.AddFloat("uiscale", v.uiScale);
    m.AddString("projdir", v.projectDir.c_str());
    m.AddInt32("autosv", v.autosaveSeconds);
    fPrefsMsgr.SendMessage(&m);
}

void MainWindow::ShowPreferencesWindow() {
    if (fPrefsMsgr.IsValid()) {
        PushPrefsToWindow();
        BMessage act(kMsgPrefsActivate);
        fPrefsMsgr.SendMessage(&act);
        return;
    }
    PreferencesWindow* win = new PreferencesWindow(CurrentPrefs(),
                                                   BMessenger(this));
    fPrefsMsgr = BMessenger(win);
    win->Show();
}

// The one funnel every preference goes through: the window's message and the
// Audio menu's remaining ids both land here, so the two cannot disagree about
// what changing a preference does.
void MainWindow::ApplyPrefs(const PrefsView& v) {
    // Buffer size: applied at the next Play (the engine is rebuilt then, which
    // is also when a changed click sound or level reaches it).
    fTransportCtl.fBufferFrames = (size_t)(v.bufferFrames > 0 ? v.bufferFrames
                                                              : 512);
    fRecCtl.fCountInBars = v.countInBars;

    // Input monitoring: live, exactly as the Audio menu's toggle did it --
    // the timeline's "I" lamp, a running take's recorder, and the engine.
    if (fTransportCtl.fMonitorInput != v.monitorInput) {
        fTransportCtl.fMonitorInput = v.monitorInput;
        if (fTimeline) fTimeline->SetMonitorInput(fTransportCtl.fMonitorInput);
        if (fRecCtl.fRecorder)
            fRecCtl.fRecorder->SetMonitor(fTransportCtl.fMonitorInput);
        if (fTransportCtl.fEngine)
            fTransportCtl.fEngine->SetInputMonitor(fTransportCtl.fMonitorInput);
    }

    // The MIDI editor's audition (package 51): a preference, not transport
    // state -- turning it off silences the next click (and stops the engine
    // the clicks had raised, if it was theirs).
    fRollPreview = v.previewNotes;
    if (!fRollPreview) StopAuditionEngine();

    // The metronome's sound and level: live on the running engine (a stopped
    // one gets them from EnsureEngine's next build).
    fTransportCtl.fMetronomeSound = v.metronomeSound;
    fTransportCtl.fMetronomeGain  = v.metronomeLevel;
    if (fTransportCtl.fEngine) {
        fTransportCtl.fEngine->SetMetronomeSound(v.metronomeSound);
        fTransportCtl.fEngine->SetMetronomeGain(v.metronomeLevel);
    }

    fPrefsSampleRate = v.defaultSampleRate;

    // The look: SetThemeMode applies it everywhere and saves it itself.
    const bool darkNow = ActiveThemeMode() == ThemeMode::Dark;
    if (darkNow != v.darkMode)
        SetThemeMode(v.darkMode ? ThemeMode::Dark : ThemeMode::System);

    // The UI scale (0 = the user's font size). Every window re-takes its
    // geometry colours and redraws at the new scale; a window's frame and its
    // split positions stay where the user put them (a restart lays everything
    // out at the new scale).
    if (fPrefsUiScale != v.uiScale) {
        fPrefsUiScale = v.uiScale;
        SetThemeScaleOverride(v.uiScale);
        ApplyThemeToAllWindows();
        if (fTimeline) fTimeline->Invalidate();
    }

    fPrefsProjectDir = v.projectDir;

    if (fPrefsAutosaveSeconds != v.autosaveSeconds) {
        fPrefsAutosaveSeconds = v.autosaveSeconds;
        StartAutosave(fPrefsAutosaveSeconds);
    }

    SaveSettings();          // a preference survives the quit
    UpdateTitle();           // nothing here dirties the project, but the title
                             // is cheap and keeps the poll honest
    // Tell the window what the app actually took (it re-lays itself out at the
    // new scale on this push), so the controls never show a value the app did
    // not apply.
    PushPrefsToWindow();
}

// --- the Project Settings window (M3.6) -------------------------------------

// What the project IS, as the window shows it. The meter comes from the tempo
// map's frame-0 change (the authoritative copy the engine and the ruler read;
// SetMeterCommand keeps the project's own timeSig equal to it).
ProjectSettingsView MainWindow::CurrentProjectSettings() const {
    ProjectSettingsView v;
    v.sampleRate = fProject->sampleRate;
    v.tempoBPM   = fProject->tempoBPM;
    const MeterChange& m0 = fProject->tempoMap.Meters().front();
    v.meterNum   = m0.num;
    v.meterDenom = m0.denom;
    v.notes      = fProject->notes;
    for (const Track& t : fProject->Tracks())
        for (const Clip& c : t.clips)
            if (!c.sourcePath.empty()) v.hasMedia = true;
    return v;
}

void MainWindow::PushProjectSettingsToWindow() {
    if (!fProjSettingsMsgr.IsValid()) return;
    const ProjectSettingsView v = CurrentProjectSettings();
    BMessage m(kMsgProjSettingsValues);
    m.AddDouble("rate", v.sampleRate);
    m.AddDouble("tempo", v.tempoBPM);
    m.AddInt32("mnum", v.meterNum);
    m.AddInt32("mdenom", v.meterDenom);
    m.AddString("notes", v.notes.c_str());
    m.AddBool("hasmedia", v.hasMedia);
    fProjSettingsMsgr.SendMessage(&m);
}

void MainWindow::ShowProjectSettingsWindow() {
    if (fProjSettingsMsgr.IsValid()) {
        PushProjectSettingsToWindow();
        BMessage act(kMsgProjSettingsActivate);
        fProjSettingsMsgr.SendMessage(&act);
        return;
    }
    ProjectSettingsWindow* win =
        new ProjectSettingsWindow(CurrentProjectSettings(), BMessenger(this));
    fProjSettingsMsgr = BMessenger(win);
    win->Show();
}

// The differences become ONE macro command, applied and undone as a unit: a
// change to the rate AND the tempo AND the notes is one "Project Settings"
// step in the Edit menu, not four.
void MainWindow::ApplyProjectSettings(const ProjectSettingsView& values) {
    const ProjectSettingsView cur = CurrentProjectSettings();
    auto macro = std::make_unique<MacroCommand>("Project Settings");
    bool any = false;

    const bool rateChanged = values.sampleRate > 0.0
        && std::fabs(values.sampleRate - cur.sampleRate) >= 0.5;
    if (rateChanged) {
        // First: it rescales the timeline the other edits are positioned in.
        macro->Add(std::make_unique<SetSampleRateCommand>(
            values.sampleRate, values.resampleMedia));
        any = true;
    }
    if (std::fabs(values.tempoBPM - cur.tempoBPM) > 1e-6) {
        double bpm = values.tempoBPM;
        if (bpm < 20.0)  bpm = 20.0;
        if (bpm > 300.0) bpm = 300.0;
        macro->Add(std::make_unique<SetTempoCommand>(bpm));
        any = true;
    }
    if (values.meterNum != cur.meterNum || values.meterDenom != cur.meterDenom) {
        macro->Add(std::make_unique<SetMeterCommand>(values.meterNum,
                                                     values.meterDenom));
        any = true;
    }
    if (values.notes != cur.notes) {
        macro->Add(std::make_unique<SetProjectNotesCommand>(values.notes));
        any = true;
    }
    if (!any) return;              // Apply with nothing changed: a no-op, and
                                   // not an empty undo step

    if (!fStack->Execute(std::move(macro), *fProject)) return;

    // The window's other views follow the model: the transport's tempo field,
    // the ruler's grid, the waveforms (a rate change can repoint every clip at
    // a converted file) and the running engine.
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f", fProject->tempoBPM);
    if (fTempo != nullptr) fTempo->SetText(buf);
    if (rateChanged) RebuildPeaks();
    if (fTimeline != nullptr) fTimeline->Invalidate();
    fTransportCtl.ReloadActiveEngine();
    UpdateEditMenu();
    UpdateTitle();
    PushProjectSettingsToWindow();   // the window shows what was actually applied
}

// The crash-recovery runner at the chosen interval; 0 = no runner at all.
void MainWindow::StartAutosave(int seconds) {
    delete fAutosave;
    fAutosave = nullptr;
    if (seconds <= 0) return;
    fAutosave = new BMessageRunner(BMessenger(this), new BMessage(MSG_AUTOSAVE),
                                   (bigtime_t)seconds * 1000000LL);
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
