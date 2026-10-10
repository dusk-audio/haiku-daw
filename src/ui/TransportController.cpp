// TransportController — the engine's lifetime and the transport's work, moved out
// of MainWindow (M1.1, slice 2). The bodies are the window's, body for body,
// with the window passed in: a pure move, so the suites that already cover
// play/stop/rebuild are the verification.
//
// M4.1 changed WHAT the bodies do on one point only: a load no longer happens
// here. The engine object is kept, its player survives, and every rebuild is a
// graph built on the engine's worker thread and swapped in — so no path in this
// file blocks the window thread on a load any more. The behaviour each path
// asks for (which position, which range, which transport state) is unchanged.
#include "TransportController.h"

#include "MainWindow.h"
#include "TimelineView.h"
#include "MeterView.h"
#include "TransportBar.h"

namespace daw {

// How long the record start waits for its graph. Generous: it is the same
// build a play now does in the background, and a record start that gave up
// early would put the take out of time with the tracks.
static constexpr bigtime_t kRecordLoadTimeoutUs = 30000000;

Engine* TransportController::EnsureEngine() {
    if (!fEngine) {
        fEngine.reset(new Engine());
        fSeenLoads = 0;   // a new engine has completed no loads
    }
    return fEngine.get();
}

Frame TransportController::PlayRangeEnd(bool looping) const {
    const Transport& tr = fWin->fProject->transport;
    Frame minEnd = looping ? tr.loopEnd : 0;   // run through silence to loop end
    // Metronome with no audio content: run the transport (10 min) so the click
    // plays over silence rather than the engine reporting "nothing to play".
    if (fMetronome && ProjectEndFrame(*fWin->fProject) == 0) {
        const Frame ten = (Frame)(fWin->fProject->sampleRate * 600.0);
        if (ten > minEnd) minEnd = ten;
    }
    return minEnd;
}

void TransportController::StartPlayback() {
    if (fWin->fRecCtl.fRecMode)
        return;   // recording runs its own engine (overdub)
    fWin->fRecCtl.StopMidiMonitor();   // playback owns the engine + MIDI input
    // Rebuild the engine from the current model each time (RT-safe: no live
    // mutation of a running graph). Playback begins at the current playhead;
    // if it's already at/after the end (and not looping), rewind first.
    const Transport& tr = fWin->fProject->transport;
    const bool looping = tr.loopEnabled && tr.loopEnd > tr.loopStart;
    if (!looping && fWin->fProject->transport.playhead >= ProjectEndFrame(*fWin->fProject)) {
        fWin->fProject->transport.playhead = 0;
        fWin->fTimeline->SetPlayhead(0);
    }
    // When looping, begin at the loop start unless already inside the loop.
    if (looping && (fWin->fProject->transport.playhead < tr.loopStart
                    || fWin->fProject->transport.playhead >= tr.loopEnd)) {
        fWin->fProject->transport.playhead = tr.loopStart;
        fWin->fTimeline->SetPlayhead(tr.loopStart);
    }
    const Frame start   = fWin->fProject->transport.playhead;
    const Frame minEnd  = PlayRangeEnd(looping);

    Engine* e = EnsureEngine();
    e->SetBufferFrames(fBufferFrames);
    e->SetMonitorOnly(false);   // an idle monitor session may have owned this engine
    // The device: the ONE part of a load that still runs here, because it is the
    // part whose failure the user is shown. The player is then kept, so this is
    // a no-op on every play after the first (same format).
    const status_t dev = e->EnsurePlayer(*fWin->fProject);
    if (dev != B_OK) {
        fWin->ReportError("Audio Device",
                    "The audio output device could not be opened.\n\n"
                    "Check the Audio menu's buffer size, and that no other "
                    "application holds the device.");
        std::fprintf(stderr, "MainWindow: engine load failed (%s)\n",
                     strerror(dev));
        fEngine.reset();
        fSeenLoads = 0;
        // Leave the transport genuinely stopped: ReloadActiveEngine calls this
        // while fPlaying is ALREADY true, and a failed device open there is
        // reachable. Returning without clearing the flag would leave fPlaying
        // true with no engine: the transport button stayed lit, the pulse kept
        // firing for an engine that no longer existed, and live monitoring
        // refused to start because it believed playback still owned the engine.
        fPlaying = false;
        if (fWin->fTransport) fWin->fTransport->SetPlaying(false);
        fWin->UpdatePulse();
        return;
    }
    // Position-changing load: the graph is built on the worker and detached
    // from the old position immediately — the RT renders silence and the
    // playhead stands still until it lands (the same seek/loop-wrap behaviour
    // as before, without the frozen window). A load that finds nothing to play
    // reports through PollEngineLoad, from the pulse.
    e->RequestLoad(*fWin->fProject, start, minEnd, Engine::LoadMode::NewPosition);
    e->Start();
    e->SetMetronome(fMetronome);
    e->SetMonitorDim(fMonDim);
    e->SetMonitorMono(fMonMono);
    // Re-apply the effect-meter focus onto the engine (else an open FX
    // editor's GR/FFT meters die on a play after the engine was replaced).
    if (fWin->fFxTrack != kInvalidTrackId) e->SetMeterFocus(fWin->fFxTrack);
    // Same reason: the watches live in the Engine object too.
    fWin->ReapplyFxWatches();
    e->SetMidiRoutes(fWin->fRecCtl.fMidiRoutes);   // survives every rebuild
    fPlaying = true;
    if (fWin->fTransport) fWin->fTransport->SetPlaying(true);
    fWin->UpdatePulse();
}

void TransportController::StopPlayback(bool resumeMonitor) {
    if (fEngine)
        fEngine->Stop();
    fPlaying = false;
    if (fWin->fTransport) fWin->fTransport->SetPlaying(false);
    fWin->UpdatePulse();
    fWin->PushRollPlayhead(-1);   // hide the roll playhead when stopped
    fWin->fMeter->SetLevels(0.0f, 0.0f);
    fWin->fTimeline->ClearTrackPeaks();
    fWin->UpdateLoudnessReadout(Loudness::kSilenceLufs, Loudness::kSilenceLufs,
                          Loudness::kSilenceDb);
    // Leave the playhead where it stopped; the readout keeps its last value.
    if (resumeMonitor)
        fWin->fRecCtl.UpdateMidiMonitor();   // resume idle monitoring if a MIDI track is armed
}

void TransportController::ReloadActiveEngine() {
    // Rebuild whatever engine is running, at the current position, so a change
    // that can't be applied in place (adding/removing an effect, a tempo edit, a
    // clip edit) takes effect without a manual stop/play.
    if (fPlaying) {
        if (!fEngine) return;
        // If the engine has already reached the end, let it stop naturally
        // rather than restart from 0 (StartPlayback rewinds a past-end playhead).
        if (fEngine->IsFinished()) return;
        const Frame ph = fEngine->Playhead();
        fWin->fProject->transport.playhead = ph;
        const bool looping = fWin->fProject->transport.loopEnabled
                             && fWin->fProject->transport.loopEnd
                                    > fWin->fProject->transport.loopStart;
        // IN PLACE (M4.1): the graph is rebuilt on the worker while the current
        // one keeps playing, and the new one joins at the playhead the transport
        // has reached by then — so the edit is heard with no gap and no drift.
        fEngine->RequestLoad(*fWin->fProject, ph, PlayRangeEnd(looping),
                             Engine::LoadMode::InPlace);
    } else if (fMonitoring) {
        fWin->fRecCtl.UpdateMidiMonitor();      // rebuilds the idle monitor engine
    }
    // Stopped / recording: the change applies on the next Play / take.
}

bool TransportController::StartRecordEngine(Frame engineStart) {
    const Frame tenMin = (Frame)(fWin->fProject->sampleRate * 600.0);
    Engine* e = EnsureEngine();
    e->SetBufferFrames(fBufferFrames);
    e->SetMonitorOnly(false);   // a monitor-only graph carries no clips
    const status_t dev = e->EnsurePlayer(*fWin->fProject);
    if (dev != B_OK) {
        fWin->ReportError("Audio Device",
                    "The audio device could not be opened, so the take "
                    "was not started.");
        fEngine.reset();
        fSeenLoads = 0;
        return false;
    }
    // The record range always extends past content, so this build cannot fail
    // with "nothing to play"; the device above is the only failure it has.
    e->RequestLoad(*fWin->fProject, engineStart, engineStart + tenMin,
                   Engine::LoadMode::NewPosition);
    // WAITED for, unlike a play: the take's alignment (RecordPlan) is measured
    // from the moment the engine rolls, so the capture must not begin before
    // the graph is in. The record start blocked on the same build before M4.1;
    // it still does, for the same reason.
    if (!e->WaitForLoad(kRecordLoadTimeoutUs)
        || e->LastLoadStatus() != B_OK) {
        std::fprintf(stderr, "MainWindow: record engine failed to start\n");
        e->SetMonitorOnly(false);
        return false;
    }
    e->Start();
    // Count-in needs the click; force it on during record if a count-in is set.
    e->SetMetronome(fMetronome || fWin->fRecCtl.fCountInBars > 0);
    e->SetMonitorDim(fMonDim);
    e->SetMonitorMono(fMonMono);
    // Re-attach input monitoring across an engine restart (loop-record seam).
    if (fWin->fRecCtl.fRecorder) {
        e->SetMonitorSource(fWin->fRecCtl.fRecorder.get());
        e->SetInputMonitor(fWin->AudioMonitorOn());
    }
    if (fWin->fRecCtl.fMidiIn) e->SetLiveMidi(fWin->fRecCtl.fMidiIn->MonitorInput());
    // Routes live on the Engine and survive a graph swap, but re-pushing costs
    // nothing and keeps the demux right for a fresh engine object.
    e->SetMidiRoutes(fWin->fRecCtl.fMidiRoutes);
    if (fWin->fFxTrack != kInvalidTrackId) e->SetMeterFocus(fWin->fFxTrack);
    fWin->ReapplyFxWatches();   // the watches live in the Engine object
    return true;
}

void TransportController::PollEngineLoad() {
    if (!fEngine) return;
    const uint64_t done = fEngine->LoadsCompleted();
    if (done == fSeenLoads) return;
    fSeenLoads = done;
    const status_t rc = fEngine->LastLoadStatus();
    if (rc == B_OK || rc == B_CANCELED) return;   // built, or superseded at teardown
    // A build failed on the worker. B_ENTRY_NOT_FOUND is "nothing to play": a
    // fact, not an alert.
    if (rc != B_ENTRY_NOT_FOUND)
        fWin->ReportError("Audio Device",
                    "The audio output device could not be opened.\n\n"
                    "Check the Audio menu's buffer size, and that no other "
                    "application holds the device.");
    std::fprintf(stderr, "MainWindow: engine load failed (%s)\n", strerror(rc));
    if (!fPlaying) return;   // the monitor path loads synchronously and reports itself
    fEngine->Stop();
    fPlaying = false;
    if (fWin->fTransport) fWin->fTransport->SetPlaying(false);
    fWin->UpdatePulse();
}
} // namespace daw
