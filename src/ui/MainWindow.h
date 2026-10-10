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
#include "../model/Commands.h"   // RelinkEntry (the Locate… walk)
#include "../engine/Engine.h"
#include "../engine/Recorder.h"
#include "../midi/MidiPort.h"
#include "../midi/MidiRecorder.h"
#include "../plugin/FxWatchTable.h"   // the open editors + what a chain edit means
#include "ExportWindow.h"             // ExportChoices (the export dialog's fields)
#include "ProjectDocument.h"          // the project's file, recovery, recent list
#include "Theme.h"                    // ThemeAware + the tokens (T1)
#include "RecordController.h"         // the record state + the recorder
#include "RenderJobs.h"              // export/freeze/region rendering
#include "TransportController.h"      // the engine + transport state

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
constexpr uint32 MSG_EXPORT           = 'expt';
constexpr uint32 MSG_EXPORT_REF       = 'exrf';   // from the export file panel
constexpr uint32 MSG_EXPORT_STEMS     = 'stem';
constexpr uint32 MSG_EXPORT_STEMS_REF = 'stmr';
// View > Dark Mode: flip the look between the user's own colours and the
// DAW's dark palette (T1). Also what the functional test posts.
constexpr uint32 MSG_THEME_MODE       = 'thmm';
// The dock's segmented control -> the window (T2): int32 "index" is the page
// to show (0 Editor, 1 Samples, 2 Plugins).
constexpr uint32 MSG_DOCK_PAGE        = 'dkpg';
// MainWindow -> itself, a moment after a file panel is shown: the panel builds
// its views after Show() returns, so the themed colours go on a second time.
constexpr uint32 MSG_THEME_STOCK      = 'thst';

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
    // Keys belong to the focused view -- except that the timeline only takes
    // focus on a click, so after anyone touched a text field the transport
    // keys (Space, arrows, Home) went nowhere. Route a key with no command
    // modifier to the timeline whenever the focus is not a text field; Cmd
    // shortcuts keep flowing to the menus.
    void DispatchMessage(BMessage* message, BHandler* handler) override;
    bool QuitRequested() override;   // quit the app when the window closes
    // For the functional tests: whether the transport is rolling.
    bool IsPlaying() const { return fTransportCtl.fPlaying; }

private:
    // Feed one live event to every armed track whose route accepts it.
    void SyncFxToEngine();           // a committed chain edit -> the running engine
    bool AudioMonitorOn() const;     // global flag OR an armed audio track's I btn
    void UpdatePulse();              // run the poll iff playing or recording
    void UpdateTimeReadout(Frame playhead);
    void UpdateLoudnessReadout(float momLufs, float shortLufs, float truePeakDb);
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
    // File > New: ask, then a fresh empty project at the session's rate.
    void NewProject();
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

    // Clip region ops (decode-backed): normalize to unity peak (via clip gain),
    // reverse (render a new file), strip silence (split into clips). Each finds
    // the clip, does any file work, and issues the resulting command(s).
    void RegionNormalize(TrackId track, ClipId clip);
    void RegionReverse(TrackId track, ClipId clip);
    void RegionStripSilence(TrackId track, ClipId clip);
    void FreezeTrack(TrackId track, bool freeze);   // render-to-audio / restore
    // Decode a clip's played region to interleaved-stereo float at the source
    // rate. Returns frames decoded (0 on failure); sets outRate to the file rate.
    int64_t DecodeClipRegion(const Clip& c, std::vector<float>& out,
                             double& outRate) const;
    // A unique path in the take/working dir for a rendered region/freeze file.

    Project*        fProject;        // non-owning (the session)
    CommandStack*   fStack;          // non-owning
    PeakMap*        fPeaks;          // non-owning; new takes add entries here

    // The window's panes (M1.4): the inspector/timeline split, and the docked
    // bottom pane (the editor and the browsers) under it. Both are BSplitViews,
    // so the user resizes them and the sizes persist in AppSettings.
    BMenuItem*      fThemeItem     = nullptr;   // View > Dark Mode (T1)
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
    // follows the selection like the inspector does.
    TrackId          fSelTrack     = kInvalidTrackId;
    void OpenDockedEditor(TrackId track, ClipId clip);
    void ClearDockedEditor();                // back to the empty hint
    void ShowReplacedProject(Frame playhead);   // after Open / New
    void PopOutEditor();                     // dock -> its own window

    TimelineView*   fTimeline;
    InspectorView*  fInspector = nullptr;   // left track-inspector column
    class TransportBar* fTransport = nullptr;
    BStringView*    fTimeView;
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
    BMessageRunner*           fPulse = nullptr;  // 60 Hz UI poll
    BMessageRunner*           fAutosave = nullptr;  // periodic crash-recovery save
    // The dirty marker has to follow edits made anywhere (the timeline and the
    // inspector execute commands directly, with no message to the window), and
    // the 60 Hz pulse only runs while the transport does — so it gets its own
    // slow runner. UpdateTitle() is also called directly on save/load/undo.
    BMessageRunner*           fTitlePoll = nullptr; // 2 Hz dirty-marker poll
    BMessenger                fMixerMsgr;    // open mixer window (for live peaks)
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
    BMenuItem*                fMetItem = nullptr;   // metronome toggle
    BMenuItem*                fDimItem = nullptr;   // monitor dim toggle
    BMenuItem*                fMonoItem = nullptr;  // monitor mono toggle
    BMenu*                    fBufMenu = nullptr;   // buffer-size submenu (for marks)
    BMenu*                    fCountInMenu = nullptr; // radio submenu (for marks)
    BMenuItem*                fMonInItem = nullptr;   // input-monitor toggle
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
};

} // namespace daw
