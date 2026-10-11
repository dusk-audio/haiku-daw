// RecordController — the record work, moved out of MainWindow (M1.1,
// slice 3b). The bodies are the window's, body for body, with the window
// passed in.
#include "RecordController.h"

#include "MainWindow.h"
#include "TimelineView.h"
#include "TransportBar.h"
#include "MeterView.h"
#include "../engine/DeviceLatency.h"
#include "../engine/WavSource.h"   // reading a finished take back (a WAV we wrote)
#include "../model/RecordPlan.h"   // CountInFrames / CompensateRoundTrip
#include "../model/TakeNames.h"     // NextFreeWavPath (the take name)

namespace daw {

void RecordController::StartRecording() {
    if (fRecMode || (fRecorder && fRecorder->IsRecording()))
        return;
    if (fWin->fTransportCtl.fPlaying) fWin->fTransportCtl.StopPlayback(false);   // don't spin up a monitor we replace
    StopMidiMonitor();   // the record engine takes over monitoring

    // Record onto every armed audio track (one input take, dropped on each),
    // and capture live MIDI onto every armed MIDI track that has a MIDI input.
    fRecTracks.clear();
    fMidiRecTracks.clear();
    for (const Track& t : fWin->fProject->Tracks()) {
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
    const Transport& tr = fWin->fProject->transport;
    fLoopRecord = tr.loopEnabled && tr.loopEnd > tr.loopStart;
    // What the device costs, in frames: the capture arrives this many frames
    // after the timeline frame it belongs to, and every take path below slides
    // it back by that much. Asked here, once per take, because the device (and
    // so the number) can change between them; 0 when the roster cannot answer,
    // which degrades to no compensation rather than to a wrong slide.
    fRoundTripFrames = DeviceRoundTripFrames(fWin->fProject->sampleRate);

    // Record point = loop start (loop-record) or the playhead. A count-in plays
    // the engine (existing tracks + click) for N bars leading up to it.
    fWin->fProject->tempoMap.sampleRate = fWin->fProject->sampleRate;
    fRecPoint = fLoopRecord ? tr.loopStart : fWin->fProject->transport.playhead;
    const Frame countIn = CountInFrames(fWin->fProject->tempoMap, fRecPoint,
                                        fCountInBars);
    const Frame engineStart = (fRecPoint > countIn) ? fRecPoint - countIn : 0;

    if (!fWin->fTransportCtl.StartRecordEngine(engineStart)) {
        std::fprintf(stderr, "MainWindow: record engine failed to start\n");
        return;
    }
    fRecMode = true;
    if (fWin->fTransport) fWin->fTransport->SetRecording(true);
    fCapturePending = (countIn > 0);
    if (!fCapturePending)
        StartCapture();         // no count-in: capture immediately
    fWin->UpdatePulse();
}

void RecordController::StopRecording() {
    if (!fRecMode)
        return;

    // Take end = the playhead now, before the engine stops (MIDI needs it to
    // close held notes at the take boundary).
    const Frame endPh = fWin->fTransportCtl.fEngine ? fWin->fTransportCtl.fEngine->Playhead() : fRecStart;
    const bool captured = fRecorder && fRecorder->IsRecording();
    int64_t frames = 0;
    double  recRate = fWin->fProject->sampleRate;
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
    if (fWin->fTransportCtl.fEngine) {
        fWin->fTransportCtl.fEngine->SetInputMonitor(false);
        fWin->fTransportCtl.fEngine->SetMonitorSource(nullptr);
        fWin->fTransportCtl.fEngine->Stop();
    }
    fRecMode = false;
    if (fWin->fTransport) fWin->fTransport->SetRecording(false);
    fCapturePending = false;
    fWin->fTimeline->SetRecording(false, 0, 0);
    fRecTracks.clear();
    fWin->UpdatePulse();
    fWin->fMeter->SetLevels(0.0f, 0.0f);

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
        (*fWin->fPeaks)[path].Build(src);

    // lengthFrames is timeline (project-rate) frames, converted from the
    // recorder-rate frame count. `ratio` = project/record rate; a source-frame
    // offset is a timeline offset divided by ratio (the capture file is at the
    // record rate).
    const double ratio = recRate > 0 ? fWin->fProject->sampleRate / recRate : 1.0;
    const Frame captureLen = (Frame)llround(frames * ratio);
    auto toSourceOffset = [&](Frame timelineOffset) -> Frame {
        return (Frame)llround(timelineOffset / ratio);
    };

    const Transport& tr = fWin->fProject->transport;

    // Loop-record: split the linear capture into one take per loop pass and
    // stack them as a take group on each armed track (last pass active).
    if (fLoopRecord) {
        // The capture's first `fRoundTripFrames` frames belong to the pass
        // BEFORE the loop; dropping them is what makes each take start where
        // the loop does.
        const std::vector<TakeRegion> takes =
            LoopTakes(tr.loopStart, tr.loopEnd, captureLen, fRoundTripFrames);
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
            fWin->fStack->Execute(std::move(macro), *fWin->fProject);
        }
        fRecorder.reset();
        fWin->fTimeline->ZoomToFit();   // show the whole take after recording
        return;
    }

    // Punch: trim the take to the punch range (non-destructive — the clip just
    // references a sub-span of the captured file). The plain take is round-trip-
    // compensated (slid earlier by the record latency); punch trimming is applied
    // as before (its compensation is a follow-up alongside loop-record).
    TakeRegion region;
    if (tr.punchEnabled) {
        region.startFrame = fRecStart;
        region.sourceOffset = 0;
        region.lengthFrames = captureLen;
        // The capture's own origin is the record start slid earlier by the
        // round trip: frame i of the file IS timeline frame (fRecStart -
        // rtFrames) + i, so the punch is intersected against that.
        if (!PunchedTake(fRecStart - fRoundTripFrames, captureLen,
                         tr.punchIn, tr.punchOut, &region)) {
            std::fprintf(stderr, "MainWindow: take outside punch range, discarded\n");
            fRecorder.reset();
            return;
        }
    } else {
        region = CompensateRoundTrip(fRecStart, captureLen, fRoundTripFrames);
    }

    for (TrackId target : targets) {
        Clip clip;
        clip.startFrame   = region.startFrame;
        clip.lengthFrames = region.lengthFrames;
        clip.sourceOffset = toSourceOffset(region.sourceOffset);
        clip.sourcePath   = path;
        fWin->fStack->Execute(std::make_unique<AddClipCommand>(target, clip), *fWin->fProject);
    }

    fRecorder.reset();
    fWin->fTimeline->ZoomToFit();   // show the whole take after recording
}

void RecordController::StartCapture() {
    fRecStart = fRecPoint;      // clip origin = record point
    fCapturePending = false;

    // Audio capture: only when an audio track is armed (a MIDI-only take opens
    // no input device and writes no WAV).
    if (!fRecTracks.empty()) {
        // Write takes into the project's directory (a self-contained bundle)
        // when the project has been saved; otherwise the working directory.
        // The name is the first free take-N.wav THERE -- not a session counter,
        // which restarts at 0 after reopening a project and would overwrite the
        // takes that project still uses (the Recorder refuses to clobber, but
        // scanning means the take lands instead of failing).
        fTakePath = NextFreeWavPath(fTakeDir, "take");
        fRecorder.reset(new Recorder());
        if (fRecorder->Start(fTakePath.c_str()) != B_OK) {
            std::fprintf(stderr, "MainWindow: recording failed to start\n");
            fWin->ReportError("Recording",
                        std::string("The take could not be started:\n") +
                        fTakePath);
            fRecorder.reset();
        } else {
            // Wire input monitoring: the engine mixes the recorder's live input
            // (only if the input rate matches the output rate).
            const bool audMon = fWin->AudioMonitorOn();
            fRecorder->SetMonitor(audMon);
            if (fWin->fTransportCtl.fEngine) {
                fWin->fTransportCtl.fEngine->SetMonitorSource(fRecorder.get());
                fWin->fTransportCtl.fEngine->SetInputMonitor(audMon);
            }
        }
    }

    StartMidiCapture();
}

void RecordController::StartMidiCapture() {
    std::vector<TrackId> monitor;   // armed OR input-monitor, with a MIDI input
    for (const Track& t : fWin->fProject->Tracks())
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
        const Track* t = fWin->fProject->FindTrack(id);
        if (!t) continue;
        for (const MidiEndpointInfo& e : eps)
            if (e.isProducer && e.name == t->input.name) {
                if (connected.insert(e.id).second) fMidiIn->ConnectFrom(e.id);
                break;
            }
    }
    ResolveMidiRoutes(eps);   // ids for the demux (engine + per-track recorders)
    // Discard events queued before this take (both the record and monitor
    // rings) so stale pre-connect notes don't record or sound, then start.
    MidiEvent tmp[64];
    while (fMidiIn->ReadEvents(tmp, 64) > 0) {}
    while (fMidiIn->MonitorInput()->ReadEvents(tmp, 64) > 0) {}
    fMidiRecs.clear();
    for (TrackId id : fMidiRecTracks) fMidiRecs[id].Begin(fRecStart);
    fMidiT0 = system_time();
    // Route live events to the engine so armed MIDI tracks sound as you play.
    if (fWin->fTransportCtl.fEngine) fWin->fTransportCtl.fEngine->SetLiveMidi(fMidiIn->MonitorInput());
}

void RecordController::StopMidiCapture(Frame endFrame) {
    if (!fMidiIn) return;
    // Drain any events still queued, stamping each by wall-clock frame.
    MidiEvent ev[64];
    std::size_t n;
    while ((n = fMidiIn->ReadEvents(ev, 64)) > 0)
        for (std::size_t i = 0; i < n; i++) {
            Frame mf = fRecStart + (Frame)((double)(ev[i].timeUs - fMidiT0)
                                           * 1e-6 * fWin->fProject->sampleRate);
            if (mf < fRecStart) mf = fRecStart;
            FeedMidiEvent(ev[i], mf);
        }
    // Close every armed track's take. Each holds only the events its own route
    // accepted, so two keyboards produce two different regions.
    std::map<TrackId, MidiClip> takes;
    for (TrackId id : fMidiRecTracks)
        takes[id] = fMidiRecs[id].End(endFrame);
    fMidiRecs.clear();
    if (fWin->fTransportCtl.fEngine) {
        fWin->fTransportCtl.fEngine->SetLiveMidi(nullptr);            // stop monitoring this source
        // Wait out any in-flight RT deref. If quiescence can't be confirmed,
        // Stop() blocks until the RT thread truly quiesces — never free first.
        if (!fWin->fTransportCtl.fEngine->QuiesceMonitorInput())
            fWin->fTransportCtl.fEngine->Stop();
    }
    fMidiIn.reset();   // disconnect + unregister the consumer (now UAF-safe)

    std::vector<TrackId> targets;
    targets.swap(fMidiRecTracks);

    // Each target drops only what IT captured. A track whose keyboard stayed
    // silent gets no clip, even while another track was recording.
    bool any = false;
    const Transport& tr = fWin->fProject->transport;
    const bool loopTakes =
        fLoopRecord && tr.loopEnabled && tr.loopEnd > tr.loopStart;

    for (TrackId target : targets) {
        const auto it = takes.find(target);
        if (it == takes.end()) continue;
        // Notes OR controllers make a take worth keeping: a pass that only moved
        // the expression pedal still recorded something.
        if (it->second.notes.empty() && it->second.events.empty()) continue;
        const MidiClip& take = it->second;

        // Loop-record: split this track's linear take into one region per pass
        // and stack them as a take group (last non-empty pass active), the MIDI
        // mirror of the audio LoopTakes path. fLoopRecord is still set here —
        // StopRecording resets it only after this returns.
        if (loopTakes) {
            const Frame loopLen = tr.loopEnd - tr.loopStart;
            const std::vector<std::vector<MidiNote>> passes =
                SplitMidiLoopTakes(take.notes, loopLen);
            int last = -1;   // index of the last non-empty pass
            for (size_t k = 0; k < passes.size(); k++)
                if (!passes[k].empty()) last = (int)k;
            if (last < 0) continue;   // every pass silent on this track
            // Controllers recorded during the take get dealt into the same
            // passes, each seeded with the value in force when it began.
            const std::vector<std::vector<MidiClipEvent>> evPasses =
                SplitMidiLoopEvents(take.events, loopLen, (int)passes.size());
            const int group = ++fTakeGroup;
            auto macro = std::make_unique<MacroCommand>("Loop MIDI Takes");
            for (size_t k = 0; k < passes.size(); k++) {
                if (passes[k].empty()) continue;   // no silent stacked take
                MidiClip c;
                c.startFrame   = tr.loopStart;
                c.lengthFrames = loopLen;
                c.notes        = passes[k];
                if (k < evPasses.size()) c.events = evPasses[k];
                c.takeGroup    = group;
                c.takeActive   = ((int)k == last);
                macro->Add(std::make_unique<AddMidiClipCommand>(target, c));
            }
            fWin->fStack->Execute(std::move(macro), *fWin->fProject);
        } else {
            MidiClip c = take;         // AddMidiClipCommand assigns a fresh id
            c.id = kInvalidClipId;
            fWin->fStack->Execute(std::make_unique<AddMidiClipCommand>(target, c),
                            *fWin->fProject);
        }
        any = true;
    }
    if (any) fWin->fTimeline->ZoomToFit();   // show the whole take after recording
}

void RecordController::UpdateMidiMonitor() {
    StopMidiMonitor();
    if (fWin->fTransportCtl.fPlaying || fRecMode) return;   // playback / record own the engine + input

    std::vector<TrackId> armed;   // MIDI tracks to monitor: armed OR input-monitor
    for (const Track& t : fWin->fProject->Tracks())
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
        const Track* t = fWin->fProject->FindTrack(id);
        if (!t) continue;
        for (const MidiEndpointInfo& e : eps)
            if (e.isProducer && e.name == t->input.name) {
                if (connected.insert(e.id).second) fMidiIn->ConnectFrom(e.id);
                break;
            }
    }

    // A monitor-only engine: renders live voices through the armed instruments,
    // no clip playback, no playhead advance.
    const Frame ph = fWin->fProject->transport.playhead;
    const Frame tenMin = (Frame)(fWin->fProject->sampleRate * 600.0);
    fWin->fTransportCtl.fEngine.reset(new Engine());
    fWin->fTransportCtl.fEngine->SetBufferFrames(fWin->fTransportCtl.fBufferFrames);
    fWin->fTransportCtl.fEngine->SetMonitorOnly(true);
    const status_t monRc = fWin->fTransportCtl.fEngine->Load(*fWin->fProject, ph, ph + tenMin);
    if (monRc != B_OK) {
        // Monitoring is a convenience; say why the meters are dead once, and
        // only when it is the device (not an empty project).
        if (monRc != B_ENTRY_NOT_FOUND)
            fWin->ReportError("Audio Device",
                        "The audio device could not be opened, so input "
                        "monitoring is off.");
        fWin->fTransportCtl.fEngine.reset();
        fMidiIn.reset();
        return;
    }
    ResolveMidiRoutes(eps);   // demux: each track hears only its own endpoint
    MidiEvent tmp[64];   // drop stale pre-connect events before monitoring
    while (fMidiIn->MonitorInput()->ReadEvents(tmp, 64) > 0) {}
    fWin->fTransportCtl.fEngine->SetLiveMidi(fMidiIn->MonitorInput());
    fWin->fTransportCtl.fEngine->Start();
    fWin->ReapplyFxWatches();   // brand-new engine: the watches live in the old one
    fWin->fTransportCtl.fMonitoring = true;
    fWin->UpdatePulse();   // poll the meters while monitoring
}

void RecordController::StopMidiMonitor() {
    if (!fWin->fTransportCtl.fMonitoring) return;
    if (fWin->fTransportCtl.fEngine) {
        fWin->fTransportCtl.fEngine->SetLiveMidi(nullptr);
        fWin->fTransportCtl.fEngine->Stop();
        fWin->fTransportCtl.fEngine.reset();
    }
    fMidiIn.reset();
    fWin->fTransportCtl.fMonitoring = false;
    fWin->UpdatePulse();
}

void RecordController::ResolveMidiRoutes(const std::vector<MidiEndpointInfo>& eps) {
    fMidiRoutes.clear();
    for (const Track& t : fWin->fProject->Tracks()) {
        if (t.type != TrackType::Midi || t.input.kind != InputSource::kMidi)
            continue;
        for (const MidiEndpointInfo& e : eps)
            if (e.isProducer && e.name == t.input.name) {
                fMidiRoutes.push_back(MidiInputRoute{ t.id, e.id,
                                                      t.input.channel });
                break;
            }
    }
    if (fWin->fTransportCtl.fEngine) fWin->fTransportCtl.fEngine->SetMidiRoutes(fMidiRoutes);
}

void RecordController::FeedMidiEvent(const MidiEvent& e, Frame at) {
    for (TrackId id : fMidiRecTracks) {
        if (!RouteAccepts(RouteFor(fMidiRoutes, id), e)) continue;
        fMidiRecs[id].OnEvent(e, at);
    }
}
} // namespace daw
