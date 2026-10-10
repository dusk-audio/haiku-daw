#include "Engine.h"

#include "../dsp/EffectFactory.h"
#include "../model/Crossfade.h"
#include "../model/MidiControl.h"

#include <MediaDefs.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <pmmintrin.h>   // DAZ (denormals-are-zero)
#include <xmmintrin.h>   // FTZ (flush-to-zero)
#endif

namespace daw {

// Flush-to-zero + denormals-are-zero on the current (audio) thread. Denormal
// float state in feedback lines (reverb/delay/EQ decay tails) otherwise traps
// into microcode on x86 and spikes CPU -> xruns. Cheap to set every callback.
static inline void EnableDenormalFlush() {
#if defined(__x86_64__) || defined(__i386__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

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

// How long a synchronous Load() waits for the worker. Generous: the caller is
// a prototype or the monitor path, and the point of the wait is to replace a
// blocking build, not to bound one.
static constexpr bigtime_t kSyncLoadTimeoutUs = 20000000;

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

// Re-align the ring with a playhead that has moved on since this stream's graph
// was built (an in-place rebuild published while the transport kept rolling).
// The ring's origin — the timeline frame of its next unread frame — is
// fStart + fSeekDelta (Prepare seeks the source by exactly that), and Mix
// consumes one ring frame per timeline frame inside the clip's window.
void TrackStream::RebaseTo(Frame timelineFrame) {
    const Frame skip = RebaseSkipFrames(fStart + fSeekDelta, fStart,
                                        fStart + fLength, timelineFrame);
    if (skip <= 0) return;
    // Two floats (L/R) per frame; Skip is the consumer-side O(1) advance. What
    // the ring does not hold yet becomes under-run debt, which Mix's resync
    // drains as the disk thread fills it — the same path an xrun takes.
    const size_t got = fRing.Skip((size_t)skip * 2);
    fSkipDebt += skip - (Frame)(got / 2);
}

void TrackStream::Mix(float* out, size_t frames, Frame blockStart) {
    // First block after this stream's graph became active: drop what the
    // transport moved past while the graph was being built. Nothing to do when
    // the graph was built for exactly this frame (play, seek, the sync path).
    if (fRebasePending) {
        fRebasePending = false;
        RebaseTo(blockStart);
    }

    const Frame clipEnd = fStart + fLength;
    // Snapshot the live params once per block.
    const float gl  = fGainL.load(std::memory_order_relaxed);
    const float gr  = fGainR.load(std::memory_order_relaxed);
    const bool  aud = fAudible.load(std::memory_order_relaxed);

    for (size_t i = 0; i < frames; i++) {
        const Frame ph = blockStart + static_cast<Frame>(i);
        if (ph < fStart || ph >= clipEnd)
            continue;   // outside the clip -> contribute nothing

        // Resync after an earlier underrun: the ring lags the timeline by the
        // frames we couldn't read. Drop that many stale frames now that data is
        // back, so late-arriving audio plays at the CURRENT position instead of
        // offset forever.
        while (fSkipDebt > 0) {
            float junk[2];
            if (fRing.Read(junk, 2) < 2) break;   // still starved
            fSkipDebt--;
        }

        float lr[2];
        if (fRing.Read(lr, 2) < 2) {
            fSkipDebt++;   // underrun: owe a drop; output silence this frame
            continue;
        }

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

// --- Engine::Graph ----------------------------------------------------

void Engine::Graph::ApplyRoutes(const std::vector<MidiInputRoute>& routes) {
    for (Bus& b : fBuses) {
        const MidiInputRoute r = RouteFor(routes, b.id);
        b.inEndpoint.store(r.endpoint, std::memory_order_relaxed);
        b.inChannel.store(r.channel, std::memory_order_relaxed);
    }
}

void Engine::Graph::UpdateLiveVoices(Engine& e, Frame blockStart) {
    fLiveNotes.clear();
    for (Bus& b : fBuses) b.liveNotes.clear();
    IMidiInput* in = e.fLiveMidi.load(std::memory_order_relaxed);
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
            const MidiEvent& evt = ev[i];
            if (evt.IsNoteOn()) {
                int slot = -1;
                for (int k = 0; k < kMaxLiveVoices; k++)
                    if (!fVoices[k].active) { slot = k; break; }
                if (slot < 0) {                       // steal the oldest voice
                    slot = 0;
                    for (int k = 1; k < kMaxLiveVoices; k++)
                        if (fVoices[k].start < fVoices[slot].start) slot = k;
                }
                fVoices[slot] = LiveVoice{ true, false, evt.data1, evt.data2,
                                           evt.channel, blockStart, 0, evt.source };
            } else if (evt.IsNoteOff()) {
                // Match the source too: the same key on a second keyboard is a
                // different voice, and letting its note-off close this one would
                // cut the first keyboard's note short.
                for (int k = 0; k < kMaxLiveVoices; k++)
                    if (fVoices[k].active && !fVoices[k].releasing
                        && fVoices[k].pitch == evt.data1
                        && fVoices[k].channel == evt.channel
                        && fVoices[k].source == evt.source) {
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
        // Reap a released voice once its tail has passed. `blockStart < v.off`
        // means the playhead jumped backward (loop) — reap it too so it can't
        // linger forever.
        if (v.releasing && (blockStart < v.off || blockStart - v.off > kTail)) {
            v.active = false; continue;
        }
        Frame len = v.releasing ? (v.off - v.start) : kHeld;
        if (len < 1) len = 1;
        const MidiNote note{ (int)v.pitch, (int)v.vel, v.start, len };
        fLiveNotes.push_back(note);
        // Hand the voice to each monitored bus whose route accepts it, so two
        // keyboards drive two tracks instead of both tracks hearing everything.
        // Reserved at build, so these push_backs never allocate on the RT thread.
        MidiEvent probe;
        probe.channel = v.channel;
        probe.source  = v.source;
        for (Bus& b : fBuses) {
            if (!b.liveMonitor) continue;
            if (!RouteAccepts(b.inEndpoint.load(std::memory_order_relaxed),
                              b.inChannel.load(std::memory_order_relaxed), probe))
                continue;
            if (b.liveNotes.size() < b.liveNotes.capacity())
                b.liveNotes.push_back(note);
        }
    }
}

// Monitor-only: render just the live keyboard voices through each armed MIDI
// track's instrument + fader + FX. No clips, no playhead advance.
void Engine::Graph::RenderMonitorOnly(Engine& e, float* out, size_t frames) {
    const Frame bs = fMonFrame;
    fMonFrame += (Frame)frames;
    UpdateLiveVoices(e, bs);
    const size_t nfloats = frames * 2;
    for (size_t i = 0; i < fBuses.size(); i++) {        // reset track meters
        fNodePeakL[i].store(0.0f, std::memory_order_relaxed);
        fNodePeakR[i].store(0.0f, std::memory_order_relaxed);
    }
    float pl = 0.0f, pr = 0.0f;
    for (size_t idx = 0; idx < fBuses.size(); idx++) {
        Bus& b = fBuses[idx];
        if (!b.liveMonitor || b.liveNotes.empty()) continue;
        float* nb = fNodeBufs[idx].data();
        std::memset(nb, 0, nfloats * sizeof(float));
        if (b.instrument)
            b.instrument->Render(b.liveNotes, nb, frames, bs, 1.0f);
        const float mgl = b.midiGainL.load(std::memory_order_relaxed);
        const float mgr = b.midiGainR.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; i++) {
            nb[i * 2 + 0] *= mgl;
            nb[i * 2 + 1] *= mgr;
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
    const float mg = e.fMasterGain.load(std::memory_order_relaxed);
    for (size_t i = 0; i < nfloats; i++) {
        if (mg != 1.0f) out[i] *= mg;
        if (!std::isfinite(out[i])) out[i] = 0.0f;   // never blast the DAC
        const float a = std::fabs(out[i]);
        if (i & 1) { if (a > pr) pr = a; } else { if (a > pl) pl = a; }
    }
    // Watched inserts publish here too, or an editor open while monitoring
    // would stop hearing about the generic panel: this branch is the only
    // block running then, and PublishFxWatchNow deliberately stands aside
    // whenever a callback is running.
    e.CaptureFxWatches(kInvalidTrackId, true, fMasterFx);
    for (Bus& b2 : fBuses) e.CaptureFxWatches(b2.id, false, b2.fx);
    e.fPeakL.store(pl); e.fPeakR.store(pr);
}

// The RT body. `e` carries the state that outlives this graph; everything else
// read here belongs to the graph and is only ever written by the RT thread or
// by an atomic live-parameter store from the UI.
void Engine::Graph::Render(Engine& e, float* out, size_t frames) {
    if (e.fMonitorOnly.load(std::memory_order_relaxed)) {
        RenderMonitorOnly(e, out, frames);
        return;
    }

    if (!e.fPlaying.load()) {
        e.fPeakL.store(0.0f); e.fPeakR.store(0.0f);
        return;
    }

    const Frame blockStart = e.fPlayhead.load();
    if (blockStart >= fEndFrame) {
        e.fFinished.store(true);   // main thread will Stop(); RT stays silent
        e.fPeakL.store(0.0f); e.fPeakR.store(0.0f);
        return;
    }

    // Live MIDI monitoring: drain incoming events and advance held voices, then
    // rebuild fLiveNotes for this block. Rendered per armed MIDI bus below.
    UpdateLiveVoices(e, blockStart);

    // Tempo-synced effects (e.g. a delay) follow the tempo MAP: push the BPM at
    // this block to every effect when it changes (RT-safe: integer retune, no
    // alloc). Guarded so it only fires when crossing a tempo change.
    const double bpm = fTempoMap.BpmAt(blockStart);
    if (bpm != fLastFxBpm) { e.ApplyTempo(*this, bpm); fLastFxBpm = bpm; }

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
        if (!b.audible.load(std::memory_order_relaxed)) {
            // Muted / solo'd out: route nothing, but a watched insert in this
            // chain still publishes. Its editor is open on screen either way,
            // and a knob turned there -- or automation running under the mute,
            // which the main thread keeps writing -- would otherwise not show
            // up until the track was unmuted.
            e.CaptureFxWatches(b.id, false, b.fx);
            continue;
        }
        float* nb = fNodeBufs[idx].data();

        // Automation: drive this node's gain/pan from its lanes at the block
        // start (absolute, per block). Non-automated nodes keep their live
        // UpdateMix values. Constant across the block (fine at ~10 ms).
        if (b.hasAuto) {
            const float g = b.gainAuto.ValueAt(blockStart, b.statGain);
            const float p = b.panAuto.ValueAt(blockStart, b.statPan);
            if (b.isBus) {
                float bgl, bgr; EqualPowerGains(g, p, &bgl, &bgr);
                b.busGainL.store(bgl, std::memory_order_relaxed);
                b.busGainR.store(bgr, std::memory_order_relaxed);
            } else if (!b.notes.empty() || b.liveMonitor) {
                // MIDI node with notes OR a live-monitor-only armed track: its
                // midiGain feeds the synth render, so automated gain/pan must
                // reach it even when the node carries no recorded notes.
                float mgl, mgr; EqualPowerGains(g, p, &mgl, &mgr);
                b.midiGainL.store(mgl, std::memory_order_relaxed);
                b.midiGainR.store(mgr, std::memory_order_relaxed);
            }
            for (TrackStream* s : b.streams)   // audio leaves
                s->SetGainPan(g, p);
        }

        for (TrackStream* s : b.streams)    // audio leaves (fader is per-stream)
            s->Mix(nb, frames, blockStart);
        const bool live = b.liveMonitor && !b.liveNotes.empty();
        if (!b.notes.empty() || live) {     // MIDI: render dry then fader
            if (!b.notes.empty()) {
                // CC7 (volume) x CC11 (expression) channel gain, placed by the
                // CC10 pan, per block. Live-monitor notes below stay centered at
                // unity — the channel controls belong to the clip's events.
                float cgl, cgr;
                MidiChannelGains(b.events, blockStart, &cgl, &cgr);
                // Glide from where the last block ended so a stepped controller
                // doesn't click at the block seam; snap on the first block after
                // a build/seek (chanL < 0) rather than sweeping from a stale value.
                const StereoGain to{cgl, cgr};
                const StereoGain from = (b.chanL < 0.0f) ? to
                                                         : StereoGain{b.chanL, b.chanR};
                if (b.instrument)
                    b.instrument->Render(b.notes, nb, frames, blockStart, from, to);
                b.chanL = cgl;
                b.chanR = cgr;
            }
            if (live)                        // live keyboard through this voice
                if (b.instrument)
                    b.instrument->Render(b.liveNotes, nb, frames, blockStart, 1.0f);
            const float mgl = b.midiGainL.load(std::memory_order_relaxed);
            const float mgr = b.midiGainR.load(std::memory_order_relaxed);
            for (size_t i = 0; i < frames; i++) {
                nb[i * 2 + 0] *= mgl;
                nb[i * 2 + 1] *= mgr;
            }
        }
        if (b.isBus) {                      // bus: fader on the summed upstream
            const float bgl = b.busGainL.load(std::memory_order_relaxed);
            const float bgr = b.busGainR.load(std::memory_order_relaxed);
            for (size_t i = 0; i < frames; i++) {
                nb[i * 2 + 0] *= bgl;
                nb[i * 2 + 1] *= bgr;
            }
        }
        // Effect-parameter automation: set each automated param from its lane
        // at the block start before the chain processes (per block, RT-safe).
        for (const FxAutoLane& fa : b.fxAuto) {
            if (fa.fxIndex < 0 || fa.fxIndex >= (int)b.fx.size()) continue;
            if (!b.fx[fa.fxIndex] || fa.lane.Count() == 0) continue;
            b.fx[fa.fxIndex]->SetParam(fa.slot, fa.lane.ValueAt(blockStart, 0.0f));
        }
        // Insert chain. Each slot honours its own bypass / wet-dry mix; the
        // plain fully-wet slot is a bare Process, as before.
        for (size_t fi = 0; fi < b.fx.size(); fi++)
            RunInsertSlot(b.fx[fi].get(), b.fxDryDelay[fi],
                      b.fxBypass[fi].load(std::memory_order_relaxed),
                      b.fxMix[fi].load(std::memory_order_relaxed),
                      nb, frames, fScratch.data());

        // Effect metering: if this bus's track is the editor's focus, copy the
        // whole chain's meters into flat storage for the UI.
        if (b.id == e.fMeterTrack.load(std::memory_order_relaxed))
            e.CaptureFxMeters(b.fx);

        // Same for the inserts open native editors are watching: their
        // parameter values, which automation has just written into the plugin's
        // control ports on this thread. Published here for the same reason the
        // meters are -- the UI must never touch an effect the audio thread runs.
        e.CaptureFxWatches(b.id, false, b.fx);

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

        // Aux sends: add this node's post-FX signal into each dest node buffer,
        // PDC-delayed to the dest's input latency. Topo order guarantees the
        // dest is processed later, so it sees this.
        for (auto& st : b.sendTargets)
            st.delay.ProcessAdd(nb, fNodeBufs[st.dest].data(), frames, st.level);

        // Route into the output bus, or the master (out), PDC-delayed to that
        // destination's input latency (a plain accumulate when nothing is latent).
        float* dst = out;
        if (b.output != kInvalidTrackId) {
            for (size_t j = 0; j < fBuses.size(); j++)
                if (fBuses[j].id == b.output) { dst = fNodeBufs[j].data(); break; }
        }
        b.outDelay.ProcessAdd(nb, dst, frames, 1.0f);
    }

    // Master bus FX on the summed output (before gain/metering).
    for (size_t fi = 0; fi < fMasterFx.size(); fi++)
        RunInsertSlot(fMasterFx[fi].get(), fMasterFxDelay[fi],
                  fMasterFxBypass[fi].load(std::memory_order_relaxed),
                  fMasterFxMix[fi].load(std::memory_order_relaxed),
                  out, frames, fScratch.data());
    // Effect metering for the master chain (UI focus sentinel = ~0).
    if (e.fMeterTrack.load(std::memory_order_relaxed) == ~(TrackId)0)
        e.CaptureFxMeters(fMasterFx);
    // Native editors on MASTER inserts watch through the same publishing.
    e.CaptureFxWatches(kInvalidTrackId, true, fMasterFx);

    // Master gain on the summed output (before metering so the meter reflects
    // what actually leaves the engine).
    const float mg = e.fMasterGain.load(std::memory_order_relaxed);
    if (mg != 1.0f)
        for (size_t i = 0; i < nfloats; i++)
            out[i] *= mg;

    // Metronome click on top of the mix (not affected by master gain).
    if (e.fMetronomeOn.load(std::memory_order_relaxed))
        fMetronome.Render(out, frames, blockStart, 0.3f);

    // Input monitoring: mix the live input into the output. Equal rate is a
    // direct copy; otherwise a linear resampler (carried across blocks) brings
    // the source to the output rate. RT-safe: no allocation, no locks.
    if (e.fInputMonitor.load(std::memory_order_relaxed)) {
        IMonitorSource* src = e.fMonSource.load(std::memory_order_relaxed);
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

    // Final safety pass: a single NaN/Inf from any effect or a torn gain write
    // would otherwise blast the DAC at full scale and poison the meters/LUFS
    // (max propagates NaN). Zero any non-finite sample before it leaves here.
    for (size_t i = 0; i < nfloats; i++)
        if (!std::isfinite(out[i])) out[i] = 0.0f;

    // Block peak per channel for the UI meters (arithmetic only, RT-safe).
    float pl = 0.0f, pr = 0.0f;
    for (size_t i = 0; i < frames; i++) {
        const float l = std::fabs(out[i * 2 + 0]);
        const float r = std::fabs(out[i * 2 + 1]);
        if (l > pl) pl = l;
        if (r > pr) pr = r;
    }
    e.fPeakL.store(pl);
    e.fPeakR.store(pr);

    // Master loudness on the true mix (post master-gain + metronome, pre
    // monitor). RT-safe: integrated is disabled, so no allocation.
    fLoudness.Process(out, static_cast<int>(frames));
    e.fLufsM.store(fLoudness.MomentaryLufs());
    e.fLufsS.store(fLoudness.ShortTermLufs());
    e.fTpDb.store(fLoudness.TruePeakDb());

    // Monitor section: applied AFTER metering so the meters show the true mix.
    if (e.fMonitorMono.load(std::memory_order_relaxed))
        for (size_t i = 0; i < frames; i++) {
            const float m = 0.5f * (out[i * 2 + 0] + out[i * 2 + 1]);
            out[i * 2 + 0] = out[i * 2 + 1] = m;
        }
    if (e.fMonitorDim.load(std::memory_order_relaxed))
        for (size_t i = 0; i < nfloats; i++)
            out[i] *= 0.1f;   // ~-20 dB

    e.fPlayhead.store(blockStart + static_cast<Frame>(frames));
}

// --- Engine -----------------------------------------------------------

Engine::Engine() {
    for (int i = 0; i < kMeterFxMax; i++)
        fMeterGr[i].store(0.0f, std::memory_order_relaxed);
    // A watch slot that has published nothing yet -- which is what
    // WatchedFxParams reports as "0 values, generation untouched" rather
    // than "0 values, generation 0".
    for (int s = 0; s < kWatchSlots; s++)
        fWatchN[s].store(-1, std::memory_order_relaxed);
    // The graph slot + its reclaim thread. The predicate is the engine's proof
    // that no thread can still be holding a retired graph:
    //   - no off-RT user is inside it (GraphPin — see the header),
    //   - and either the player is not running (fPlayerRunning is cleared only
    //     after BSoundPlayer::Stop() has returned, i.e. no callback is in
    //     flight), or two callback boundaries have passed since the last
    //     publish — the callback that may have loaded the old pointer has run
    //     to completion (the same proof QuiesceMonitorInput uses).
    fGraphs.reset(new GraphSwap<Graph>([this] {
        if (fGraphUsers.load(std::memory_order_acquire) != 0) return false;
        if (!fPlayerRunning.load(std::memory_order_acquire)) return true;
        return fCallbackGen.load(std::memory_order_acquire)
               >= fRetireGen.load(std::memory_order_relaxed) + 2;
    }));
    fBuilder = std::thread(&Engine::BuilderLoop, this);
}

Engine::~Engine() {
    // 1. Stop taking requests; a build in flight aborts at its next checkpoint
    //    (BuildGraph checks between clips) instead of being waited out.
    {
        std::lock_guard<std::mutex> lk(fBuildMutex);
        fShutdown.store(true, std::memory_order_release);
        fRequest.reset();
    }
    fBuildCv.notify_all();
    if (fBuilder.joinable()) fBuilder.join();
    // 2. Quiesce the RT side. After this no callback can be running, so the
    //    reclaimer's predicate is true and the graph slot frees at once.
    Stop();
    fGraphs.reset();
    // 3. fPlayer dies with the object; the device is closed here.
}

status_t Engine::EnsurePlayer(const Project& project) {
    if (fPlayer && fPlayerRequestRate == project.sampleRate
        && fPlayerRequestFrames == fBufferFrames)
        return B_OK;

    // Re-create for a different rate/buffer (or the first time). The old player
    // is destroyed first: the caller only gets here with the transport stopped
    // (play/monitor start, or a fresh engine).
    fPlayer.reset();
    media_raw_audio_format format = media_raw_audio_format::wildcard;
    format.frame_rate    = project.sampleRate;
    format.channel_count = 2;
    format.format        = media_raw_audio_format::B_AUDIO_FLOAT;
    format.byte_order    = B_MEDIA_HOST_ENDIAN;
    // Request an explicit buffer size (frames * channels * 4 bytes) instead of
    // letting media_server suggest one. Smaller = lower latency, more xrun risk.
    format.buffer_size   = fBufferFrames * 2 * sizeof(float);

    std::unique_ptr<BSoundPlayer> p(new BSoundPlayer(
        &format, "haiku_daw", PlayTrampoline, nullptr, this));
    const status_t err = p->InitCheck();
    if (err != B_OK) {
        fprintf(stderr, "Engine: BSoundPlayer init failed: %s\n", strerror(err));
        return err;
    }
    fPlayer = std::move(p);
    fPlayerRequestRate   = project.sampleRate;
    fPlayerRequestFrames = fBufferFrames;
    fOutputRate          = fPlayer->Format().frame_rate;
    fPlayerBufferBytes   = fPlayer->Format().buffer_size;
    fPlayersOpened.fetch_add(1, std::memory_order_relaxed);
    return B_OK;
}

void Engine::RequestLoad(const Project& project, Frame startFrame,
                         Frame minEndFrame, LoadMode mode) {
    const uint64_t gen = fRequestGen.fetch_add(1, std::memory_order_acq_rel) + 1;

    // The device first: a build whose player cannot open is a completed failed
    // load, not something to hand the worker.
    const status_t dev = EnsurePlayer(project);
    if (dev != B_OK) {
        {
            std::lock_guard<std::mutex> lk(fBuildMutex);
            fLoadStatus.store((int)dev, std::memory_order_relaxed);
            fCompletedGen.store(gen, std::memory_order_release);
        }
        fBuildCv.notify_all();
        return;
    }

    // A reposition detaches the active graph NOW: its content belongs to the
    // old position, so playing it under the new playhead would be wrong (and a
    // seek did not play on either). The RT then renders silence, and does not
    // advance the playhead, until the new graph is published.
    if (mode == LoadMode::NewPosition) DetachGraph();

    fStartFrame = startFrame;   // Start() (same thread) reads this
    // The engine now outlives a load, so anything the old Load reset from the
    // model must be reset here too, or a load would inherit the previous
    // project's value until the first UpdateMix poll.
    fMasterGain.store(project.masterGain, std::memory_order_relaxed);

    auto req = std::unique_ptr<BuildRequest>(new BuildRequest());
    req->project       = project;          // the snapshot: see BuildRequest
    req->startFrame    = startFrame;
    req->minEndFrame   = minEndFrame;
    req->mode          = mode;
    req->monitorOnly   = fMonitorOnly.load(std::memory_order_relaxed);
    req->gen           = gen;
    req->outputRate    = fOutputRate;
    req->playerBufferBytes = fPlayerBufferBytes;
    {
        std::lock_guard<std::mutex> lk(fBuildMutex);
        fRequest = std::move(req);         // newest wins; a queued one is dropped
    }
    fBuildCv.notify_one();
}

bool Engine::WaitForLoad(bigtime_t timeoutUs) {
    const uint64_t want = fRequestGen.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> lk(fBuildMutex);
    return fBuildCv.wait_for(lk, std::chrono::microseconds(timeoutUs), [&] {
        return fCompletedGen.load(std::memory_order_acquire) >= want;
    });
}

status_t Engine::Load(const Project& project, Frame startFrame,
                      Frame minEndFrame) {
    const status_t dev = EnsurePlayer(project);
    if (dev != B_OK) return dev;
    RequestLoad(project, startFrame, minEndFrame, LoadMode::NewPosition);
    if (!WaitForLoad(kSyncLoadTimeoutUs)) return B_TIMED_OUT;
    return LastLoadStatus();
}

void Engine::BuilderLoop() {
    for (;;) {
        std::unique_ptr<BuildRequest> req;
        {
            std::unique_lock<std::mutex> lk(fBuildMutex);
            fBuildCv.wait(lk, [this] {
                return fShutdown.load(std::memory_order_acquire)
                       || fRequest != nullptr;
            });
            if (fShutdown.load(std::memory_order_acquire) || !fRequest)
                return;
            req = std::move(fRequest);
        }

        status_t rc = B_OK;
        std::unique_ptr<Graph> g = BuildGraph(*req, &rc);

        // A newer request came in while this one built: publishing this graph
        // would hand the RT a stale snapshot (and, for a reposition, the wrong
        // position). Discard it here — this is the builder thread, so the
        // streams' disk threads are joined here, never on the RT thread.
        const bool superseded =
            fRequestGen.load(std::memory_order_acquire) != req->gen
            || fShutdown.load(std::memory_order_acquire);
        if (g) {
            if (superseded) { g.reset(); rc = B_CANCELED; }
            else            PublishGraph(std::move(g));
        }
        PublishCompletion(req->gen, rc);
    }
}

void Engine::PublishCompletion(uint64_t gen, status_t rc) {
    {
        std::lock_guard<std::mutex> lk(fBuildMutex);
        fLoadStatus.store((int)rc, std::memory_order_relaxed);
        fCompletedGen.store(gen, std::memory_order_release);
    }
    fBuildCv.notify_all();
}

void Engine::PublishGraph(std::unique_ptr<Graph> g) {
    {
        // Routes may have changed while this graph was building: the buses get
        // the list as of PUBLICATION, so the graph can never go live with a
        // stale demux.
        std::lock_guard<std::mutex> lk(fRouteMutex);
        g->ApplyRoutes(fMidiRoutes);
    }
    // Sample the boundary counter BEFORE the exchange: a callback that started
    // earlier may still hold the graph we are about to retire, and it will bump
    // this counter when it finishes.
    fRetireGen.store(fCallbackGen.load(std::memory_order_acquire),
                     std::memory_order_relaxed);
    fGraphs->Publish(std::move(g));
    fGraphsPublished.fetch_add(1, std::memory_order_relaxed);
}

void Engine::DetachGraph() {
    fRetireGen.store(fCallbackGen.load(std::memory_order_acquire),
                     std::memory_order_relaxed);
    fGraphs->Detach();
}

void Engine::Start() {
    if (!fPlayer) return;   // no device: the caller has already reported it
    fPlayhead.store(fStartFrame);
    GraphPin pin(*this);
    Graph* g = pin.Active();
    fFinished.store(g != nullptr && fStartFrame >= g->EndFrame());
    if (g) g->fMonFrame = fStartFrame;   // monitor-only clock restarts here
    // The loudness readout starts from silence, as it did when every start was
    // preceded by a load (the RT replaces these on its first block).
    fLufsM.store(Loudness::kSilenceLufs, std::memory_order_relaxed);
    fLufsS.store(Loudness::kSilenceLufs, std::memory_order_relaxed);
    fTpDb.store(Loudness::kSilenceDb, std::memory_order_relaxed);
    fPlaying.store(true);
    fPlayerRunning.store(true, std::memory_order_release);   // callbacks may run now
    fPlayer->SetHasData(true);
    fPlayer->Start();
    fPlayerStarts.fetch_add(1, std::memory_order_relaxed);
}

void Engine::Stop() {
    fPlaying.store(false);
    if (fPlayer)
        fPlayer->Stop();   // blocks until the last callback returns: RT quiesced
    // Only now — after fPlayer->Stop() has drained the last callback — is it
    // true that no callback can be in flight. QuiesceMonitorInput() keys its
    // fast path off this, NOT fPlaying (which was cleared above, before Stop).
    fPlayerRunning.store(false, std::memory_order_release);
    // MIDI panic: release every live monitor voice so a note held at stop can't
    // sustain (up to kHeld ~1 h) or re-sound if the same engine restarts. Safe
    // now that the player is stopped and no callback is running.
    GraphPin pin(*this);
    if (Graph* g = pin.Active()) g->ResetVoices();
}

void Engine::PlayTrampoline(void* cookie, void* buffer, size_t size,
                            const media_raw_audio_format& format) {
    Engine* self = static_cast<Engine*>(cookie);
    const size_t frames = size / (sizeof(float) * format.channel_count);
    self->FillBuffer(static_cast<float*>(buffer), frames);
    // Mark this callback complete. QuiesceMonitorInput() waits on this so a
    // caller can safely destroy a monitor/live-MIDI source it just detached,
    // once any in-flight dereference of the old pointer has finished; the graph
    // reclaimer waits on it too (a retired graph is freed two boundaries later).
    self->fCallbackGen.fetch_add(1, std::memory_order_release);
}

void Engine::FillBuffer(float* out, size_t frames) {
    EnableDenormalFlush();                             // RT-safe, per callback
    std::memset(out, 0, frames * 2 * sizeof(float));   // stereo silence

    // ONE acquire load for the whole block: whatever the builder publishes
    // meanwhile, this block is rendered from one graph from start to finish.
    Graph* g = fGraphs->Active();
    if (!g) {
        // No graph yet (a rebuild is in flight, or the transport was just
        // repositioned): silence, and do NOT advance the playhead — the
        // transport must not run on ahead of the audio.
        fPeakL.store(0.0f);
        fPeakR.store(0.0f);
        return;
    }
    g->Render(*this, out, frames);
}

bool Engine::QuiesceMonitorInput() {
    // Fast path keys off fPlayerRunning, which is false only when the player is
    // confirmed stopped (fPlayer->Stop() returned) or never started — a real
    // proof that no callback is in flight. (fPlaying is cleared at the TOP of
    // Stop(), before the player drains, so it is NOT such a proof.)
    if (!fPlayer || !fPlayerRunning.load(std::memory_order_acquire))
        return true;
    // Wait for two full callback boundaries: guarantees the callback that may
    // have loaded the now-detached pointer has run to completion. Bounded so a
    // stalled/dead callback can't hang the UI thread (~1 s worst case) — but on
    // timeout we could NOT prove quiescence, so report failure. The caller must
    // then fall back to Stop() (which blocks until the RT thread truly quiesces)
    // before destroying the source; never free it on a false return.
    const uint64_t start = fCallbackGen.load(std::memory_order_acquire);
    for (int spin = 0; spin < 2000; ++spin) {
        if (fCallbackGen.load(std::memory_order_acquire) - start >= 2)
            return true;
        snooze(500);   // 0.5 ms; callbacks are ~ms, so a few iterations at most
    }
    return false;   // quiescence NOT confirmed — caller must Stop() before freeing
}

float Engine::TrackPeakL(TrackId id) const {
    GraphPin pin(*this);
    Graph* g = pin.Active();
    if (!g || !g->fNodePeakL) return 0.0f;
    for (size_t i = 0; i < g->fBuses.size(); i++)
        if (g->fBuses[i].id == id)
            return g->fNodePeakL[i].load(std::memory_order_relaxed);
    return 0.0f;
}
float Engine::TrackPeakR(TrackId id) const {
    GraphPin pin(*this);
    Graph* g = pin.Active();
    if (!g || !g->fNodePeakR) return 0.0f;
    for (size_t i = 0; i < g->fBuses.size(); i++)
        if (g->fBuses[i].id == id)
            return g->fNodePeakR[i].load(std::memory_order_relaxed);
    return 0.0f;
}

void Engine::SetMidiRoutes(const std::vector<MidiInputRoute>& routes) {
    {
        std::lock_guard<std::mutex> lk(fRouteMutex);
        fMidiRoutes = routes;
    }
    // A graph being built picks the list up at publication (PublishGraph); the
    // live one is updated here, atomically per bus.
    GraphPin pin(*this);
    if (Graph* g = pin.Active()) g->ApplyRoutes(routes);
}

void Engine::UpdateMix(const Project& project) {
    fMasterGain.store(project.masterGain, std::memory_order_relaxed);
    GraphPin pin(*this);
    Graph* g = pin.Active();
    if (!g) return;   // a rebuild is in flight: the model reaches the next graph
    bool anySolo = false;
    for (const Track& t : project.Tracks())
        if (t.soloed && !t.muted)           // any track type can solo
            anySolo = true;

    for (const Track& t : project.Tracks()) {
        if (t.type != TrackType::Audio)
            continue;
        const bool audible = !t.muted && (!anySolo || t.soloed || t.soloSafe);
        // Automated tracks: automation owns gain/pan (driven per block in
        // Render); only refresh audibility here so live mute/solo still work.
        const bool automated = t.gainAuto.Count() > 0 || t.panAuto.Count() > 0;
        for (auto& s : g->fStreams)
            if (s->Track() == t.id) {
                if (automated) s->SetAudible(audible);
                else           s->SetMix(t.gain, t.pan, audible);
            }
    }

    // MIDI + bus nodes: update gain/pan/audibility live (the RT reads these
    // atomics — a torn write is at most a one-block transient, benign).
    for (Graph::Bus& b : g->fBuses) {
        const Track* t = project.FindTrack(b.id);
        if (!t) continue;
        const bool audible = !t->muted && (!anySolo || t->soloed || t->soloSafe);
        const bool automated = t->gainAuto.Count() > 0 || t->panAuto.Count() > 0;
        if (t->type == TrackType::Midi) {
            b.audible.store(audible || b.liveMonitor, std::memory_order_relaxed);  // keep monitored input audible
            if (!automated) {
                float mgl, mgr; EqualPowerGains(t->gain, t->pan, &mgl, &mgr);
                b.midiGainL.store(mgl, std::memory_order_relaxed);
                b.midiGainR.store(mgr, std::memory_order_relaxed);
            }
        } else if (b.isBus) {
            b.audible.store(audible, std::memory_order_relaxed);
            if (!automated) {
                float bgl, bgr; EqualPowerGains(t->gain, t->pan, &bgl, &bgr);
                b.busGainL.store(bgl, std::memory_order_relaxed);
                b.busGainR.store(bgr, std::memory_order_relaxed);
            }
        }
    }
}

bool Engine::SyncFx(const Project& project) {
    // Push each effect's params from the model into the live objects. SetParam
    // is RT-safe (the RT thread may run concurrently). A chain whose structure
    // changed (different types/count) can't be updated in place -> report it so
    // the caller triggers a rebuild. With no active graph (a rebuild in flight)
    // there is nothing to sync and the graph being built predates the model, so
    // report a mismatch and let the caller request one.
    GraphPin pin(*this);
    Graph* g = pin.Active();
    if (!g) return false;
    bool allMatched = true;
    auto sync = [&](std::vector<std::unique_ptr<IEffect>>& fx,
                    const std::vector<EffectType>& types,
                    const std::vector<EffectDesc>& descs,
                    std::atomic<bool>* bypass, std::atomic<float>* mix) {
        if (fx.size() != descs.size()) { allMatched = false; return; }
        for (size_t i = 0; i < fx.size(); i++) {
            if (i >= types.size() || types[i] != descs[i].type) {
                allMatched = false;   // an effect was replaced at this slot
                continue;
            }
            // Insert bypass / wet-dry are PUSHABLE, like params: they change
            // what the host does around the effect, not the chain's structure —
            // and, because bypass is soft, not its reported latency either. So
            // they never count as a structural mismatch and a toggle never
            // forces a rebuild. Pushed even for a nullptr slot (an unavailable
            // plugin) so the state is already right if the chain is rebuilt.
            if (bypass) bypass[i].store(descs[i].bypassed,
                                        std::memory_order_relaxed);
            if (mix)    mix[i].store(ClampFxMix(descs[i].mix),
                                     std::memory_order_relaxed);
            if (!fx[i]) continue;
            for (size_t s = 0; s < descs[i].params.size(); s++)
                fx[i]->SetParam((int)s, descs[i].params[s]);
        }
    };
    for (Graph::Bus& b : g->fBuses) {
        const Track* t = project.FindTrack(b.id);
        if (t) sync(b.fx, b.fxTypes, t->fx, b.fxBypass.get(), b.fxMix.get());
    }
    sync(g->fMasterFx, g->fMasterFxTypes, project.masterFx,
         g->fMasterFxBypass.get(), g->fMasterFxMix.get());
    return allMatched;
}

void Engine::ApplyTempo(Graph& g, double bpm) {
    for (Graph::Bus& b : g.fBuses)
        for (auto& fx : b.fx)
            if (fx) fx->SetTempo(bpm);
    for (auto& fx : g.fMasterFx)
        if (fx) fx->SetTempo(bpm);
}

void Engine::SetFxTempo(double bpm) {
    GraphPin pin(*this);
    if (Graph* g = pin.Active()) ApplyTempo(*g, bpm);
}

void Engine::SetFxParamLive(TrackId track, bool master, int fxIndex, int slot,
                            float value) {
    GraphPin pin(*this);
    Graph* g = pin.Active();
    if (!g) return;
    std::vector<std::unique_ptr<IEffect>>* chain = nullptr;
    if (master) {
        chain = &g->fMasterFx;
    } else {
        for (Graph::Bus& b : g->fBuses)
            if (b.id == track) { chain = &b.fx; break; }
    }
    if (!chain || fxIndex < 0 || fxIndex >= (int)chain->size()) return;
    if ((*chain)[(size_t)fxIndex])
        (*chain)[(size_t)fxIndex]->SetParam(slot, value);
}

void Engine::PublishFxWatchNow() {
    // The audio callback owns the published storage while it runs (it rewrites
    // the same arrays every block); this exists for the stopped transport, where
    // there is no block and no second writer.
    if (fPlayerRunning.load(std::memory_order_acquire)) return;
    GraphPin pin(*this);
    Graph* g = pin.Active();
    if (!g) return;   // no graph yet: the model is the only source of truth
    CaptureFxWatches(kInvalidTrackId, true, g->fMasterFx);
    for (Graph::Bus& b : g->fBuses)
        CaptureFxWatches(b.id, false, b.fx);
}

// --- the build (builder thread) ---------------------------------------

std::unique_ptr<Engine::Graph> Engine::BuildGraph(const BuildRequest& req,
                                                  status_t* rc) {
    const Project& project = req.project;
    *rc = B_OK;
    auto graph = std::unique_ptr<Graph>(new Graph());
    Graph& g = *graph;
    g.fBuildStart = req.startFrame;
    g.fEndFrame   = 0;
    g.fOutputRate = req.outputRate;
    g.fMonFrame   = req.startFrame;   // free-running clock for monitor-only mode
    // The old Load opened the device on every call; the graph is now built for
    // the format the (persistent) player already has.
    g.fMetronome = Metronome(g.fOutputRate, project.tempoBPM,
                             project.timeSig.numerator);

    // Solo overrides mute: if any track (audio or MIDI) is soloed, only
    // soloed (non-muted) tracks play.
    bool anySolo = false;
    for (const Track& t : project.Tracks())
        if (t.soloed && !t.muted)
            anySolo = true;

    // Build one stream per audio clip. All clips get a stream (even muted /
    // non-soloed) so mute/solo can be toggled live; audibility is a per-stream
    // flag the RT mix honors, not a build-time filter.
    const bool monitorOnly = req.monitorOnly;
    for (const Track& t : project.Tracks()) {
        if (t.type != TrackType::Audio || monitorOnly)
            continue;   // monitor-only: no clip streams (no disk I/O)
        const bool audible = !t.muted && (!anySolo || t.soloed || t.soloSafe);
        const std::vector<ClipFades> fades = ComputeCrossfades(t.clips);
        for (size_t ci = 0; ci < t.clips.size(); ci++) {
            const Clip& c = t.clips[ci];
            if (c.sourcePath.empty())
                continue;
            if (c.takeGroup > 0 && !c.takeActive)
                continue;   // inactive loop-record take: don't stream it
            // Check between clips: a quit during a long build must not wait it
            // out. The partial graph dies here, on this thread.
            if (fShutdown.load(std::memory_order_acquire)) {
                *rc = B_CANCELED;
                return nullptr;
            }
            // How far into the clip (in timeline frames) playback starts.
            const Frame seekDelta =
                (req.startFrame > c.startFrame) ? req.startFrame - c.startFrame : 0;
            auto s = std::make_unique<TrackStream>(
                t.id, c.sourcePath, c.startFrame, c.lengthFrames,
                c.sourceOffset, t.gain, t.pan, audible, seekDelta,
                g.fOutputRate, fades[ci].fadeIn, fades[ci].fadeOut, c.gain);
            if (s->Prepare() != B_OK || !s->Valid()) {
                fprintf(stderr, "Engine: skipping clip '%s'\n",
                        c.sourcePath.c_str());
                continue;
            }
            // Sources are resampled to the output rate on their disk thread.
            if (s->EndFrame() > g.fEndFrame)
                g.fEndFrame = s->EndFrame();
            g.fStreams.push_back(std::move(s));
        }
    }

    // Group audio streams into per-track buses.
    for (auto& s : g.fStreams) {
        Graph::Bus* bus = nullptr;
        for (Graph::Bus& b : g.fBuses)
            if (b.id == s->Track()) { bus = &b; break; }
        if (!bus) {
            g.fBuses.push_back(Graph::Bus{});
            g.fBuses.back().id = s->Track();
            bus = &g.fBuses.back();
        }
        bus->streams.push_back(s.get());
    }

    // Live-monitor state: reset the held-voice pool for this build and make
    // sure the note scratch has capacity so the RT rebuild never allocates.
    g.ResetVoices();
    g.fLiveNotes.reserve(Graph::kMaxLiveVoices);

    // Add a bus per audible MIDI track: snapshot its notes for the synth to
    // render (rebuild-on-play, so MIDI edits apply on the next build). An armed
    // MIDI track also gets a bus even with no notes, so live input can be
    // monitored (synthesized) through its instrument while recording.
    for (const Track& t : project.Tracks()) {
        if (t.type != TrackType::Midi)
            continue;
        std::vector<MidiNote> notes = t.CollectNotes();   // absolute-timeline
        const bool monitor = t.armed || t.inputMonitor;   // live input synth
        const bool audible = !t.muted && (!anySolo || t.soloed || t.soloSafe);
        if ((notes.empty() && !monitor) || (!audible && !monitor))
            continue;
        Graph::Bus b;
        b.id    = t.id;
        for (const MidiNote& n : notes)
            if (n.startFrame + n.lengthFrames > g.fEndFrame)
                g.fEndFrame = n.startFrame + n.lengthFrames;
        b.notes = std::move(notes);
        b.events = t.CollectEvents();         // channel CC/PB (absolute frames)
        // The voice is built here, off the RT thread. A soundfont one resolves
        // against the SoundfontCache and never decodes — see InstrumentFactory.
        b.instrument = MakeInstrument(t.instrument, g.fOutputRate);
        b.liveMonitor = monitor;             // synth live input into this bus
        // Reserve this bus's share of the live voices up front:
        // UpdateLiveVoices fills it on the RT thread every block and must never
        // allocate there.
        b.liveNotes.reserve(Graph::kMaxLiveVoices);
        b.audible.store(audible || monitor, std::memory_order_relaxed);  // monitor overrides mute/solo
        float mgl, mgr; EqualPowerGains(t.gain, t.pan, &mgl, &mgr);
        b.midiGainL.store(mgl, std::memory_order_relaxed);
        b.midiGainR.store(mgr, std::memory_order_relaxed);
        g.fBuses.push_back(std::move(b));
    }

    // Add a node per bus track (always, even muted, so it stays a valid
    // routing target; a muted bus just doesn't route its sum onward).
    for (const Track& t : project.Tracks()) {
        if (t.type != TrackType::Bus)
            continue;
        Graph::Bus b;
        b.id    = t.id;
        b.isBus = true;
        float bgl, bgr; EqualPowerGains(t.gain, t.pan, &bgl, &bgr);
        b.busGainL.store(bgl, std::memory_order_relaxed);
        b.busGainR.store(bgr, std::memory_order_relaxed);
        g.fBuses.push_back(std::move(b));
    }

    // No content is fine only if the caller extends the range (metronome/loop).
    bool anyContent = false;
    for (const Graph::Bus& b : g.fBuses)
        if (!b.streams.empty() || !b.notes.empty()) { anyContent = true; break; }
    if (!anyContent && req.minEndFrame <= 0) {
        // Distinct from a device failure, so the UI can tell "there is nothing
        // to play" (not worth an alert) from "the device did not open".
        fprintf(stderr, "Engine: nothing to play\n");
        *rc = B_ENTRY_NOT_FOUND;
        return nullptr;
    }

    // Extend the play range past content for looping / a metronome-only run.
    if (req.minEndFrame > g.fEndFrame)
        g.fEndFrame = req.minEndFrame;

    // Set each node's routing output + audibility, and build its effect chain.
    for (Graph::Bus& b : g.fBuses) {
        const Track* t = project.FindTrack(b.id);
        if (!t) continue;
        b.output  = t->output;
        // Audio leaves are gated live by their streams (so live mute/unmute
        // works); a bus node is gated here (rebuild-on-play) and skips routing
        // its sum when muted / solo'd out. Only recompute audibility for bus
        // nodes — a non-bus node keeps the audibility set when it was created
        // (a MIDI node's audible||monitor, an audio leaf's default true), so a
        // muted MIDI node stays inaudible until the first UpdateMix() poll
        // instead of being force-unmuted here.
        if (b.isBus)
            b.audible.store(!t->muted && (!anySolo || t->soloed || t->soloSafe),
                            std::memory_order_relaxed);
        // Keep b.fx index-aligned with t->fx (nullptr placeholder for any
        // effect that fails to build, e.g. a missing plugin add-on) so
        // fxAuto's fxIndex addresses the right effect. Process skips nulls.
        for (const EffectDesc& d : t->fx) {
            auto fx = MakeEffect(d, g.fOutputRate);
            if (fx) { fx->Prepare(g.fOutputRate); fx->SetTempo(project.tempoBPM); }
            b.fx.push_back(std::move(fx));
            b.fxTypes.push_back(d.type);
        }
        // Per-insert slot state, sized once the chain is built (arrays of
        // atomics, so they are allocated rather than grown). The dry-delay line
        // is sized from the effect's OWN latency, read after Prepare() — the
        // point at which IEffect guarantees it is fixed — because that is what
        // both the soft-bypass path and the wet/dry blend realign against.
        if (!b.fx.empty()) {
            b.fxBypass.reset(new std::atomic<bool>[b.fx.size()]);
            b.fxMix.reset(new std::atomic<float>[b.fx.size()]);
            b.fxDryDelay.assign(b.fx.size(), FrameDelay{});
            for (size_t i = 0; i < b.fx.size(); i++) {
                b.fxBypass[i].store(t->fx[i].bypassed, std::memory_order_relaxed);
                b.fxMix[i].store(ClampFxMix(t->fx[i].mix), std::memory_order_relaxed);
                const int lat = b.fx[i] ? b.fx[i]->LatencySamples() : 0;
                b.fxDryDelay[i].Prepare(lat > 0 ? (size_t)lat : 0);
            }
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
        for (size_t i = 0; i < g.fBuses.size(); i++)
            if (g.fBuses[i].id == id) return (long)i;
        return -1;
    };
    for (Graph::Bus& b : g.fBuses) {
        const Track* t = project.FindTrack(b.id);
        if (!t) continue;
        for (const Send& s : t->sends) {
            if (s.dest == kInvalidTrackId || s.dest == b.id) continue;
            const long di = nodeIndexOf(s.dest);
            if (di >= 0) {
                Graph::Bus::SendTarget st;
                st.dest = (size_t)di;
                st.level = s.level;
                b.sendTargets.push_back(std::move(st));
            }
        }
    }

    // Topological processing order: a node before every node it feeds — its
    // output AND every send destination. Sends add extra edges, so use the
    // general edge topo (single-output ResolveRoutingOrder can't express them).
    {
        std::vector<TrackId> nids;
        std::vector<std::pair<TrackId, TrackId>> edges;
        nids.reserve(g.fBuses.size());
        for (const Graph::Bus& b : g.fBuses) {
            nids.push_back(b.id);
            edges.push_back({b.id, b.output});
            const Track* t = project.FindTrack(b.id);
            if (t)
                for (const Send& s : t->sends)
                    if (s.dest != kInvalidTrackId && s.dest != b.id)
                        edges.push_back({b.id, s.dest});   // skip self-send edge
        }
        std::vector<TrackId> ord;
        if (ResolveOrderWithEdges(nids, edges, ord)) {
            for (TrackId id : ord)
                for (size_t i = 0; i < g.fBuses.size(); i++)
                    if (g.fBuses[i].id == id) { g.fOrder.push_back(i); break; }
        } else {   // cycle / bad graph: flat order, all to master
            for (size_t i = 0; i < g.fBuses.size(); i++) g.fOrder.push_back(i);
        }
    }

    // Plugin delay compensation: size each edge's delay line so sibling paths
    // meeting at a bus or the master are time-aligned. A node's own latency is
    // the sum of its fx chain's IEffect::LatencySamples() — 0 for every built-in
    // effect today, so every delay line below is length 0 (a plain accumulate)
    // and playback is bit-identical to an uncompensated mix until a latent
    // effect appears. Off the RT thread: Prepare allocates the rings; the RT
    // callback only runs them.
    {
        std::vector<PdcNode> pnodes;
        std::vector<std::pair<TrackId, TrackId>> edges;
        pnodes.reserve(g.fBuses.size());
        for (const Graph::Bus& b : g.fBuses) {
            int lat = 0;
            for (const auto& fx : b.fx)
                if (fx) lat += fx->LatencySamples();
            pnodes.push_back({b.id, lat});
            edges.push_back({b.id, b.output});
            const Track* t = project.FindTrack(b.id);
            if (t)
                for (const Send& s : t->sends)
                    if (s.dest != kInvalidTrackId && s.dest != b.id)
                        edges.push_back({b.id, s.dest});
        }
        PdcGraph pdc;
        const bool ok = ComputePdc(pnodes, edges, pdc);
        for (Graph::Bus& b : g.fBuses) {
            // A dangling output (routed to a deleted node) feeds the master sink
            // in both the topo and the mix, so align it to the master target.
            TrackId outId = b.output;
            if (outId != kRoutingMaster && nodeIndexOf(outId) < 0)
                outId = kRoutingMaster;
            b.outDelay.Prepare(ok ? (size_t)pdc.EdgeDelay(b.id, outId) : 0);
            for (Graph::Bus::SendTarget& st : b.sendTargets)
                st.delay.Prepare(
                    ok ? (size_t)pdc.EdgeDelay(b.id, g.fBuses[st.dest].id) : 0);
        }
    }

    // Master bus effect chain (applied to the summed output).
    // Insert state collected alongside, then moved into the atomic arrays once
    // the final chain length is known (this chain DROPS effects that fail to
    // build, so its indices don't track project.masterFx).
    std::vector<char>  masterBypass;
    std::vector<float> masterMix;
    for (const EffectDesc& d : project.masterFx) {
        auto fx = MakeEffect(d, g.fOutputRate);
        if (!fx) continue;
        fx->Prepare(g.fOutputRate);
        fx->SetTempo(project.tempoBPM);
        const int lat = fx->LatencySamples();   // fixed once Prepare has run
        g.fMasterFxDelay.emplace_back();
        g.fMasterFxDelay.back().Prepare(lat > 0 ? (size_t)lat : 0);
        masterBypass.push_back(d.bypassed ? 1 : 0);
        masterMix.push_back(ClampFxMix(d.mix));
        g.fMasterFx.push_back(std::move(fx));
        g.fMasterFxTypes.push_back(d.type);
    }
    if (!g.fMasterFx.empty()) {
        g.fMasterFxBypass.reset(new std::atomic<bool>[g.fMasterFx.size()]);
        g.fMasterFxMix.reset(new std::atomic<float>[g.fMasterFx.size()]);
        for (size_t i = 0; i < g.fMasterFx.size(); i++) {
            g.fMasterFxBypass[i].store(masterBypass[i] != 0,
                                       std::memory_order_relaxed);
            g.fMasterFxMix[i].store(masterMix[i], std::memory_order_relaxed);
        }
    }

    // Master loudness meter. Integrated accumulation is disabled: it allocates
    // per 100 ms and this runs on the audio thread. Momentary / short-term /
    // true-peak stay valid (preallocated rings + fixed arrays).
    // Metronome follows the project's tempo/meter map (copied for the RT
    // thread). Sync its rate to the engine's output rate.
    {
        TempoMap tm = project.tempoMap;
        tm.sampleRate = g.fOutputRate;
        g.fMetronome.SetTempoMap(tm);
        g.fTempoMap = tm;        // for tempo-synced effects (delay)
        g.fLastFxBpm = 0.0;      // force a push on the first block
    }

    g.fLoudness.Prepare(g.fOutputRate);
    g.fLoudness.SetIntegratedEnabled(false);
    g.fMonPhase = 0.0; g.fMonPrevL = g.fMonPrevR = 0.0f;

    // Per-node mix buffers, sized to the output buffer (generous floor).
    size_t maxFrames = req.playerBufferBytes / (sizeof(float) * 2);
    if (maxFrames < 8192) maxFrames = 8192;
    g.fScratch.assign(maxFrames * 2, 0.0f);
    g.fMonBuf.assign(maxFrames * 2, 0.0f);
    g.fMonSrc.assign(maxFrames * 2 * 8, 0.0f);   // source-rate scratch (<=8x down)
    g.fNodeBufs.assign(g.fBuses.size(), std::vector<float>(maxFrames * 2, 0.0f));
    // Per-node peak meters (one atomic pair per node).
    g.fNodePeakL.reset(new std::atomic<float>[g.fBuses.size()]);
    g.fNodePeakR.reset(new std::atomic<float>[g.fBuses.size()]);
    for (size_t i = 0; i < g.fBuses.size(); i++) {
        g.fNodePeakL[i].store(0.0f);
        g.fNodePeakR[i].store(0.0f);
    }
    // Live-input routing is applied by PublishGraph (the list as of
    // publication, so a route change during the build is not lost).
    return graph;
}

} // namespace daw
