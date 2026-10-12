// MainWindow — top-level DAW window.
//
// Layout: a transport bar (play/stop + time readout) across the top, the
// timeline view filling the rest. Owns the playback Engine: Play builds it
// from the current Project and starts it (rebuild-on-play — RT-safe, matches
// how the engine already loads a session); a BMessageRunner polls the
// engine's atomic playhead ~60 Hz and drives the timeline's playhead line.
//
// Track-header controls + command/undo wiring arrive in M4e.
#pragma once

#include "../model/Project.h"
#include "../model/PeakCache.h"
#include "PeakLoader.h"
#include "../model/Commands.h"   // RelinkEntry (the Locate… walk)
#include "../engine/Engine.h"
#include "../engine/Recorder.h"
#include "../midi/MidiPort.h"
#include "../midi/MidiRecorder.h"
#include "../plugin/FxWatchTable.h"   // the open editors + what a chain edit means
#include "ExportWindow.h"             // ExportChoices (the export dialog's fields)
#include "PreferencesWindow.h"        // PrefsView (M3.5)
#include "ProjectSettingsWindow.h"    // ProjectSettingsView (M3.6)
#include "NewProjectWindow.h"         // TemplateInfo rows (M3.7)
#include "ProjectDocument.h"          // the project's file, recovery, recent list
#include "Theme.h"                    // ThemeAware + the tokens (T1)
#include "RecordController.h"         // the record state + the recorder
#include "RenderJobs.h"              // export/freeze/region rendering
#include "TransportController.h"      // the engine + transport state

#include "../model/TimeDisplay.h"      // the transport's time formats (M3.1)

#include <Messenger.h>
#include <Window.h>

#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include <utility>
#include <string>
#include <vector>
#include <cstdint>

class BButton;
class BStringView;
class BMessageRunner;
class BFilePanel;
class BSlider;
class BMenuItem;
class BTextControl;
class BMenu;

class BSplitView;   // Haiku classes live in the global namespace
class BGroupView;
class BView;

namespace daw {

class TimelineView;
class InspectorView;
class DawSegments;
class SampleBrowserView;
class PluginBrowserView;
class PianoRollView;
class MeterView;
class DawButton;

// Message ids that are part of the app's wiring rather than MainWindow's
// private business: the menus, the file panels and the pulse post them, and so
// do the functional tests (tests/ui_functional_tests.cpp), which drive the real
// window by posting exactly what a widget posts. The rest of the ids stay
// private to MainWindow.cpp.
constexpr uint32 MSG_ABOUT            = 'abot';   // Help > About
constexpr uint32 MSG_PULSE            = 'puls';   // the 60 Hz BMessageRunner
constexpr uint32 MSG_NEW_AUDIO        = 'naud';
constexpr uint32 MSG_NEW_MIDI         = 'nmid';
constexpr uint32 MSG_NEW_PROJECT      = 'nprj';   // File > New
constexpr uint32 MSG_SAVE             = 'save';   // File > Save (silent once pathed)
constexpr uint32 MSG_SAVE_AS          = 'svas';   // File > Save As (always a panel)
constexpr uint32 MSG_SAVE_REF         = 'svrf';   // from the save file panel
constexpr uint32 MSG_OPEN_REF         = 'oprf';   // from the open file panel
constexpr uint32 MSG_RELINK_REF       = 'rlkr';   // from the Locate… walk's panel
// The full Edit menu (M2.4). The timeline owns the range and the clipboard, so
// the window routes these to it. Public ids: the functional tests post exactly
// what the menu posts.
constexpr uint32 MSG_EDIT_UNDO        = 'undo';
constexpr uint32 MSG_EDIT_REDO        = 'redo';
constexpr uint32 MSG_EDIT_CUT         = 'ecut';
constexpr uint32 MSG_EDIT_COPY        = 'ecpy';
constexpr uint32 MSG_EDIT_PASTE       = 'epst';
constexpr uint32 MSG_EDIT_DELETE      = 'edel';
constexpr uint32 MSG_EDIT_SELECT_ALL  = 'esel';
constexpr uint32 MSG_EDIT_DUPLICATE   = 'edup';
constexpr uint32 MSG_EDIT_INSERT_SIL  = 'eins';
constexpr uint32 MSG_EXPORT           = 'expt';
constexpr uint32 MSG_EXPORT_REF       = 'exrf';   // from the export file panel
constexpr uint32 MSG_EXPORT_STEMS     = 'stem';
constexpr uint32 MSG_EXPORT_STEMS_REF = 'stmr';
// View > Dark Mode: flip the look between the user's own colours and the
// DAW's dark palette (T1). Also what the functional test posts.
constexpr uint32 MSG_THEME_MODE       = 'thmm';
// View > Markers (M2.7): the markers/locations list window, single-instance.
constexpr uint32 MSG_MARKERS          = 'mkrs';
// View > Marker Lane: the flag strip between the ruler and the lanes.
constexpr uint32 MSG_MARKER_LANE      = 'mkln';
// Track > Duplicate Track (the whole track, with fresh clip ids).
constexpr uint32 MSG_DUPLICATE_TRACK  = 'dptr';
// The dock's segmented control -> the window (T2): int32 "index" is the page
// to show (0 Editor, 1 Samples, 2 Plugins).
constexpr uint32 MSG_DOCK_PAGE        = 'dkpg';
// MainWindow -> itself, a moment after a file panel is shown: the panel builds
// its views after Show() returns, so the themed colours go on a second time.
constexpr uint32 MSG_THEME_STOCK      = 'thst';
// --- M3.5, added here so this block stays the end of the file --------------
// File > Preferences…: the Preferences window, single-instance. What the
// window itself posts back (kMsgPrefsChanged and friends) is in
// PreferencesWindow.h — this is only the id that opens it, and what the
// functional tests post.
constexpr uint32 MSG_PREFERENCES      = 'prfs';
// File > Project Settings… (M3.6): sample rate, tempo, meter and notes.
constexpr uint32 MSG_PROJECT_SETTINGS = 'prjs';

class MainWindow : public BWindow, public ThemeAware {
public:
    // A mode switch re-takes every colour this window cached (the window's own
    // background and the transport bar's readouts); the walker invalidates the
    // rest, which draw from the tokens (T1).
    void ApplyTheme() override;
    // Switch the whole process — tokens, control look and every open window —
    // and remember it. The View menu and the functional test both land here.
    void SetThemeMode(ThemeMode mode);

    // The transport's method moves (M1.1) live in TransportController; it
    // reaches the widgets and the model through the window it is given.
    friend class TransportController;
    friend class RecordController;
    friend class RenderJobs;
    // Like TimelineView: BWindow::Frame() would shadow the model's frame type
    // for every unqualified `Frame` in this class. A member typedef hides it.
    using Frame = daw::Frame;

    using PeakMap = std::map<std::string, PeakCache>;

    // The transport bar's controls are pinned offsets, not a flow, so the
    // window fits them to the bar's new width itself (see LayoutTransportBar).
    void FrameResized(float newWidth, float newHeight) override;
    MainWindow(BRect frame, Project* project, CommandStack* stack,
               PeakMap* peaks);
    ~MainWindow() override;

    void MessageReceived(BMessage* msg) override;
    // The Edit menu's Undo/Redo labels come from the stack (M2.4):
    // "Undo Move Clip" / "Redo Split at Playhead". Called after every undoable
    // path the window itself runs and from the 2 Hz title poll, which covers
    // the edits the timeline makes without a message to the window.
    void UpdateEditMenu();
    // Keys belong to the focused view -- except that the timeline only takes
    // focus on a click, so after anyone touched a text field the transport
    // keys (Space, arrows, Home) went nowhere. Route a key with no command
    // modifier to the timeline whenever the focus is not a text field; Cmd
    // shortcuts keep flowing to the menus.
    void DispatchMessage(BMessage* message, BHandler* handler) override;
    bool QuitRequested() override;   // quit the app when the window closes
    // For the functional tests: whether the transport is rolling, and the two
    // transport states the keyboard map toggles (M3.2).
    bool IsPlaying() const { return fTransportCtl.fPlaying; }
    // For the functional tests (M4.1): what the engine has done, so a test can
    // prove that a rebuild during playback swapped the graph WITHOUT re-creating
    // the BSoundPlayer (the counters must not move) and without stopping the
    // transport. All read the engine's atomics; 0 before one exists.
    uint64_t EngineGraphsPublished() const {
        return fTransportCtl.fEngine
                   ? fTransportCtl.fEngine->GraphsPublished() : 0;
    }
    uint64_t EnginePlayersOpened() const {
        return fTransportCtl.fEngine
                   ? fTransportCtl.fEngine->PlayersOpened() : 0;
    }
    uint64_t EnginePlayerStarts() const {
        return fTransportCtl.fEngine
                   ? fTransportCtl.fEngine->PlayerStarts() : 0;
    }
    // Waveform envelopes (M4.4), for the tests: how many the loader has handed
    // over, and whether it is still working.
    int PeaksLoaded() const {
        int n = 0;
        for (const auto& kv : *fPeaks) if (kv.second.IsValid()) n++;
        return n;
    }
    int PeakCacheHits()   const { return fPeakLoader.CacheHits(); }
    int PeakCacheMisses() const { return fPeakLoader.CacheMisses(); }
    bool PeaksLoading()   const { return fPeakLoader.Busy(); }
    // What the last pass did with one path (PeakLoader::Outcome).
    int PeakOutcome(const std::string& path) const {
        return fPeakLoader.OutcomeOf(path);
    }
    // Re-run the waveform pass the way a project load does (the tests cannot ask
    // for a file panel mid-suite).
    void RebuildPeaksForTest() { RebuildPeaks(); }

    // The master output peak of the last rendered block (the audio-out guard).
    float MasterPeakL() const {
        return fTransportCtl.fEngine ? fTransportCtl.fEngine->PeakL() : 0.0f;
    }
    // The last graph build's own duration (M4.3): the plan's audio-start budget
    // is about this, not about the window's message latency.
    bigtime_t EngineBuildUs() const {
        return fTransportCtl.fEngine
                   ? fTransportCtl.fEngine->LastBuildUs() : 0;
    }
    // How many times the transport has crossed a loop end (M4.2). A seamless
    // loop is exactly this counter moving while the three above do not.
    uint64_t EngineWraps() const {
        return fTransportCtl.fEngine
                   ? fTransportCtl.fEngine->Wraps() : 0;
    }
    // Is a graph in place for the RT side (i.e. has the last rebuild landed)?
    bool EngineGraphReady() const {
        return fTransportCtl.fEngine && fTransportCtl.fEngine->HasGraph();
    }
    bool EngineLoadPending() const {
        return fTransportCtl.fEngine && fTransportCtl.fEngine->GraphPending();
    }
    Frame EnginePlayhead() const {
        return fTransportCtl.fEngine ? fTransportCtl.fEngine->Playhead() : 0;
    }
    bool IsRecording() const { return fRecCtl.fRecMode; }
    bool MetronomeOn() const { return fTransportCtl.fMetronome; }
    // The Preferences window's effect on the transport, for the functional
    // test (both are private state the window's message changes; M3.5).
    int BufferFramesForTest() const { return (int)fTransportCtl.fBufferFrames; }
    int CountInBarsForTest()  const { return fRecCtl.fCountInBars; }
    // For the functional tests: re-run the settings load, which a restart would
    // do. The suite keeps ONE window for the whole run (closing it quits the
    // app), so the restore path is otherwise unreachable from a test.
    void ReloadSettings() { LoadSettings(); }
    // The transport bar's own state (M3.1), for the functional tests: which
    // format the big display is showing, and the two text lines it draws.
    int TransportTimeMode() const { return (int)fTimeMode; }
    // The bar view itself, so a test clicks a control where the BAR says it is
    // (TransportButtonRect) instead of computing an offset (docs/HANDOFF.md).
    BView* TransportBarView() const;
    BRect TransportButtonRect(int which) const;   // daw::TransportButton

private:
    // Feed one live event to every armed track whose route accepts it.
    void SyncFxToEngine();           // a committed chain edit -> the running engine
    bool AudioMonitorOn() const;     // global flag OR an armed audio track's I btn
    void UpdatePulse();              // run the poll iff playing or recording
    void UpdateTimeReadout(Frame playhead);
    void UpdateLoudnessReadout(float momLufs, float shortLufs, float truePeakDb);
    // Engine (or recorder) meter readings -> the master meter (M3.3).
    void PushMeter(bool fromRecorder);
    void PushTrackPeaks();           // engine per-track peaks -> timeline meters
    void PushRollPlayhead(Frame ph); // push the playhead to an open piano roll
    void PushFxMeters();             // engine fx meters -> effects window
    void PushChainToFxWindow(TrackId tid);   // model's chain -> open panel
    void PushFxParams();             // engine insert values -> native editor
    void LayoutTransportBar();       // fit the bar's controls to its width
    void ValidateFxWatch();          // keep a live editor pointed at ITS insert
    void CloseFxEditors();           // project is going away: close them all
    void FlushFxEditors();           // before a save: commit what they wrote
    // Save the insert's current patch as a named preset (the panel's
    // "Save Preset..."). Captures the state the way a save does, then writes a
    // preset file; `fx` is an index into the track's (or master's) chain.
    void SaveFxPreset(TrackId tid, int fx, const char* name);
    bool SaveTo(const char* path);   // false = the file was not written
    void LoadFrom(const char* path, bool asRecovery = false);
    // File > New (M3.7): the New Project dialog (Empty or a template), then
    // the unsaved-changes prompt, then the new session. `templatePath` empty
    // means the built-in Empty.
    void ShowNewProjectWindow();
    void NewProjectFrom(const std::string& templateId,
                        const std::string& templatePath);
    void RememberProject(const std::string& path);  // recent list + menu
    // After a load: one dialog for media that is gone, then either silence
    // (Skip) or a walk to find each file (Locate…), applied as one command.
    void CollectMissingMedia();
    void StartRelinkWalk(std::vector<RelinkEntry> missing);
    void RelinkNext();
    void FinishRelink();
    void ForgetRecent(const std::string& path);     // ... a file that vanished
    void RebuildRecentMenu();
    // Ask about unsaved changes before an action that would drop them (Quit,
    // Open, New). True = the caller may proceed. Save flushes the editors
    // first; with no path yet the save panel opens and the caller is refused.
    bool ConfirmDiscardChanges();
    // One visible report for a failure the user has to know about: an
    // asynchronous alert, so a report can never hold the window thread.
    void ReportError(const char* title, const std::string& detail);
    void UpdateTitle();              // "*name — Haiku DAW" while dirty
    // Decode the loaded project's soundfonts into the SoundfontCache before
    // the engine is built (it only ever looks them up). Warns about misses.
    void PrimeSoundfonts();
    void LoadSettings();             // ~/config/settings/HaikuDAW/settings
    void SaveSettings();
    void ImportAudio(const char* path);   // add a WAV as a clip (first track/playhead)
    void ImportAudioAt(const char* path, TrackId track, Frame start);  // drop target
    // .mid -> new MIDI tracks (tempo-relative). `at` places the created regions
    // on the timeline (a drop position); 0 = the start of the project.
    void ImportMidi(const char* path, Frame at = 0);
    void ExportMidi(const char* path);    // project MIDI tracks -> .mid
    void RebuildPeaks();             // rebuild waveform envelopes after load

    // (The clip region ops -- normalize, reverse, strip silence -- and Freeze
    // are RenderJobs' worker jobs now; see src/ui/RenderJobs.h. Their bodies,
    // including the isolated-track render, live in the kit-free
    // src/model/RegionJobs.cpp.)

    Project*        fProject;        // non-owning (the session)
    CommandStack*   fStack;          // non-owning
    PeakMap*        fPeaks;          // non-owning; new takes add entries here
    PeakLoader      fPeakLoader;     // waveform envelopes, off this thread (M4.4)

    // The window's panes (M1.4): the inspector/timeline split, and the docked
    // bottom pane (the editor and the browsers) under it. Both are BSplitViews,
    // so the user resizes them and the sizes persist in AppSettings.
    BMenuItem*      fUndoItem      = nullptr;   // Edit > Undo  (its label from
    BMenuItem*      fRedoItem      = nullptr;   // ... UndoName/RedoName, M2.4)
    BMenuItem*      fCopyItem      = nullptr;   // Edit > Copy (nothing to copy)
    BMenuItem*      fCutItem       = nullptr;
    BMenuItem*      fDeleteItem    = nullptr;
    BMenuItem*      fPasteItem     = nullptr;
    BMenuItem*      fSelectAllItem = nullptr;
    BMenuItem*      fDupItem       = nullptr;
    BMenuItem*      fSilenceItem   = nullptr;   // Edit > Insert Silence (range)
    BMenuItem*      fMarkerLaneItem = nullptr;  // View > Marker Lane (M2.7)
    BMenuItem*      fDupTrackItem  = nullptr;   // Track > Duplicate Track (M2.6)
    BMenuItem*      fInspectorItem = nullptr;   // View > Inspector (I)
    BMenuItem*      fDockItem      = nullptr;   // View > Editor & Browsers (J)
    BSplitView*     fPaneSplit = nullptr;   // inspector | timeline
    BSplitView*     fRootSplit = nullptr;   // the panes over the dock
    BView*          fDock      = nullptr;   // header strip over the body
    BGroupView*     fEditorPane = nullptr;   // the dock's body: roll or hint
    BStringView*    fDockTitle  = nullptr;   // what the dock is showing
    BView*          fDockEmpty  = nullptr;   // the hint while nothing is open
    BStringView*    fDockHint   = nullptr;   // its text (re-coloured on a mode switch)
    DawButton*      fDockPop    = nullptr;   // Pop out (only with a region)
    PianoRollView*  fDockRoll   = nullptr;   // the docked MIDI editor, or null
    TrackId         fDockTrack  = kInvalidTrackId;
    ClipId          fDockClip   = kInvalidClipId;
    bool            fInspectorShown = true;
    void SetInspectorShown(bool shown);
    void SetDockShown(bool shown);
    // The dock's pages (T2). The body holds ONE of them: the piano roll (or its
    // hint), a sample browser, or the plugin browser. Switching shows the dock,
    // so "add an effect" and "browse samples" land where the content is.
    void SetDockPage(int page);
    void PopOutDockPage();   // the showing page becomes its own window
    // The markers window (M2.7): show and activate the one that exists, or
    // make it — and push it the current list. Every marker edit goes through
    // PushMarkers (the kMsgUiRefresh handler calls it), so the window is never
    // stale.
    void ShowMarkersWindow();
    void PushMarkers();
    // The effects window is single-instance (T2): this shows and activates the
    // one that exists, retargeted at `track`/`focusSlot`, or creates it. Every
    // "edit this chain" path goes through here so there is one window and one
    // place that decides.
    void ShowFxWindow(TrackId track, int focusSlot = -1);
    enum { kDockEditor = 0, kDockSamples = 1, kDockPlugins = 2 };
    // Show a browser page, pointed at `track` when it is a plugin browser (the
    // chain a choice will be added to). kInvalidTrackId leaves it as it is.
    void ShowDockBrowser(int page, TrackId track = kInvalidTrackId);
    int              fDockPage     = kDockEditor;
    DawSegments*     fDockSegments = nullptr;
    SampleBrowserView* fDockSamples = nullptr;
    PluginBrowserView* fDockPlugins = nullptr;
    // The track the inspector is pointed at: the plugin page adds to it, so it
    // follows the selection like the inspector does — through the same call,
    // so the two cannot drift apart (T2 review).
    TrackId          fSelTrack     = kInvalidTrackId;
    void SelectTrack(TrackId tid);   // inspector + the plugin page follow
    void OpenDockedEditor(TrackId track, ClipId clip);
    void ClearDockedEditor();                // back to the empty hint
    void ShowReplacedProject(Frame playhead);   // after Open / New
    void PopOutEditor();                     // dock -> its own window

    TimelineView*   fTimeline;
    InspectorView*  fInspector = nullptr;   // left track-inspector column
    class TransportBar* fTransport = nullptr;
    BStringView*    fLoudView = nullptr;   // LUFS / true-peak readout
    BStringView*    fVolLbl = nullptr;     // labels + readouts the bar hides
    BStringView*    fBpmLbl = nullptr;     // when there is no room for them
    // Whether each is currently shown, so the layout only calls Show()/Hide()
    // on a transition (both are counted, not idempotent, and the layout runs on
    // every resize).
    bool fLoudShown = true;
    bool fBpmShown  = true;
    bool fVolShown  = true;
    MeterView*      fMeter;
    BSlider*        fMaster;
    BTextControl*   fTempo;

    // The record state (and the recorder) lives in RecordController, declared
    // BEFORE the transport controller: the engine must be destroyed first.
    RecordController           fRecCtl;
    TransportController       fTransportCtl;  // the engine + transport state (M1.1)
    // MIDI capture: a consumer connected to the armed MIDI tracks' input
    // endpoints, feeding a note-pairing recorder. Independent of the audio path.
    // One recorder per armed MIDI track, not one shared: inputs are demuxed, so
    // each track pairs only the events its own route accepts. With a single
    // keyboard every track's route is permissive and they all capture the same
    // stream, exactly as before.
    // Endpoint id + channel each MIDI track listens to, resolved from the
    // track's endpoint NAME when the input is opened (see ResolveMidiRoutes).
    BMessenger                fKeysMsgr;    // the open Keyboard Shortcuts window
    BMessageRunner*           fPulse = nullptr;  // 60 Hz UI poll
    BMessageRunner*           fAutosave = nullptr;  // periodic crash-recovery save
    // The dirty marker has to follow edits made anywhere (the timeline and the
    // inspector execute commands directly, with no message to the window), and
    // the 60 Hz pulse only runs while the transport does — so it gets its own
    // slow runner. UpdateTitle() is also called directly on save/load/undo.
    BMessageRunner*           fTitlePoll = nullptr; // 2 Hz dirty-marker poll
    BMessenger                fMixerMsgr;    // open mixer window (for live peaks)
    BMessenger                fMarkersMsgr;  // open markers window (the list push)
    BMessenger                fRollMsgr;     // last-opened piano roll (playhead)
    BMessenger                fFxMsgr;       // open effects window (for live meters)
    TrackId                   fFxTrack = kInvalidTrackId;  // its track (~0 master)
    // An open native LV2 editor watching one insert (engine -> editor values).
    // The watch lives in the engine, addressed by (track, master, fx), and is
    // cleared both by the editor saying so and by its messenger dying.
    // One open native editor and the insert it is showing (see
    // src/plugin/FxWatchTable.h -- the table, its rules and its slot allocation
    // are kit-free and host-tested, because those rules are where the bugs
    // were). Each entry owns one engine watch slot; the editor is fed on the
    // 60 Hz pulse.
    struct EditorHandle;
    struct FxEntry {
        std::shared_ptr<EditorHandle> editor;
        std::string uri;
        TrackId     track = kInvalidTrackId;  // chain address (master sentinel ok)
        int         fx = -1;
        int         slot = -1;                // engine watch slot
        uint32_t    gen = 0;                  // last frame pushed to it
        // Last frame pushed from the MODEL, for the case with no engine at all
        // (nothing played yet): then the model is the only source of truth.
        std::vector<float> pushed;
    };
    std::vector<FxEntry> fFxWatches;
    std::vector<FxWatch> FxWatchSnapshot() const;   // for the kit-free rules
    void PushFxParamsFromModel(FxEntry& w);   // ... or the model, with no engine
    FxChainView FxChainFor(const FxWatch& w) const; // the chain one entry names

    void ApplyFxWatchToEngine(const FxEntry& w); // program the engine for one
    void ReapplyFxWatches();                     // after an engine rebuild
    void PublishFxParamsNow();                   // stopped transport: publish
    // Open a plugin's own editor for an insert, resolving it against the MODEL
    // (callers may hold a stale chain snapshot). False if it has no editor.
    bool OpenNativeEditor(TrackId tid, int fx);
    BFilePanel*               fSavePanel = nullptr;
    BFilePanel*               fOpenPanel = nullptr;
    BFilePanel*               fExportPanel = nullptr;
    BFilePanel*               fStemsPanel = nullptr;
    BFilePanel*               fImportPanel = nullptr;
    BFilePanel*               fMidiImportPanel = nullptr;
    BFilePanel*               fMidiExportPanel = nullptr;
    BMenuItem*                fMetItem = nullptr;   // Audio > Metronome toggle
    BMenuItem*                fDimItem = nullptr;   // monitor dim toggle
    BMenuItem*                fMonoItem = nullptr;  // monitor mono toggle
    // (Buffer size, count-in and input monitoring left the Audio menu for the
    // Preferences window in M3.5; the menu keeps the quick toggles.)
    BMenuItem*                fFollowItem = nullptr;  // follow/chase playhead toggle
    std::string               fLastDir;      // last Open/Save/Import directory
    ProjectDocument           fDoc;          // the file, recovery copy, recent list
    std::vector<RelinkEntry>  fRelinkQueue;     // Locate… walk (newPath filled as picked)
    BFilePanel*               fRelinkPanel = nullptr;
    BMenu*                    fRecentMenu = nullptr;
    std::string               fTitleShown;   // last title set (skip a redundant SetTitle)

    // RenderJobs carries the off-looper export machinery (M1.1): the snapshot,
    // the worker thread, the progress messenger and the cancel flag. The
    // dialog and its remembered choices stay here.
    RenderJobs                fRender;
    ExportChoices             fExportChoices;    // the dialog's last settings

    void OpenExportWindow(bool stems);            // the options dialog

    // --- the Preferences window (M3.5), grouped at the end ----------------
    // Show and activate the one that exists, or make it — the same
    // single-instance rule the mixer and the markers window follow.
    void ShowPreferencesWindow();
    void PushPrefsToWindow();      // the current state -> the open window
    // The state the window shows, taken from where each field actually lives
    // (the controllers for the transport's own, the members below for the rest).
    PrefsView CurrentPrefs() const;
    // One funnel for every way a preference arrives (the window's message and
    // the Audio menu's ids): applies, tells the engine, and persists.
    void ApplyPrefs(const PrefsView& values);
    // The crash-recovery runner at the chosen interval; 0 = no runner at all.
    void StartAutosave(int seconds);
    // --- the Project Settings window (M3.6), the same shape ----------------
    void ShowProjectSettingsWindow();
    void PushProjectSettingsToWindow();
    ProjectSettingsView CurrentProjectSettings() const;
    // The differences between `values` and the project become ONE undoable
    // command-stack edit ("Project Settings"), applied here.
    void ApplyProjectSettings(const ProjectSettingsView& values);
    BMessenger fProjSettingsMsgr;       // the open Project Settings window
    BMessenger fPrefsMsgr;              // the open Preferences window
    BMessenger fNewProjectMsgr;         // the open New Project dialog (M3.7)
    // The preferences nothing else owns: the rate a NEW project starts at, the
    // UI scale (0 = the user's font), the default project folder and the
    // autosave interval. Loaded/saved with the rest of AppSettings.
    int         fPrefsSampleRate    = 48000;
    float       fPrefsUiScale       = 0.0f;
    std::string fPrefsProjectDir;
    int         fPrefsAutosaveSeconds = 30;
    // ---- the transport bar's own state (M3.1) -----------------------------
    // The time display's format is the WINDOW's: the display view cycles it by
    // asking, and the window formats every position, so there is one copy of
    // "which format are we showing" and one place that can format it.
    TimeMode fTimeMode = TimeMode::Bbt;
    void UpdateTransportReadouts();  // all three, for one state change
    void UpdateTransportInfo();      // tempo/meter + DSP load + xruns (LCD)
    void UpdateTransportButtons();   // playing/recording/loop/click/punch lamps
    void UpdateTransportStatus();    // the state line, when the state changed
    void SetTransportStatus(const char* text);   // one message, until it changes
    void NudgePlayheadBar(int dir);  // rewind / fast-forward one bar
    std::string fStatusState;        // the last state string shown
    float       fDspPercent = 0.0f;  // the last measured callback peak load
};

} // namespace daw
