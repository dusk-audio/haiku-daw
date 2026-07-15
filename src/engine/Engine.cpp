#include "Engine.h"

#include "../dsp/EffectFactory.h"
#include "../model/Crossfade.h"

#include <MediaDefs.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace daw {

// Ring holds ~2 seconds of stereo audio at 48k: plenty of slack so the
// disk thread stays ahead of the RT callback without hoarding memory.
static constexpr size_t kRingFramesPerStream = 48000 * 2;   // frames
static constexpr size_t kRingFloats = kRingFramesPerStream * 2;

// Equal-power pan: pan -1 = hard left, 0 = center (-3 dB each), +1 = hard
// right. Folds the track gain into the returned per-channel gains.
static void EqualPowerGains(float gain, float pan, float* outL, float* outR) {
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    const float theta = (pan * 0.5f + 0.5f) * float(M_PI) * 0.5f;
    *outL = gain * std::cos(theta);
    *outR = gain * std::sin(theta);
}

// --- TrackStream ------------------------------------------------------

TrackStream::TrackStream(TrackId track, const std::string& path,
                         Frame startFrame, Frame lengthFrames,
                         Frame sourceOffset, float gain, float pan,
                         bool audible, Frame seekProjectDelta, float outputRate,
                         Frame fadeIn, Frame fadeOut, float clipGain)
    : fTrackId(track), fPath(path), fStart(startFrame), fLength(lengthFrames),
      fSourceOffset(sourceOffset), fSeekDelta(seekProjectDelta),
      fOutputRate(outputRate), fFadeIn(fadeIn), fFadeOut(fadeOut),
      fClipGain(clipGain), fRing(kRingFloats) {
    float gl, gr;
    EqualPowerGains(gain, pan, &gl, &gr);
    fGainL.store(gl);
    fGainR.store(gr);
    fAudible.store(audible);
}

void TrackStream::SetMix(float gain, float pan, bool audible) {
    float gl, gr;
    EqualPowerGains(gain, pan, &gl, &gr);
    fGainL.store(gl, std::memory_order_relaxed);
    fGainR.store(gr, std::memory_order_relaxed);
    fAudible.store(audible, std::memory_order_relaxed);
}

void TrackStream::SetGainPan(float gain, float pan) {
    float gl, gr;
    EqualPowerGains(gain, pan, &gl, &gr);
    fGainL.store(gl, std::memory_order_relaxed);
    fGainR.store(gr, std::memory_order_relaxed);   // audibility untouched
}

void TrackStream::SetAudible(bool audible) {
    fAudible.store(audible, std::memory_order_relaxed);   // gain untouched
}

TrackStream::~TrackStream() {
    StopThread();
}

status_t TrackStream::Prepare() {
    if (!fSource.Open(fPath))
        return B_ERROR;

    const float srcRate = fSource.FrameRate();
    fResampler.reset(new Resampler(srcRate, fOutputRate));

    // Seek the source to match the start playhead. fSeekDelta is in output
    // frames; convert to source frames by the rate ratio.
    if (fSeekDelta > 0 && fOutputRate > 0) {
        const Frame srcSkip = static_cast<Frame>(
            fSeekDelta * (double)srcRate / fOutputRate + 0.5);
        fSource.Seek(fSourceOffset + srcSkip);
    } else if (fSourceOffset > 0) {
        fSource.Seek(fSourceOffset);
    }

    fRunning.store(true);
    fDiskThread = std::thread(&TrackStream::DiskLoop, this);

    // Prime: give the disk thread a moment to fill the ring before the RT
    // callback starts draining, so playback doesn't underrun on frame 0.
    for (int i = 0; i < 50 && fRing.ReadAvailable() < kRingFloats / 2; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    return B_OK;
}

void TrackStream::StopThread() {
    fRunning.store(false);
    if (fDiskThread.joinable())
        fDiskThread.join();
}

// Producer thread: decode chunks and push them into the ring. Carries the
// unpushed remainder of a chunk across iterations when the ring is full.
void TrackStream::DiskLoop() {
    const float* chunk = nullptr;   // points into fResampled
    size_t chunkFloats = 0;         // remaining floats to push
    size_t chunkOffset = 0;

    while (fRunning.load()) {
        if (chunkFloats == 0) {
            const float* src = nullptr;
            size_t frames = 0;
            if (!fSource.ReadChunk(&src, &frames)) {
                // End of file: nothing more to push. Idle until stopped.
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            // Resample source-rate audio up/down to the output rate so the RT
            // callback drains at 1 ring frame == 1 timeline frame.
            fResampled.clear();
            fResampler->Process(src, frames, fResampled);
            if (fResampled.empty())
                continue;   // produced nothing this pass (heavy downsample)
            chunk = fResampled.data();
            chunkFloats = fResampled.size();
            chunkOffset = 0;
        }

        size_t pushed = fRing.Write(chunk + chunkOffset, chunkFloats);
        chunkOffset += pushed;
        chunkFloats -= pushed;

        if (pushed == 0)   // ring full: wait for the RT thread to drain
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void TrackStream::Mix(float* out, size_t frames, Frame blockStart) {
    const Frame clipEnd = fStart + fLength;
    // Snapshot the live params once per block.
    const float gl  = fGainL.load(std::memory_order_relaxed);
    const float gr  = fGainR.load(std::memory_order_relaxed);
    const bool  aud = fAudible.load(std::memory_order_relaxed);

    for (size_t i = 0; i < frames; i++) {
        const Frame ph = blockStart + static_cast<Frame>(i);
        if (ph < fStart || ph >= clipEnd)
            continue;   // outside the clip -> contribute nothing

        float lr[2];
        if (fRing.Read(lr, 2) < 2)
            continue;   // underrun -> silence for this frame

        // Always consume the ring above; only sum when audible so an unmute
        // resumes in sample-sync rather than replaying buffered audio.
        if (aud) {
            // Linear fade-in/out envelope (combined as a min).
            float fade = 1.0f;
            const Frame rel = ph - fStart;
            if (fFadeIn > 0 && rel < fFadeIn)
                fade = (float)rel / (float)fFadeIn;
            if (fFadeOut > 0 && rel >= fLength - fFadeOut) {
                float fo = (float)(fLength - rel) / (float)fFadeOut;
                if (fo < fade) fade = fo;
            }
            out[i * 2 + 0] += lr[0] * gl * fade * fClipGain;
            out[i * 2 + 1] += lr[1] * gr * fade * fClipGain;
        }
    }
}

// --- Engine -----------------------------------------------------------

Engine::~Engine() {
    Stop();
    fStreams.clear();   // joins disk threads via ~TrackStream
    fPlayer.reset();
}

status_t Engine::Load(const Project& project, Frame startFrame,
                      Frame minEndFrame) {
    fStartFrame = startFrame;
    // Open the output first so we know the real output rate.
    media_raw_audio_format format = media_raw_audio_format::wildcard;
    format.frame_rate    = project.sampleRate;
    format.channel_count = 2;
    format.format        = media_raw_audio_format::B_AUDIO_FLOAT;
    format.byte_order    = B_MEDIA_HOST_ENDIAN;
    // Request an explicit buffer size (frames * channels * 4 bytes) instead of
    // letting media_server suggest one. Smaller = lower latency, more xrun risk.
    format.buffer_size   = fBufferFrames * 2 * sizeof(float);

    fPlayer.reset(new BSoundPlayer(&format, "haiku_daw", PlayTrampoline,
                                   nullptr, this));
    status_t err = fPlayer->InitCheck();
    if (err != B_OK) {
        fprintf(stderr, "Engine: BSoundPlayer init failed: %s\n", strerror(err));
        return err;
    }
    fOutputRate = fPlayer->Format().frame_rate;
    fSynth.SetSampleRate(fOutputRate);
    fMetronome = Metronome(fOutputRate, project.tempoBPM,
                           project.timeSig.numerator);
    fMasterGain.store(project.masterGain);

    // Solo overrides mute: if any track (audio or MIDI) is soloed, only
    // soloed (non-muted) tracks play.
    bool anySolo = false;
    for (const Track& t : project.Tracks())
        if (t.soloed && !t.muted)
            anySolo = true;

    // Build one stream per audio clip. All clips get a stream (even muted /
    // non-soloed) so mute/solo can be toggled live; audibility is a per-stream
    // flag the RT mix honors, not a build-time filter.
    fEndFrame = 0;
    fMonFrame = startFrame;   // free-running clock for monitor-only mode
    const bool monitorOnly = fMonitorOnly.load();
    for (const Track& t : project.Tracks()) {
        if (t.type != TrackType::Audio || monitorOnly)
            continue;   // monitor-only: no clip streams (no disk I/O)
        const bool audible = !t.muted && (!anySolo || t.soloed);
        const std::vector<ClipFades> fades = ComputeCrossfades(t.clips);
        for (size_t ci = 0; ci < t.clips.size(); ci++) {
            const Clip& c = t.clips[ci];
            if (c.sourcePath.empty())
                continue;
            if (c.takeGroup > 0 && !c.takeActive)
                continue;   // inactive loop-record take: don't stream it
            // How far into the clip (in timeline frames) playback starts.
            const Frame seekDelta =
                (startFrame > c.startFrame) ? startFrame - c.startFrame : 0;
            auto s = std::make_unique<TrackStream>(
                t.id, c.sourcePath, c.startFrame, c.lengthFrames,
                c.sourceOffset, t.gain, t.pan, audible, seekDelta, fOutputRate,
                fades[ci].fadeIn, fades[ci].fadeOut, c.gain);
            if (s->Prepare() != B_OK || !s->Valid()) {
                fprintf(stderr, "Engine: skipping clip '%s'\n",
                        c.sourcePath.c_str());
                continue;
            }
            // Sources are resampled to the output rate on their disk thread.
            if (s->EndFrame() > fEndFrame)
                fEndFrame = s->EndFrame();
            fStreams.push_back(std::move(s));
        }
    }

    // Group audio streams into per-track buses.
    fBuses.clear();
    for (auto& s : fStreams) {
        Bus* bus = nullptr;
        for (Bus& b : fBuses)
            if (b.id == s->Track()) { bus = &b; break; }
        if (!bus) {
            fBuses.push_back(Bus{});
            fBuses.back().id = s->Track();
            bus = &fBuses.back();
        }
        bus->streams.push_back(s.get());
    }

    // Live-monitor state: reset the held-voice pool for this (re)load and make
    // sure the note scratch has capacity so the RT rebuild never allocates.
    for (LiveVoice& v : fVoices) v.active = false;
    fLiveNotes.clear();
    fLiveNotes.reserve(kMaxLiveVoices);

    // Add a bus per audible MIDI track: snapshot its notes for the synth to
    // render (rebuild-on-play, so MIDI edits apply on the next Start). An armed
    // MIDI track also gets a bus even with no notes, so live input can be
    // monitored (synthesized) through its instrument while recording.
    for (const Track& t : project.Tracks()) {
        if (t.type != TrackType::Midi)
            continue;
        std::vector<MidiNote> notes = t.CollectNotes();   // absolute-timeline
        const bool monitor = t.armed;
        const bool audible = !t.muted && (!anySolo || t.soloed);
        if ((notes.empty() && !monitor) || (!audible && !monitor))
            continue;
        Bus b;
        b.id    = t.id;
        for (const MidiNote& n : notes)
            if (n.startFrame + n.lengthFrames > fEndFrame)
                fEndFrame = n.startFrame + n.lengthFrames;
        b.notes = std::move(notes);
        b.instrument = t.instrument;
        b.liveMonitor = monitor;             // synth live input into this bus
        b.audible     = audible || monitor;  // monitoring overrides mute/solo
        EqualPowerGains(t.gain, t.pan, &b.midiGainL, &b.midiGainR);
        fBuses.push_back(std::move(b));
    }

    // Add a node per bus track (always, even muted, so it stays a valid
    // routing target; a muted bus just doesn't route its sum onward).
    for (const Track& t : project.Tracks()) {
        if (t.type != TrackType::Bus)
            continue;
        Bus b;
        b.id    = t.id;
        b.isBus = true;
        EqualPowerGains(t.gain, t.pan, &b.busGainL, &b.busGainR);
        fBuses.push_back(std::move(b));
    }

    // No content is fine only if the caller extends the range (metronome/loop).
    bool anyContent = false;
    for (const Bus& b : fBuses)
        if (!b.streams.empty() || !b.notes.empty()) { anyContent = true; break; }
    if (!anyContent && minEndFrame <= 0) {
        fprintf(stderr, "Engine: nothing to play\n");
        return B_ERROR;
    }

    // Extend the play range past content for looping / a metronome-only run.
    if (minEndFrame > fEndFrame)
        fEndFrame = minEndFrame;

    // Set each node's routing output + audibility, and build its effect chain.
    for (Bus& b : fBuses) {
        const Track* t = project.FindTrack(b.id);
        if (!t) continue;
        b.output  = t->output;
        // Audio leaves are gated live by their streams (so live mute/unmute
        // works); a bus node is gated here (rebuild-on-play) and skips routing
        // its sum when muted / solo'd out.
        b.audible = b.isBus ? (!t->muted && (!anySolo || t->soloed)) : true;
        // Keep b.fx index-aligned with t->fx (nullptr placeholder for any
        // effect that fails to build, e.g. a missing plugin add-on) so
        // fxAuto's fxIndex addresses the right effect. Process skips nulls.
        for (const EffectDesc& d : t->fx) {
            auto fx = MakeEffect(d);
            if (fx) { fx->Prepare(fOutputRate); fx->SetTempo(project.tempoBPM); }
            b.fx.push_back(std::move(fx));
            b.fxTypes.push_back(d.type);
        }
        // Automation snapshot (RT-owned copy of the lanes).
        b.statGain = t->gain;
        b.statPan  = t->pan;
        b.gainAuto = t->gainAuto;
        b.panAuto  = t->panAuto;
        b.fxAuto   = t->fxAuto;   // effect-parameter automation (index-aligned)
        b.hasAuto  = t->gainAuto.Count() > 0 || t->panAuto.Count() > 0;
    }

    // Resolve each node's aux-send destinations to node indices (RT does no id
    // lookups). Post-fader in the live engine; the offline Exporter honors the
    // pre/post-fader flag exactly.
    auto nodeIndexOf = [&](TrackId id) -> long {
        for (size_t i = 0; i < fBuses.size(); i++)
            if (fBuses[i].id == id) return (long)i;
        return -1;
    };
    for (Bus& b : fBuses) {
        const Track* t = project.FindTrack(b.id);
        if (!t) continue;
        for (const Send& s : t->sends) {
            if (s.dest == kInvalidTrackId || s.dest == b.id) continue;
            const long di = nodeIndexOf(s.dest);
            if (di >= 0) b.sendTargets.push_back({(size_t)di, s.level});
        }
    }

    // Topological processing order: a node before every node it feeds — its
    // output AND every send destination. Sends add extra edges, so use the
    // general edge topo (single-output ResolveRoutingOrder can't express them).
    {
        std::vector<TrackId> nids;
        std::vector<std::pair<TrackId, TrackId>> edges;
        nids.reserve(fBuses.size());
        for (const Bus& b : fBuses) {
            nids.push_back(b.id);
            edges.push_back({b.id, b.output});
            const Track* t = project.FindTrack(b.id);
            if (t)
                for (const Send& s : t->sends)
                    if (s.dest != kInvalidTrackId && s.dest != b.id)
                        edges.push_back({b.id, s.dest});   // skip self-send edge
        }
        std::vector<TrackId> ord;
        fOrder.clear();
        if (ResolveOrderWithEdges(nids, edges, ord)) {
            for (TrackId id : ord)
                for (size_t i = 0; i < fBuses.size(); i++)
                    if (fBuses[i].id == id) { fOrder.push_back(i); break; }
        } else {   // cycle / bad graph: flat order, all to master
            for (size_t i = 0; i < fBuses.size(); i++) fOrder.push_back(i);
        }
    }

    // Master bus effect chain (applied to the summed output).
    fMasterFx.clear();
    fMasterFxTypes.clear();
    for (const EffectDesc& d : project.masterFx) {
        auto fx = MakeEffect(d);
        if (!fx) continue;
        fx->Prepare(fOutputRate);
        fx->SetTempo(project.tempoBPM);
        fMasterFx.push_back(std::move(fx));
        fMasterFxTypes.push_back(d.type);
    }

    // Master loudness meter. Integrated accumulation is disabled: it allocates
    // per 100 ms and this runs on the audio thread. Momentary / short-term /
    // true-peak stay valid (preallocated rings + fixed arrays).
    // Metronome follows the project's tempo/meter map (copied for the RT
    // thread). Sync its rate to the engine's output rate.
    {
        TempoMap tm = project.tempoMap;
        tm.sampleRate = fOutputRate;
        fMetronome.SetTempoMap(tm);
        fTempoMap = tm;          // for tempo-synced effects (delay)
        fLastFxBpm = 0.0;        // force a push on the first block
    }

    fLoudness.Prepare(fOutputRate);
    fLoudness.SetIntegratedEnabled(false);
    fLufsM.store(Loudness::kSilenceLufs);
    fLufsS.store(Loudness::kSilenceLufs);
    fTpDb.store(Loudness::kSilenceDb);

    // Per-node mix buffers, sized to the output buffer (generous floor).
    size_t maxFrames = (size_t)(fPlayer->Format().buffer_size
                                / (sizeof(float) * 2));
    if (maxFrames < 8192) maxFrames = 8192;
    fScratch.assign(maxFrames * 2, 0.0f);
    fMonBuf.assign(maxFrames * 2, 0.0f);
    fMonSrc.assign(maxFrames * 2 * 8, 0.0f);   // source-rate scratch (<=8x down)
    fMonPhase = 0.0; fMonPrevL = fMonPrevR = 0.0f;
    fNodeBufs.assign(fBuses.size(), std::vector<float>(maxFrames * 2, 0.0f));
    // Per-node peak meters (one atomic pair per node).
    fNodePeakL.reset(new std::atomic<float>[fBuses.size()]);
    fNodePeakR.reset(new std::atomic<float>[fBuses.size()]);
    for (size_t i = 0; i < fBuses.size(); i++) {
        fNodePeakL[i].store(0.0f);
        fNodePeakR[i].store(0.0f);
    }

    return B_OK;
}

void Engine::UpdateMix(const Project& project) {
    fMasterGain.store(project.masterGain, std::memory_order_relaxed);
    bool anySolo = false;
    for (const Track& t : project.Tracks())
        if (t.soloed && !t.muted)           // any track type can solo
            anySolo = true;

    for (const Track& t : project.Tracks()) {
        if (t.type != TrackType::Audio)
            continue;
        const bool audible = !t.muted && (!anySolo || t.soloed);
        // Automated tracks: automation owns gain/pan (driven per block in
        // FillBuffer); only refresh audibility here so live mute/solo still work.
        const bool automated = t.gainAuto.Count() > 0 || t.panAuto.Count() > 0;
        for (auto& s : fStreams)
            if (s->Track() == t.id) {
                if (automated) s->SetAudible(audible);
                else           s->SetMix(t.gain, t.pan, audible);
            }
    }
}

bool Engine::SyncFx(const Project& project) {
    // Push each effect's params from the model into the live objects. SetParam
    // is RT-safe (the RT thread may run concurrently). A chain whose structure
    // changed (different types/count) can't be updated in place -> report it so
    // the caller rebuilds.
    bool allMatched = true;
    auto sync = [&](std::vector<std::unique_ptr<IEffect>>& fx,
                    const std::vector<EffectType>& types,
                    const std::vector<EffectDesc>& descs) {
        if (fx.size() != descs.size()) { allMatched = false; return; }
        for (size_t i = 0; i < fx.size(); i++) {
            if (i >= types.size() || types[i] != descs[i].type) {
                allMatched = false;   // an effect was replaced at this slot
                continue;
            }
            if (!fx[i]) continue;
            for (size_t s = 0; s < descs[i].params.size(); s++)
                fx[i]->SetParam((int)s, descs[i].params[s]);
        }
    };
    for (Bus& b : fBuses) {
        const Track* t = project.FindTrack(b.id);
        if (t) sync(b.fx, b.fxTypes, t->fx);
    }
    sync(fMasterFx, fMasterFxTypes, project.masterFx);
    return allMatched;
}

void Engine::SetFxTempo(double bpm) {
    for (Bus& b : fBuses)
        for (auto& fx : b.fx)
            if (fx) fx->SetTempo(bpm);
    for (auto& fx : fMasterFx)
        if (fx) fx->SetTempo(bpm);
}

void Engine::Start() {
    fPlayhead.store(fStartFrame);
    fFinished.store(fStartFrame >= fEndFrame);
    fPlaying.store(true);
    fPlayer->SetHasData(true);
    fPlayer->Start();
}

void Engine::Stop() {
    fPlaying.store(false);
    if (fPlayer)
        fPlayer->Stop();
}

void Engine::PlayTrampoline(void* cookie, void* buffer, size_t size,
                            const media_raw_audio_format& format) {
    Engine* self = static_cast<Engine*>(cookie);
    const size_t frames = size / (sizeof(float) * format.channel_count);
    self->FillBuffer(static_cast<float*>(buffer), frames);
}

float Engine::TrackPeakL(TrackId id) const {
    if (!fNodePeakL) return 0.0f;
    for (size_t i = 0; i < fBuses.size(); i++)
        if (fBuses[i].id == id) return fNodePeakL[i].load(std::memory_order_relaxed);
    return 0.0f;
}
float Engine::TrackPeakR(TrackId id) const {
    if (!fNodePeakR) return 0.0f;
    for (size_t i = 0; i < fBuses.size(); i++)
        if (fBuses[i].id == id) return fNodePeakR[i].load(std::memory_order_relaxed);
    return 0.0f;
}

void Engine::UpdateLiveVoices(Frame blockStart) {
    fLiveNotes.clear();
    IMidiInput* in = fLiveMidi.load(std::memory_order_relaxed);
    if (!in) {                       // monitoring off: release every voice
        for (LiveVoice& v : fVoices) v.active = false;
        return;
    }

    // Drain events (RT-safe: stack buffer, lock-free ring). A note-on takes a
    // free voice (or steals the oldest); a note-off starts that voice's release.
    MidiEvent ev[64];
    std::size_t n;
    while ((n = in->ReadEvents(ev, 64)) > 0) {
        for (std::size_t i = 0; i < n; i++) {
            const MidiEvent& e = ev[i];
            if (e.IsNoteOn()) {
                int slot = -1;
                for (int k = 0; k < kMaxLiveVoices; k++)
                    if (!fVoices[k].active) { slot = k; break; }
                if (slot < 0) {                       // steal the oldest voice
                    slot = 0;
                    for (int k = 1; k < kMaxLiveVoices; k++)
                        if (fVoices[k].start < fVoices[slot].start) slot = k;
                }
                fVoices[slot] = LiveVoice{ true, false, e.data1, e.data2,
                                           e.channel, blockStart, 0 };
            } else if (e.IsNoteOff()) {
                for (int k = 0; k < kMaxLiveVoices; k++)
                    if (fVoices[k].active && !fVoices[k].releasing
                        && fVoices[k].pitch == e.data1
                        && fVoices[k].channel == e.channel) {
                        fVoices[k].releasing = true;
                        fVoices[k].off       = blockStart;
                        break;
                    }
            }
        }
    }

    // Rebuild the note list: a held voice sustains (huge length); a releasing
    // voice ends at its note-off so the Synth rings out its release tail. Free
    // voices whose tail is long past (a generous fixed window; the pool is small).
    const Frame kHeld = (Frame)(3600.0f * fOutputRate);   // "still down"
    const Frame kTail = (Frame)(4.0f * fOutputRate);      // release lingers <= 4 s
    for (LiveVoice& v : fVoices) {
        if (!v.active) continue;
        if (v.releasing && blockStart - v.off > kTail) { v.active = false; continue; }
        Frame len = v.releasing ? (v.off - v.start) : kHeld;
        if (len < 1) len = 1;
        fLiveNotes.push_back(MidiNote{ (int)v.pitch, (int)v.vel, v.start, len });
    }
}

void Engine::FillBuffer(float* out, size_t frames) {
    std::memset(out, 0, frames * 2 * sizeof(float));   // stereo silence

    // Monitor-only: render just the live keyboard voices through each armed
    // MIDI track's instrument + fader + FX. No clips, no playhead advance.
    if (fMonitorOnly.load(std::memory_order_relaxed)) {
        const Frame bs = fMonFrame;
        fMonFrame += (Frame)frames;
        UpdateLiveVoices(bs);
        const size_t nfloats = frames * 2;
        for (size_t i = 0; i < fBuses.size(); i++) {        // reset track meters
            fNodePeakL[i].store(0.0f, std::memory_order_relaxed);
            fNodePeakR[i].store(0.0f, std::memory_order_relaxed);
        }
        float pl = 0.0f, pr = 0.0f;
        for (size_t idx = 0; idx < fBuses.size(); idx++) {
            Bus& b = fBuses[idx];
            if (!b.liveMonitor || fLiveNotes.empty()) continue;
            float* nb = fNodeBufs[idx].data();
            std::memset(nb, 0, nfloats * sizeof(float));
            fSynth.Render(fLiveNotes, b.instrument, nb, frames, bs, 1.0f);
            for (size_t i = 0; i < frames; i++) {
                nb[i * 2 + 0] *= b.midiGainL;
                nb[i * 2 + 1] *= b.midiGainR;
            }
            for (auto& fx : b.fx)
                if (fx) fx->Process(nb, static_cast<int>(frames));
            float npl = 0.0f, npr = 0.0f;                   // per-track meter
            for (size_t i = 0; i < frames; i++) {
                const float l = std::fabs(nb[i * 2 + 0]);
                const float r = std::fabs(nb[i * 2 + 1]);
                if (l > npl) npl = l;
                if (r > npr) npr = r;
            }
            fNodePeakL[idx].store(npl, std::memory_order_relaxed);
            fNodePeakR[idx].store(npr, std::memory_order_relaxed);
            for (size_t i = 0; i < nfloats; i++) out[i] += nb[i];
        }
        const float mg = fMasterGain.load(std::memory_order_relaxed);
        for (size_t i = 0; i < nfloats; i++) {
            if (mg != 1.0f) out[i] *= mg;
            const float a = std::fabs(out[i]);
            if (i & 1) { if (a > pr) pr = a; } else { if (a > pl) pl = a; }
        }
        fPeakL.store(pl); fPeakR.store(pr);
        return;
    }

    if (!fPlaying.load()) {
        fPeakL.store(0.0f); fPeakR.store(0.0f);
        return;
    }

    const Frame blockStart = fPlayhead.load();
    if (blockStart >= fEndFrame) {
        fFinished.store(true);   // main thread will Stop(); RT stays silent
        fPeakL.store(0.0f); fPeakR.store(0.0f);
        return;
    }

    // Live MIDI monitoring: drain incoming events and advance held voices, then
    // rebuild fLiveNotes for this block. Rendered per armed MIDI bus below.
    UpdateLiveVoices(blockStart);

    // Tempo-synced effects (e.g. a delay) follow the tempo MAP: push the BPM at
    // this block to every effect when it changes (RT-safe: integer retune, no
    // alloc). Guarded so it only fires when crossing a tempo change.
    const double bpm = fTempoMap.BpmAt(blockStart);
    if (bpm != fLastFxBpm) { SetFxTempo(bpm); fLastFxBpm = bpm; }

    // Routing graph: process nodes in topo order, each into its output bus or
    // the master. Node buffers accumulate upstream inputs across the pass.
    const size_t nfloats = frames * 2;
    for (size_t i = 0; i < fBuses.size(); i++) {
        std::memset(fNodeBufs[i].data(), 0, nfloats * sizeof(float));
        fNodePeakL[i].store(0.0f, std::memory_order_relaxed);   // muted -> 0
        fNodePeakR[i].store(0.0f, std::memory_order_relaxed);
    }
    for (size_t oi = 0; oi < fOrder.size(); oi++) {
        const size_t idx = fOrder[oi];
        Bus& b = fBuses[idx];
        if (!b.audible)
            continue;                       // muted / solo'd out: route nothing
        float* nb = fNodeBufs[idx].data();

        // Automation: drive this node's gain/pan from its lanes at the block
        // start (absolute, per block). Non-automated nodes keep their live
        // UpdateMix values. Constant across the block (fine at ~10 ms).
        if (b.hasAuto) {
            const float g = b.gainAuto.ValueAt(blockStart, b.statGain);
            const float p = b.panAuto.ValueAt(blockStart, b.statPan);
            if (b.isBus)
                EqualPowerGains(g, p, &b.busGainL, &b.busGainR);
            else if (!b.notes.empty())
                EqualPowerGains(g, p, &b.midiGainL, &b.midiGainR);
            for (TrackStream* s : b.streams)   // audio leaves
                s->SetGainPan(g, p);
        }

        for (TrackStream* s : b.streams)    // audio leaves (fader is per-stream)
            s->Mix(nb, frames, blockStart);
        const bool live = b.liveMonitor && !fLiveNotes.empty();
        if (!b.notes.empty() || live) {     // MIDI: render dry then fader
            if (!b.notes.empty())
                fSynth.Render(b.notes, b.instrument, nb, frames, blockStart, 1.0f);
            if (live)                        // live keyboard through this voice
                fSynth.Render(fLiveNotes, b.instrument, nb, frames, blockStart, 1.0f);
            for (size_t i = 0; i < frames; i++) {
                nb[i * 2 + 0] *= b.midiGainL;
                nb[i * 2 + 1] *= b.midiGainR;
            }
        }
        if (b.isBus) {                      // bus: fader on the summed upstream
            for (size_t i = 0; i < frames; i++) {
                nb[i * 2 + 0] *= b.busGainL;
                nb[i * 2 + 1] *= b.busGainR;
            }
        }
        // Effect-parameter automation: set each automated param from its lane
        // at the block start before the chain processes (per block, RT-safe).
        for (const FxAutoLane& fa : b.fxAuto) {
            if (fa.fxIndex < 0 || fa.fxIndex >= (int)b.fx.size()) continue;
            if (!b.fx[fa.fxIndex] || fa.lane.Count() == 0) continue;
            b.fx[fa.fxIndex]->SetParam(fa.slot, fa.lane.ValueAt(blockStart, 0.0f));
        }
        for (auto& fx : b.fx)
            if (fx) fx->Process(nb, static_cast<int>(frames));

        // Effect metering: if this bus's track is the editor's focus, copy the
        // whole chain's meters into flat storage for the UI.
        if (b.id == fMeterTrack.load(std::memory_order_relaxed))
            CaptureFxMeters(b.fx);

        // Per-node peak (post-FX) for the track meter.
        {
            float pl = 0.0f, pr = 0.0f;
            for (size_t i = 0; i < frames; i++) {
                const float l = std::fabs(nb[i * 2 + 0]);
                const float r = std::fabs(nb[i * 2 + 1]);
                if (l > pl) pl = l;
                if (r > pr) pr = r;
            }
            fNodePeakL[idx].store(pl, std::memory_order_relaxed);
            fNodePeakR[idx].store(pr, std::memory_order_relaxed);
        }

        // Aux sends: add this node's post-FX signal into each dest node buffer.
        // Topo order guarantees the dest is processed later, so it sees this.
        for (const auto& st : b.sendTargets) {
            float* db = fNodeBufs[st.first].data();
            const float lvl = st.second;
            for (size_t i = 0; i < nfloats; i++)
                db[i] += nb[i] * lvl;
        }

        // Route into the output bus, or the master (out).
        float* dst = out;
        if (b.output != kInvalidTrackId) {
            for (size_t j = 0; j < fBuses.size(); j++)
                if (fBuses[j].id == b.output) { dst = fNodeBufs[j].data(); break; }
        }
        for (size_t i = 0; i < nfloats; i++)
            dst[i] += nb[i];
    }

    // Master bus FX on the summed output (before gain/metering).
    for (auto& fx : fMasterFx)
        fx->Process(out, static_cast<int>(frames));
    // Effect metering for the master chain (UI focus sentinel = ~0).
    if (fMeterTrack.load(std::memory_order_relaxed) == ~(TrackId)0)
        CaptureFxMeters(fMasterFx);

    // Master gain on the summed output (before metering so the meter reflects
    // what actually leaves the engine).
    const float mg = fMasterGain.load(std::memory_order_relaxed);
    if (mg != 1.0f)
        for (size_t i = 0; i < nfloats; i++)
            out[i] *= mg;

    // Metronome click on top of the mix (not affected by master gain).
    if (fMetronomeOn.load(std::memory_order_relaxed))
        fMetronome.Render(out, frames, blockStart, 0.3f);

    // Input monitoring: mix the live input into the output. Equal rate is a
    // direct copy; otherwise a linear resampler (carried across blocks) brings
    // the source to the output rate. RT-safe: no allocation, no locks.
    if (fInputMonitor.load(std::memory_order_relaxed)) {
        IMonitorSource* src = fMonSource.load(std::memory_order_relaxed);
        const float mr = src ? src->MonitorRate() : 0.0f;
        if (src && std::fabs(mr - fOutputRate) < 1.0f
            && fMonBuf.size() >= nfloats) {
            const size_t got = src->ReadMonitor(fMonBuf.data(), nfloats);
            for (size_t i = 0; i < got; i++)
                out[i] += fMonBuf[i];
        } else if (src && mr > 0.0f && !fMonSrc.empty()) {
            const double step = (double)mr / fOutputRate;   // src frames / out
            size_t need = (size_t)(fMonPhase + step * frames);
            const size_t cap = fMonSrc.size() / 2;
            if (need > cap) need = cap;
            const size_t gotF = src->ReadMonitor(fMonSrc.data(), need * 2) / 2;
            size_t si = 0;
            for (size_t i = 0; i < frames; i++) {
                float nL, nR;
                if (si < gotF) { nL = fMonSrc[si * 2]; nR = fMonSrc[si * 2 + 1]; }
                else           { nL = fMonPrevL;       nR = fMonPrevR; }
                out[i * 2 + 0] += fMonPrevL + (nL - fMonPrevL) * (float)fMonPhase;
                out[i * 2 + 1] += fMonPrevR + (nR - fMonPrevR) * (float)fMonPhase;
                fMonPhase += step;
                while (fMonPhase >= 1.0) {
                    fMonPhase -= 1.0;
                    if (si < gotF) {
                        fMonPrevL = fMonSrc[si * 2]; fMonPrevR = fMonSrc[si * 2 + 1];
                        si++;
                    } else { fMonPhase = 0.0; break; }   // underrun: hold
                }
            }
        }
    }

    // Block peak per channel for the UI meters (arithmetic only, RT-safe).
    float pl = 0.0f, pr = 0.0f;
    for (size_t i = 0; i < frames; i++) {
        const float l = std::fabs(out[i * 2 + 0]);
        const float r = std::fabs(out[i * 2 + 1]);
        if (l > pl) pl = l;
        if (r > pr) pr = r;
    }
    fPeakL.store(pl);
    fPeakR.store(pr);

    // Master loudness on the true mix (post master-gain + metronome, pre
    // monitor). RT-safe: integrated is disabled, so no allocation.
    fLoudness.Process(out, static_cast<int>(frames));
    fLufsM.store(fLoudness.MomentaryLufs());
    fLufsS.store(fLoudness.ShortTermLufs());
    fTpDb.store(fLoudness.TruePeakDb());

    // Monitor section: applied AFTER metering so the meters show the true mix.
    if (fMonitorMono.load(std::memory_order_relaxed))
        for (size_t i = 0; i < frames; i++) {
            const float m = 0.5f * (out[i * 2 + 0] + out[i * 2 + 1]);
            out[i * 2 + 0] = out[i * 2 + 1] = m;
        }
    if (fMonitorDim.load(std::memory_order_relaxed))
        for (size_t i = 0; i < nfloats; i++)
            out[i] *= 0.1f;   // ~-20 dB

    fPlayhead.store(blockStart + static_cast<Frame>(frames));
}

} // namespace daw
