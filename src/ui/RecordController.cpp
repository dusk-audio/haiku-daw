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
    StopIdleMonitor();   // the record engine takes over monitoring

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
    std::vector<std::string> takePaths;
    if (captured) {
        fRecorder->Stop();
        frames  = fRecorder->FramesWritten();
        recRate = fRecorder->SampleRate();
        // One file per armed audio track (M5). Collected before the recorder is
        // freed, and index-aligned with `targets` below.
        for (size_t i = 0; i < fRecorder->TakeCount(); i++)
            takePaths.push_back(fRecorder->TakePath(i));
    }
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
    fTakeTracks.clear();       // the drop below works from its own copy
    fLiveTakeFor.clear();
    fWin->UpdatePulse();
    // Bars to zero; the meter's clip latch and peak hold are its own state
    // (M3.3) and are deliberately kept (see TransportController).
    fWin->fMeter->SetReadings(MeterReadings{});

    // Finalize the MIDI take independently of the audio take (a MIDI-only
    // record has no audio recorder / targets, so this must run before the
    // audio empty-take early-out below).
    StopMidiCapture(endPh);
    // Resume idle monitoring if a MIDI track is still armed (runs on every
    // StopRecording exit path, since the clip-drop code below may early-return).
    UpdateIdleMonitor();

    if (!captured || frames <= 0 || targets.empty()) {
        if (fRecorder) std::fprintf(stderr, "MainWindow: empty take, no clip added\n");
        fRecorder.reset();
        return;
    }

    // Waveform envelopes (M5): the FIRST take is scanned here, so the track
    // being recorded gets its waveform the moment the take lands -- as it
    // always has. Any further take is a second full file to read, which is
    // exactly what M4.4 moved off the window thread, so those go to the
    // background loader (and, if a pass of its own is running, wait for the
    // next project load rather than killing that pass and blanking every other
    // waveform in the project).
    if (!takePaths.empty()) {
        WavSource src;
        if (src.Open(takePaths[0]))
            (*fWin->fPeaks)[takePaths[0]].Build(src);
        if (takePaths.size() > 1)
            fWin->LoadPeaksInBackground(std::vector<std::string>(takePaths.begin() + 1,
                                                                 takePaths.end()));
    }

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
        // Each armed track stacks the passes of ITS OWN take file (M5): the
        // takes are index-aligned with `targets`.
        for (size_t i = 0; i < targets.size() && i < takePaths.size(); i++) {
            const TrackId target = targets[i];
            const int group = ++fTakeGroup;
            auto macro = std::make_unique<MacroCommand>("Loop Takes");
            for (size_t k = 0; k < takes.size(); k++) {
                Clip clip;
                clip.startFrame   = takes[k].startFrame;
                clip.lengthFrames = takes[k].lengthFrames;
                clip.sourceOffset = toSourceOffset(takes[k].sourceOffset);
                clip.sourcePath   = takePaths[i];
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

    // Each armed track gets ITS OWN take file (M5), at the same timeline
    // position: the takes came out of one capture, so they line up.
    for (size_t i = 0; i < targets.size() && i < takePaths.size(); i++) {
        Clip clip;
        clip.startFrame   = region.startFrame;
        clip.lengthFrames = region.lengthFrames;
        clip.sourceOffset = toSourceOffset(region.sourceOffset);
        clip.sourcePath   = takePaths[i];
        fWin->fStack->Execute(std::make_unique<AddClipCommand>(targets[i], clip), *fWin->fProject);
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
        // One take per armed audio track (M5): every take takes its OWN
        // channels out of ONE capture of the chosen device, written at the
        // preference's sample format. The names are picked TOGETHER -- the
        // first pick's file does not exist yet, so a second scan would hand
        // out the same name (model/TakeNames.h).
        const TakeFormat fmt = TakeFormatFromInt(fTakeFormat);
        fTakeTracks = fRecTracks;      // take i belongs to fTakeTracks[i]
        fLiveTakeFor.clear();
        for (size_t i = 0; i < fTakeTracks.size(); i++)
            fLiveTakeFor[fTakeTracks[i]] = (int)i;
        const std::vector<std::string> paths =
            NextFreeWavPaths(fTakeDir, "take", (int)fTakeTracks.size());
        fTakePath = paths.empty() ? std::string() : paths[0];

        std::vector<TakeSpec> specs;
        std::string device;
        for (size_t i = 0; i < fTakeTracks.size(); i++) {
            const Track* t = fWin->fProject->FindTrack(fTakeTracks[i]);
            // Keep the specs aligned with fTakeTracks: a track that vanished
            // between arming and capture still gets its slot (the take is then
            // dropped for nobody, not on the wrong track).
            const InputSource in = t ? t->input : InputSource{};
            const TrackChannels width = t ? t->channels : TrackChannels::Stereo;
            specs.push_back(PlanTake(i < paths.size() ? paths[i] : std::string(),
                                     in, width, fmt));
            // ONE capture has ONE device: the first armed track that names one
            // is what the whole pass records from, and the others' channel
            // addresses are read on that device (an armed track naming another
            // interface records this one's channels rather than nothing).
            if (device.empty() && !in.device.empty())
                device = in.device;
        }
        if (specs.empty() || paths.size() < fTakeTracks.size()) {
            // A directory with a million takes: NextFreeWavPaths gave up rather
            // than promising a name twice.
            std::fprintf(stderr, "MainWindow: no take name available\n");
            fWin->ReportError("Recording",
                        std::string("No free take name in ") +
                        (fTakeDir.empty() ? std::string("the working directory")
                                          : fTakeDir));
            fTakeTracks.clear();
            return;
        }

        fRecorder.reset(new Recorder());
        if (fRecorder->Start(specs, device) != B_OK) {
            std::fprintf(stderr, "MainWindow: recording failed to start\n");
            fWin->ReportError("Recording",
                        std::string("The take could not be started:\n") +
                        fTakePath);
            fRecorder.reset();
            fTakeTracks.clear();
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

// The idle monitor: a monitor-only engine that renders live input with no take
// and no transport — what the per-track I button promises ("hear the input live
// w/o record-enable"). MIDI goes through live voices (armed OR input-monitor,
// the pre-existing rule); AUDIO goes through a monitor-only capture of the
// track's own input, which is what this pass added: until now the only
// IMonitorSource was the take's recorder, so the I button on an audio track did
// nothing outside a take.
void RecordController::UpdateIdleMonitor() {
    StopIdleMonitor();
    if (fWin->fTransportCtl.fPlaying || fRecMode) return;   // playback / record own the engine + input

    std::vector<TrackId> armed;   // MIDI tracks to monitor: armed OR input-monitor
    for (const Track& t : fWin->fProject->Tracks())
        if (t.type == TrackType::Midi && (t.armed || t.inputMonitor)
            && t.input.kind == InputSource::kMidi)
            armed.push_back(t.id);

    std::vector<MidiEndpointInfo> eps;
    if (!armed.empty()) {
        // Open a consumer and connect each armed track's endpoint by name.
        fMidiIn.reset(new MidiInputPort("HaikuDAW In"));
        if (fMidiIn->Register() != B_OK) {
            fMidiIn.reset();
        } else {
            eps = EnumerateMidiEndpoints();
            std::set<int32> connected;   // dedupe: two tracks may share an endpoint
            for (TrackId id : armed) {
                const Track* t = fWin->fProject->FindTrack(id);
                if (!t) continue;
                for (const MidiEndpointInfo& e : eps)
                    if (e.isProducer && e.name == t->input.name) {
                        if (connected.insert(e.id).second)
                            fMidiIn->ConnectFrom(e.id);
                        break;
                    }
            }
        }
    }

    // Audio: the I button ALONE is the switch (the "Monitor the input while
    // recording" preference is exactly that — while recording; a take consults
    // it through AudioMonitorOn()). The monitor path is one stereo stream, like
    // a take's capture, so it follows the FIRST audio track that asks for it —
    // the same rule as "the first armed track names the device".
    const Track* audioMon = nullptr;
    for (const Track& t : fWin->fProject->Tracks()) {
        if (t.type != TrackType::Audio || !t.inputMonitor) continue;
        if (t.input.kind == InputSource::kNone) continue;
        audioMon = &t;
        break;
    }
    if (audioMon != nullptr) {
        const InputSource& in = audioMon->input;
        const bool stereo = audioMon->channels == TrackChannels::Stereo;
        const int l = in.channelL < 0 ? 0 : in.channelL;
        const int r = stereo ? (in.channelR < 0 ? l : in.channelR) : l;
        fLiveIn.reset(new Recorder());
        if (fLiveIn->StartMonitor(in.device, l, r) != B_OK)
            fLiveIn.reset();     // no input device: the app goes on, silently
    }

    if (fMidiIn == nullptr && fLiveIn == nullptr) return;

    // A monitor-only engine: renders live voices through the armed instruments
    // and mixes the monitored input, no clip playback, no playhead advance. The
    // engine object is reused (M4.1 keeps the device open across every rebuild);
    // the monitor-only flag says what this graph is for, and the playback paths
    // clear it again.
    const Frame ph = fWin->fProject->transport.playhead;
    const Frame tenMin = (Frame)(fWin->fProject->sampleRate * 600.0);
    Engine* e = fWin->fTransportCtl.EnsureEngine();
    e->SetBufferFrames(fWin->fTransportCtl.fBufferFrames);
    e->SetMonitorOnly(true);
    // Synchronous on purpose: starting the monitor is a user action with an
    // immediate answer (the meters either come alive or the reason is shown),
    // and the monitor graph carries no clips — there is nothing for the async
    // path to win here.
    const status_t monRc = e->Load(*fWin->fProject, ph, ph + tenMin);
    if (monRc != B_OK) {
        // Monitoring is a convenience; say why the meters are dead once, and
        // only when it is the device (not an empty project).
        if (monRc != B_ENTRY_NOT_FOUND)
            fWin->ReportError("Audio Device",
                        "The audio device could not be opened, so input "
                        "monitoring is off.");
        e->SetMonitorOnly(false);
        fMidiIn.reset();
        fLiveIn.reset();
        return;
    }
    if (fMidiIn != nullptr) {
        ResolveMidiRoutes(eps);   // demux: each track hears only its own endpoint
        MidiEvent tmp[64];   // drop stale pre-connect events before monitoring
        while (fMidiIn->MonitorInput()->ReadEvents(tmp, 64) > 0) {}
        e->SetLiveMidi(fMidiIn->MonitorInput());
    }
    if (fLiveIn != nullptr) {
        e->SetMonitorSource(fLiveIn.get());
        e->SetInputMonitor(true);
    }
    e->Start();
    fWin->ReapplyFxWatches();   // the watch slots live on the engine, re-addressed
    fWin->fTransportCtl.fMonitoring = true;
    fWin->UpdatePulse();   // poll the meters while monitoring
}

void RecordController::StopIdleMonitor() {
    if (!fWin->fTransportCtl.fMonitoring) return;
    if (fWin->fTransportCtl.fEngine) {
        Engine* e = fWin->fTransportCtl.fEngine.get();
        e->SetLiveMidi(nullptr);
        // The audio monitor is detached BEFORE the capture behind it is freed.
        // Stop() blocks until the last callback has exited, so the free below
        // cannot race the RT thread that reads that pointer (the same rule as
        // the take's recorder, in StopRecording).
        e->SetInputMonitor(false);
        e->SetMonitorSource(nullptr);
        e->Stop();
        // The engine — and with it the open device — is KEPT (M4.1: the player
        // outlives every rebuild). A play or a take re-arms it with
        // SetMonitorOnly(false) and loads its own graph.
    }
    fMidiIn.reset();
    fLiveIn.reset();
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
