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
#include "../model/Command.h"
#include "../engine/Engine.h"
#include "../engine/Recorder.h"
#include "../midi/MidiPort.h"
#include "../midi/MidiRecorder.h"

#include <Messenger.h>
#include <Window.h>

#include <map>
#include <utility>
#include <memory>
#include <string>
#include <vector>

class BButton;
class BStringView;
class BMessageRunner;
class BFilePanel;
class BSlider;
class BMenuItem;
class BTextControl;
class BMenu;

namespace daw {

class TimelineView;
class InspectorView;
class MeterView;

class MainWindow : public BWindow {
public:
    // Like TimelineView: BWindow::Frame() would shadow the model's frame type
    // for every unqualified `Frame` in this class. A member typedef hides it.
    using Frame = daw::Frame;

    using PeakMap = std::map<std::string, PeakCache>;
    MainWindow(BRect frame, Project* project, CommandStack* stack,
               PeakMap* peaks);
    ~MainWindow() override;

    void MessageReceived(BMessage* msg) override;
    bool QuitRequested() override;   // quit the app when the window closes

private:
    void StartPlayback();
    void StopPlayback(bool resumeMonitor = true);  // false when about to record
    void StartRecording();
    void StopRecording();
    void StartCapture();             // open the Recorder (after any count-in)
    void StartMidiCapture();         // connect armed MIDI inputs, begin the take
    // Resolve each MIDI track's endpoint NAME to a live producer id and publish
    // the routes to fMidiRoutes + the engine. Called wherever the input opens.
    void ResolveMidiRoutes(const std::vector<MidiEndpointInfo>& eps);
    // Feed one live event to every armed track whose route accepts it.
    void FeedMidiEvent(const MidiEvent& e, Frame at);
    void StopMidiCapture(Frame endFrame);  // end take, drop MidiClip(s)
    void UpdateMidiMonitor();        // start/stop idle live-monitoring per arming
    void StopMidiMonitor();          // tear down the idle monitor engine + input
    void ReloadActiveEngine();       // rebuild the running engine at the playhead
                                     // (structural fx / tempo change, keep going)
    void SyncFxToEngine();           // a committed chain edit -> the running engine
    bool AudioMonitorOn() const;     // global flag OR an armed audio track's I btn
    bool StartRecordEngine(Frame engineStart);   // engine for overdub monitoring
    void UpdatePulse();              // run the poll iff playing or recording
    void UpdateTimeReadout(Frame playhead);
    void UpdateLoudnessReadout(float momLufs, float shortLufs, float truePeakDb);
    void PushTrackPeaks();           // engine per-track peaks -> timeline meters
    void PushRollPlayhead(Frame ph); // push the playhead to an open piano roll
    void PushFxMeters();             // engine fx meters -> effects window
    void SaveTo(const char* path);
    void LoadFrom(const char* path);
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
    std::string RenderPath(const std::string& tag) const;

    Project*        fProject;        // non-owning (the session)
    CommandStack*   fStack;          // non-owning
    PeakMap*        fPeaks;          // non-owning; new takes add entries here

    TimelineView*   fTimeline;
    InspectorView*  fInspector = nullptr;   // left track-inspector column
    class TransportBar* fTransport = nullptr;
    BStringView*    fTimeView;
    BStringView*    fLoudView = nullptr;   // LUFS / true-peak readout
    MeterView*      fMeter;
    BSlider*        fMaster;
    BTextControl*   fTempo;

    // Declared recorder-first so the engine (which RT-references the recorder as
    // its monitor source) is destroyed FIRST — members die in reverse order, so
    // the RT thread is stopped before the recorder it may read is freed.
    std::unique_ptr<Recorder> fRecorder;  // active while recording
    std::unique_ptr<Engine>   fEngine;    // rebuilt each Play
    // MIDI capture: a consumer connected to the armed MIDI tracks' input
    // endpoints, feeding a note-pairing recorder. Independent of the audio path.
    std::unique_ptr<MidiInputPort> fMidiIn;   // active while recording MIDI
    // One recorder per armed MIDI track, not one shared: inputs are demuxed, so
    // each track pairs only the events its own route accepts. With a single
    // keyboard every track's route is permissive and they all capture the same
    // stream, exactly as before.
    std::map<TrackId, MidiRecorder> fMidiRecs;
    std::vector<TrackId>      fMidiRecTracks; // armed MIDI targets for the take
    // Endpoint id + channel each MIDI track listens to, resolved from the
    // track's endpoint NAME when the input is opened (see ResolveMidiRoutes).
    std::vector<MidiInputRoute> fMidiRoutes;
    bigtime_t                 fMidiT0 = 0;    // system_time at MIDI capture start
    bool                      fMonitoring = false;  // idle live-monitor engine up
    BMessageRunner*           fPulse = nullptr;  // 60 Hz UI poll
    BMessageRunner*           fAutosave = nullptr;  // periodic crash-recovery save
    BMessenger                fMixerMsgr;    // open mixer window (for live peaks)
    BMessenger                fRollMsgr;     // last-opened piano roll (playhead)
    BMessenger                fFxMsgr;       // open effects window (for live meters)
    TrackId                   fFxTrack = kInvalidTrackId;  // its track (~0 master)
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
    size_t                    fBufferFrames = 512;  // output buffer frames/channel
    bool                      fMetronome = false;
    bool                      fMonDim = false;
    bool                      fMonMono = false;
    bool                      fPlaying = false;
    bool                      fRecMode = false;      // engine running for a take
    bool                      fCapturePending = false; // in count-in, not yet capturing
    bool                      fMonitorInput = false; // hear live input while armed
    bool                      fLoopRecord = false;   // capturing stacked takes over a loop
    int                       fTakeGroup = 0;        // running take-group id
    int                       fCountInBars = 0;      // metronome bars before capture
    BMenu*                    fCountInMenu = nullptr; // radio submenu (for marks)
    BMenuItem*                fMonInItem = nullptr;   // input-monitor toggle
    BMenuItem*                fFollowItem = nullptr;  // follow/chase playhead toggle
    int                       fTakeCounter = 0;
    std::vector<TrackId>      fRecTracks;   // all armed targets for the take
    Frame                     fRecStart = 0; // frame the capture (clip) begins at
    Frame                     fRecPoint = 0; // record start (== fRecStart)
    // Record round-trip latency (output + input path), in project-rate frames.
    // A captured take is this many frames late vs the timeline; the take is slid
    // earlier by it (RecordPlan::CompensateRoundTrip). 0 = no compensation until
    // the device latency is queried (Media Kit, on the target) into this field.
    Frame                     fRoundTripFrames = 0;
    std::string               fLastDir;      // last Open/Save/Import directory
    std::string               fTakeDir;      // where recorded takes are written
    std::string               fTakePath;     // full path of the current take
    int                       fRenderSeq = 0; // counter for rendered region/freeze filenames
};

} // namespace daw
