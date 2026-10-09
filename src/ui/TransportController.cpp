// TransportController — the engine's lifetime and the transport's work, moved out
// of MainWindow (M1.1, slice 2). The bodies are the window's, body for body,
// with the window passed in: a pure move, so the suites that already cover
// play/stop/rebuild are the verification.
#include "TransportController.h"

#include "MainWindow.h"
#include "TimelineView.h"
#include "MeterView.h"
#include "TransportBar.h"

namespace daw {

void TransportController::StartPlayback() {
    if (fWin->fRecMode)
        return;   // recording runs its own engine (overdub)
    fWin->StopMidiMonitor();   // playback owns the engine + MIDI input
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
    Frame minEnd  = looping ? tr.loopEnd : 0;   // run through silence to loop end
    // Metronome with no audio content: run the transport (10 min) so the click
    // plays over silence rather than the engine reporting "nothing to play".
    if (fMetronome && ProjectEndFrame(*fWin->fProject) == 0) {
        const Frame ten = (Frame)(fWin->fProject->sampleRate * 600.0);
        if (ten > minEnd) minEnd = ten;
    }
    fEngine.reset(new Engine());
    fEngine->SetBufferFrames(fBufferFrames);
    const status_t loadRc = fEngine->Load(*fWin->fProject, start, minEnd);
    if (loadRc != B_OK) {
        // B_ENTRY_NOT_FOUND is "nothing to play": a fact, not an alert.
        if (loadRc != B_ENTRY_NOT_FOUND)
            fWin->ReportError("Audio Device",
                        "The audio output device could not be opened.\n\n"
                        "Check the Audio menu's buffer size, and that no other "
                        "application holds the device.");
        std::fprintf(stderr, "MainWindow: engine load failed (%s)\n",
                     strerror(loadRc));
        fEngine.reset();
        // Leave the transport genuinely stopped. ReloadActiveEngine calls this
        // while fPlaying is ALREADY true, and Load failing there is reachable —
        // an undo that removes the last clip is enough. Returning without
        // clearing the flag left fPlaying true with a null fEngine: the
        // transport button stayed lit, the pulse kept firing for an engine that
        // no longer existed, and live monitoring refused to start because it
        // believed playback still owned the engine.
        fPlaying = false;
        if (fWin->fTransport) fWin->fTransport->SetPlaying(false);
        fWin->UpdatePulse();
        return;
    }
    fEngine->Start();
    fEngine->SetMetronome(fMetronome);
    fEngine->SetMonitorDim(fMonDim);
    fEngine->SetMonitorMono(fMonMono);
    // Re-apply the effect-meter focus onto the fresh engine (else an open FX
    // editor's GR/FFT meters die on every play / loop-wrap / seek rebuild).
    if (fWin->fFxTrack != kInvalidTrackId) fEngine->SetMeterFocus(fWin->fFxTrack);
    // Same reason, same rebuild: the watches live in the Engine object too.
    fWin->ReapplyFxWatches();
    fEngine->SetMidiRoutes(fWin->fMidiRoutes);   // survives the rebuild, as above
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
        fWin->UpdateMidiMonitor();   // resume idle monitoring if a MIDI track is armed
}

void TransportController::ReloadActiveEngine() {
    // Rebuild whatever engine is running, at the current position, so a change
    // that can't be applied in place (adding/removing an effect, a tempo edit)
    // takes effect without a manual stop/play. A brief seam is expected.
    if (fPlaying) {
        // If the engine has already reached the end, let it stop naturally
        // rather than restart from 0 (StartPlayback rewinds a past-end playhead).
        if (fEngine && fEngine->IsFinished()) return;
        if (fEngine) fWin->fProject->transport.playhead = fEngine->Playhead();
        StartPlayback();          // rebuilds at the playhead and keeps playing
    } else if (fMonitoring) {
        fWin->UpdateMidiMonitor();      // rebuilds the idle monitor engine
    }
    // Stopped / recording: the change applies on the next Play / take.
}

bool TransportController::StartRecordEngine(Frame engineStart) {
    const Frame tenMin = (Frame)(fWin->fProject->sampleRate * 600.0);
    fEngine.reset(new Engine());
    fEngine->SetBufferFrames(fBufferFrames);
    const status_t recRc = fEngine->Load(*fWin->fProject, engineStart,
                                         engineStart + tenMin);
    if (recRc != B_OK) {
        if (recRc != B_ENTRY_NOT_FOUND)
            fWin->ReportError("Audio Device",
                        "The audio device could not be opened, so the take "
                        "was not started.");
        fEngine.reset();
        return false;
    }
    fEngine->Start();
    // Count-in needs the click; force it on during record if a count-in is set.
    fEngine->SetMetronome(fMetronome || fWin->fCountInBars > 0);
    fEngine->SetMonitorDim(fMonDim);
    fEngine->SetMonitorMono(fMonMono);
    // Re-attach input monitoring across an engine restart (loop-record seam).
    if (fWin->fRecorder) {
        fEngine->SetMonitorSource(fWin->fRecorder.get());
        fEngine->SetInputMonitor(fWin->AudioMonitorOn());
    }
    if (fWin->fMidiIn) fEngine->SetLiveMidi(fWin->fMidiIn->MonitorInput());
    // Routes live on the Engine, and this is a BRAND NEW one — without this the
    // loop-record seam would quietly drop the demux mid-take and every armed
    // track would start hearing every keyboard.
    fEngine->SetMidiRoutes(fWin->fMidiRoutes);
    if (fWin->fFxTrack != kInvalidTrackId) fEngine->SetMeterFocus(fWin->fFxTrack);
    fWin->ReapplyFxWatches();   // the watches live in the Engine object too
    return true;
}
} // namespace daw
