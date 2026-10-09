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
    void UpdateMidiMonitor();                              // arm-driven monitor
    void StopMidiMonitor();
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
    std::string fTakePath;                // full path of the current take
    std::vector<TrackId> fRecTracks;      // all armed targets for the take
    // MIDI capture: a consumer connected to the armed MIDI tracks' input
    // endpoints, feeding a note-pairing recorder. One recorder per armed track,
    // not one shared: inputs are demuxed, so each track pairs only the events
    // its own route accepts.
    std::unique_ptr<MidiInputPort> fMidiIn;   // active while recording MIDI
    std::map<TrackId, MidiRecorder> fMidiRecs;
    std::vector<TrackId>      fMidiRecTracks;  // armed MIDI targets for the take
    std::vector<MidiInputRoute> fMidiRoutes;   // per-track endpoint id + channel
    bigtime_t                 fMidiT0 = 0;     // system_time at MIDI capture start

private:
    MainWindow* fWin = nullptr;
};

} // namespace daw
