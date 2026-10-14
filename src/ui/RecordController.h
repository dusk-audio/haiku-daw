// RecordController — the record state, moved out of MainWindow (M1.1, slice 3).
//
// Declaration order matters and lives here now: the window holds this BEFORE
// its TransportController, so the engine (which RT-references the recorder as
// its monitor source) is destroyed FIRST -- members die in reverse order, and
// the RT thread must be stopped before the recorder it may read is freed.
#pragma once

#include "../engine/Recorder.h"
#include "../midi/MidiPort.h"
#include "../midi/MidiRecorder.h"
#include "../midi/MidiRouting.h"   // MidiInputRoute

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace daw {

class MainWindow;

class RecordController {
public:
    // The window this controller drives (set once by MainWindow's constructor).
    // The moved methods reach the widgets and the model through it.
    void SetWindow(MainWindow* win) { fWin = win; }

    // The window's record work, body for body (M1.1).
    void StartRecording();
    void StopRecording();                                  // end the take
    void StartCapture();                                   // open the Recorder
    void StartMidiCapture();                               // connect armed inputs
    void StopMidiCapture(Frame endFrame);                  // end take, drop clips
    // The IDLE monitor: what a track hears when it is not a take -- its input
    // monitored live (MIDI: armed OR the I button; audio: the I button, no
    // arm needed). Raised on every arm/monitor change from the timeline, the
    // inspector or the mixer (kMsgMonitorRefresh) and torn down by play and
    // record, which own the engine and the input.
    void UpdateIdleMonitor();
    void StopIdleMonitor();
    void ResolveMidiRoutes(const std::vector<MidiEndpointInfo>& eps);
    void FeedMidiEvent(const MidiEvent& e, Frame at);

    // Public members on purpose (pure move; the window still drives them).
    std::unique_ptr<Recorder> fRecorder;  // active while recording
    bool   fRecMode = false;              // engine running for a take
    bool   fCapturePending = false;       // in count-in, not yet capturing
    bool   fLoopRecord = false;           // capturing stacked takes over a loop
    int    fTakeGroup = 0;                // running take-group id
    int    fCountInBars = 0;              // metronome bars before capture
    Frame  fRecStart = 0;                 // frame the capture (clip) begins at
    Frame  fRecPoint = 0;                 // record start (== fRecStart)
    // Record round-trip latency (output + input path), in project-rate frames.
    // A captured take is this many frames late vs the timeline; the take is slid
    // earlier by it (RecordPlan::CompensateRoundTrip).
    Frame  fRoundTripFrames = 0;
    std::string fTakeDir;                 // where recorded takes are written
    std::string fTakePath;                // full path of the FIRST take
    // M5: one take file per armed audio track. `fTakeTracks[i]` is the track
    // `fRecorder->TakePath(i)` belongs to, so each take drops on its own track;
    // fTakePath stays the first of them (what the pre-M5 code called "the take").
    std::vector<TrackId> fTakeTracks;
    // Track -> take index for the live waveform the timeline draws on each
    // armed lane while capturing (M5). Built once at StartCapture, so the
    // 60 Hz poll does not build it.
    std::map<TrackId, int> fLiveTakeFor;
    // Take sample format (M5): 0 = 16-bit, 1 = 24-bit, 2 = 32-bit float.
    // Persisted by MainWindow as the `takefmt` setting; the Preferences window
    // (another package) is what will put a control in front of it.
    int    fTakeFormat = 1;
    std::vector<TrackId> fRecTracks;      // all armed targets for the take
    // MIDI capture: a consumer connected to the armed MIDI tracks' input
    // endpoints, feeding a note-pairing recorder. One recorder per armed track,
    // not one shared: inputs are demuxed, so each track pairs only the events
    // its own route accepts.
    // The idle audio monitor's capture: the input open for the I button, with
    // no take behind it (Recorder's monitor-only mode). Holds the device open
    // while monitoring, so it is stopped whenever a take or playback wants it.
    std::unique_ptr<Recorder> fLiveIn;
    std::unique_ptr<MidiInputPort> fMidiIn;   // active while recording MIDI
    std::map<TrackId, MidiRecorder> fMidiRecs;
    std::vector<TrackId>      fMidiRecTracks;  // armed MIDI targets for the take
    std::vector<MidiInputRoute> fMidiRoutes;   // per-track endpoint id + channel
    bigtime_t                 fMidiT0 = 0;     // system_time at MIDI capture start

private:
    MainWindow* fWin = nullptr;
};

} // namespace daw
