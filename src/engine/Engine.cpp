#include "Engine.h"

#include "../dsp/EffectFactory.h"
#include "../model/Crossfade.h"
#include "../model/MidiControl.h"

#include <MediaDefs.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

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
// How much audio one clip stream's ring holds. This is the slack between the
// fill workers and the audio callback, and it is also — at 2 s — what a big
// project pays BEFORE anything plays: MEASURED 2026-10-12, 329 clips at 2 s of
// stereo float is 253 MB of ring, and allocating and zeroing it was 359 ms of a
// 721 ms build (the file opens themselves were 0.5 ms). Half a second is what
// the pool needs instead: with kStreamPoolWorkers workers over a big project,
// one worker walks its share of the streams every few tens of milliseconds, so
// the ring only has to cover that walk plus a disk hiccup — and a project so
// large that a walk exceeded it degrades into the RT's under-run resync, which
// is the same path an xrun takes, rather than into wrong audio.
static constexpr size_t kRingFramesPerStream = 48000 / 2;   // frames
static_assert(kRingFramesPerStream >= 8192, "a ring must cover several blocks");

// DAW_BUILD_TIMING=1 prints where a graph build spends its time, one line per
// build: how long every clip's stream took to OPEN (file, ring, seek, disk
// thread — the cheap part) and how long the batch of streams took to PRIME
// (waiting for their rings to hold a pre-roll — the part that used to be
// serialized per clip). The measurement that shaped M4.3, kept because it is
// the number the hardware box gets asked for (the M0.7 DAW_TIMELINE_TIMING
// precedent).
static bool BuildTimingOn() {
    static const bool on = std::getenv("DAW_BUILD_TIMING") != nullptr;
    return on;
}

// How much output audio a stream should hold before its graph is published.
// A quarter of a second is a comfortable margin for the start — a local disk
// delivers it in a millisecond or two — and it replaces "half of the 2 s ring",
// which is a second of audio nobody needs before playback may begin.
static constexpr double kPrimePreRollSeconds = 0.10;
static Frame PrimePreRollFrames(float outputRate) {
    const double f = kPrimePreRollSeconds * (double)outputRate;
    return (Frame)(f + 0.5);
}
// The whole build's priming cap, inside the plan's 300 ms audio-start budget
// with room for a cold cache. A stream that misses it is not a stall: its disk
// thread keeps filling, and the RT's under-run resync plays whatever arrives
// late.
static constexpr bigtime_t kPrimeTimeoutUs = 200000;
// The pool's worker count is StreamPool::kStreamPoolWorkers (StreamPool.h): it
// is what replaced both the per-clip thread and the wave-batching that stood in
// for it while this was being measured.
static constexpr size_t kRingFloats = kRingFramesPerStream * 2;

// Cheap identities compared by SyncFx and CaptureFxStates (see
// Bus::fxIdentity / Bus::fxStateHash). Zero for "no state" in the hash, which
// is what every built-in and every stateless plugin has.
static uint64_t FxStateHash(const std::string& state) {
    return state.empty() ? 0u : (uint64_t)std::hash<std::string>{}(state);
}

// What an insert IS: its type plus the plugin it names. Two LV2 inserts at one
// index share a type, so the type alone cannot tell a swapped-in plugin from
// the one the chain was built from — for SyncFx that means pushing one plugin's
// parameters into another, and for a state capture it means writing one
// insert's patch onto another's descriptor.
static uint64_t FxIdentity(const EffectDesc& d) {
    const uint64_t h = (uint64_t)std::hash<std::string>{}(d.pluginName);
    // FNV-style mixing: the two halves must not cancel for a swapped pair
    // (type 3 + name X vs type 4 + name Y).
    return ((uint64_t)(int)d.type * 1099511628211ull) ^ (h + 0x9e3779b97f4a7c15ull);
}

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
                         Frame fadeIn, Frame fadeOut, float clipGain,
                         FadeShape fadeInShape, FadeShape fadeOutShape,
                         StreamLoopRange loop)
    : fTrackId(track), fPath(path), fStart(startFrame), fLength(lengthFrames),
      fSourceOffset(sourceOffset), fSeekDelta(seekProjectDelta),
      fOutputRate(outputRate), fFadeIn(fadeIn), fFadeOut(fadeOut),
      fFadeInShape(fadeInShape), fFadeOutShape(fadeOutShape),
      fClipGain(clipGain), fLoop(loop), fRing(kRingFloats) {
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

TrackStream::~TrackStream() = default;

status_t TrackStream::Open() {
    // Whatever container the clip's file is, through the one factory: a WAV
    // take, an imported FLAC, an AIFF. Each reader seeks by sample through its
    // own API, which is what makes a seeked playhead — and a loop pass — land
    // correctly.
    fSource = OpenAudioSource(fPath);
    if (!fSource)
        return B_ERROR;

    const float srcRate = fSource->FrameRate();
    fResampler.reset(new Resampler(srcRate, fOutputRate));

    // The passes this stream produces, in playback order (StreamLoop.h): the
    // first from where this graph's playback begins (the clip's own seek
    // position, or its start), then one per loop pass. Seek the source to the
    // first pass's own frame: the ring's origin — the timeline frame of its
    // next unread frame — is exactly fStart + fSeekDelta, which is what Mix
    // and RebaseTo assume.
    fCursor.clipStart = fStart;
    fCursor.clipEnd   = fStart + fLength;
    fCursor.looping   = fLoop.on;
    fCursor.loopStart = fLoop.start;
    fCursor.loopEnd   = fLoop.end;
    fCursor.Start(fStart + fSeekDelta);
    fPassWritten = 0;
    SeekToPass();

    return B_OK;
}

// One step of the producer: at most one decoded chunk's worth of audio into the
// ring. Called by a StreamPool worker (never by the RT thread, and never by two
// threads at once — a stream belongs to one worker, StreamPool.h), so it may
// block on the file read.
//
// It must also be BOUNDED: the worker owns other streams and comes back to this
// one later, so a call pushes what the ring has room for and returns. The
// per-stream state (the unpushed remainder of a chunk, the pass cursor, the
// resampler) is what makes that possible.
bool TrackStream::FillOnce() {
    // Fill until the ring is full enough for the pool to lose interest, the pass
    // is done, or the source ends — a chunk at a time, but several chunks per
    // call. ONE chunk per call was the first cut, and it made a worker's round
    // trip cost a single chunk of progress: a build waiting on a stream whose
    // source was already exhausted (its reader returned two frames and then
    // nothing, which is a real thing a project can contain) waited for that
    // worker to come all the way round again before the end was even discovered.
    bool did = false;
    for (int i = 0; i < kFillChunksPerCall; i++) {
        if (!NeedsFilling()) break;
        if (!FillStep()) break;
        did = true;
    }
    return did;
}

// One chunk's worth of progress, or false when there is none to make right now
// (the ring is full, the pass is done, the source has ended).
bool TrackStream::FillStep() {
    if (fCursor.done) {
        // Every pass this stream has is in the ring: nothing it could produce
        // is ever played.
        fDone.store(true, std::memory_order_relaxed);
        return false;
    }
    const Frame remaining = fCursor.Frames() - fPassWritten;
    if (remaining <= 0) {
        // The pass is in the ring: the next frames to produce are the next
        // pass's, which start with a seek (the loop's jump).
        fChunkFloats = fChunkOffset = 0;
        if (!fCursor.Next()) {
            fDone.store(true, std::memory_order_relaxed);
            return false;
        }
        fPassWritten = 0;
        SeekToPass();
        return true;                    // a new pass began
    }
    if (fChunkFloats == 0) {
        const float* src = nullptr;
        size_t frames = 0;
        if (!fSource->ReadChunk(&src, &frames)) {
            // End of this source: the rest of the pass is silence, and there is
            // nothing left to ask it for.
            fSourceEof.store(true, std::memory_order_relaxed);
            return false;
        }
        // Resample source-rate audio up/down to the output rate so the RT
        // callback drains at 1 ring frame == 1 timeline frame.
        fResampled.clear();
        fResampler->Process(src, frames, fResampled);
        if (fResampled.empty()) return false;
        // Never push past the pass end: what lies beyond it belongs to a
        // timeline range this pass does not visit (the loop jumps over it), and
        // the next pass re-seeks to its own start anyway.
        size_t floats = fResampled.size();
        const size_t limit = (size_t)remaining * 2;
        if (floats > limit) floats = limit;
        fChunk        = fResampled.data();
        fChunkFloats  = floats;
        fChunkOffset  = 0;
    }

    const size_t pushed = fRing.Write(fChunk + fChunkOffset, fChunkFloats);
    fChunkOffset += pushed;
    fChunkFloats -= pushed;
    fPassWritten += (Frame)(pushed / 2);
    return pushed > 0;
}

// Seek the source to the frame the cursor's current pass starts at, and drop
// the resampler's carried state — the audio before this frame (or before the
// loop's own start) is not what plays next, so an interpolated remnant of it
// would be a click at the seam.
void TrackStream::SeekToPass() {
    if (!fSource) return;
    fSource->Seek(PassSourceFrame(fSourceOffset, fStart, fCursor.lo,
                                  fSource->FrameRate(), fOutputRate));
    if (fResampler) fResampler->Reset();
}

void TrackStream::RebaseTo(Frame timelineFrame, uint64_t wraps) {
    const Frame skip = LoopRebaseSkipFrames(fStart + fSeekDelta, fStart,
                                           fStart + fLength, timelineFrame,
                                           fLoop.on, fLoop.start, fLoop.end,
                                           wraps);
    if (skip <= 0) return;
    // Two floats (L/R) per frame; Skip is the consumer-side O(1) advance. What
    // the ring does not hold yet becomes under-run debt, which Mix's resync
    // drains as the disk thread fills it — the same path an xrun takes.
    const size_t got = fRing.Skip((size_t)skip * 2);
    fSkipDebt += skip - (Frame)(got / 2);
}

void TrackStream::Mix(float* out, size_t frames, Frame blockStart,
                      uint64_t wraps) {
    // First block after this stream's graph became active: drop what the
    // transport moved past while the graph was being built. Nothing to do when
    // the graph was built for exactly this frame (play, seek, the sync path).
    if (fRebasePending) {
        fRebasePending = false;
        RebaseTo(blockStart, wraps);
    }

    const Frame clipEnd = fStart + fLength;
    // Snapshot the live params once per block.
    const float gl  = fGainL.load(std::memory_order_relaxed);
    const float gr  = fGainR.load(std::memory_order_relaxed);
    const bool  aud = fAudible.load(std::memory_order_relaxed);

    // Whether this block found the ring starved at any point. Counted once for
    // the block, not once per starved frame: an xrun is a moment the audio
    // dropped out, and a block holds one such moment however long it lasted.
    bool starved = false;

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
            starved = true;
            continue;
        }

        // Always consume the ring above; only sum when audible so an unmute
        // resumes in sample-sync rather than replaying buffered audio.
        if (aud) {
            // Fade-in/out envelope with the shape this fade renders with
            // (M2.5) — the same helper the exporter and the drawn waveform use.
            const Frame rel = ph - fStart;
            const float fade = ClipFadeEnvelope(rel, fLength, fFadeIn,
                                                fFadeInShape, fFadeOut,
                                                fFadeOutShape);
            out[i * 2 + 0] += lr[0] * gl * fade * fClipGain;
            out[i * 2 + 1] += lr[1] * gr * fade * fClipGain;
        }
    }
    // Published for the transport's xrun readout (M3.1). A relaxed atomic
    // increment: RT-safe (no lock, no allocation) and the exact count matters
    // less than that a dropout is visible at all.
    if (starved && fXruns != nullptr)
        fXruns->fetch_add(1, std::memory_order_relaxed);
}

// --- Engine::Graph ----------------------------------------------------

void Engine::Graph::ApplyRoutes(const std::vector<MidiInputRoute>& routes) {
    for (Bus& b : fBuses) {
        const MidiInputRoute r = RouteFor(routes, b.id);
        b.inEndpoint.store(r.endpoint, std::memory_order_relaxed);
        b.inChannel.store(r.channel, std::memory_order_relaxed);
    }
}

void Engine::Graph::ReleaseSustained(Frame at) {
    for (LiveVoice& v : fVoices)
        if (v.active && v.sustained && !v.releasing) {
            v.sustained = false;
            v.releasing = true;
            v.off       = at;
        }
}

VoiceExpression Engine::Graph::LiveExpression() const {
    VoiceExpression x;
    x.bendRatio = fLiveBendRatio;
    x.bendPhase = fLiveBendPhase;
    x.modWheel  = (float)fLiveCc[1] / 127.0f;
    return x;
}

void Engine::Graph::UpdateLiveVoices(Engine& e, Frame blockStart, size_t frames) {
    fLiveNotes.clear();
    for (Bus& b : fBuses) b.liveNotes.clear();

    // Advance the live bend phase by the block just elapsed, at the ratio it was
    // rendered with. A note-on below is then stamped with this value and the
    // render is handed the same one: the phase of a sounding voice moves by
    // whatever ratio each block carries, and a wheel move never jumps it — the
    // live twin of the recorded path's integrated bend (MidiExpression.h).
    fLiveBendPhase += ((double)fLiveBendRatio - 1.0) * (double)frames;

    IMidiInput* in = e.fLiveMidi.load(std::memory_order_relaxed);
    if (!in) {                       // monitoring off: release every voice
        for (LiveVoice& v : fVoices) v.active = false;
        return;
    }

    // Drain events (RT-safe: stack buffer, lock-free ring). A note-on takes a
    // free voice (or steals the oldest); a note-off starts that voice's release;
    // CC64 defers that release instead, and the wheel/pedal reach the voices
    // that are already sounding.
    MidiEvent ev[64];
    std::size_t n;
    while ((n = in->ReadEvents(ev, 64)) > 0) {
        for (std::size_t i = 0; i < n; i++) {
            const MidiEvent& evt = ev[i];

            // Continuous controllers: kept in the graph's fixed arrays and
            // handed to every voice this block through the VoiceExpression
            // (below), so a wheel or pedal move is audible on notes already
            // down instead of only on the next one.
            if (evt.type == MidiEvent::kControlChange) {
                fLiveCc[evt.data1 & 0x7f] = evt.data2;
                if ((evt.data1 & 0x7f) == 64 && fSustain.SetValue((int)evt.data2))
                    ReleaseSustained(blockStart);   // pedal up: let go of what it held
                continue;
            }
            if (evt.type == MidiEvent::kPitchBend) {
                fLiveBendRatio = BendRatio((int)evt.bend + 8192);
                continue;
            }

            if (evt.IsNoteOn()) {
                // A re-strike of a key the pedal is holding: release what it took
                // (so the strike is a fresh voice, not a doubling) and retrigger.
                if (fSustain.Retrigger(VoiceKey(evt.channel, evt.data1)))
                    for (LiveVoice& v : fVoices)
                        if (v.active && v.sustained && !v.releasing
                            && v.pitch == evt.data1 && v.channel == evt.channel) {
                            v.sustained = false;
                            v.releasing = true;
                            v.off       = blockStart;
                        }
                int slot = -1;
                for (int k = 0; k < kMaxLiveVoices; k++)
                    if (!fVoices[k].active) { slot = k; break; }
                if (slot < 0) {                       // steal the oldest voice
                    slot = 0;
                    for (int k = 1; k < kMaxLiveVoices; k++)
                        if (fVoices[k].start < fVoices[slot].start) slot = k;
                }
                fVoices[slot] = LiveVoice{ true, false, false, evt.data1, evt.data2,
                                           evt.channel, blockStart, 0, evt.source,
                                           fLiveBendPhase };
            } else if (evt.IsNoteOff()) {
                // Match the source too: the same key on a second keyboard is a
                // different voice, and letting its note-off close this one would
                // cut the first keyboard's note short.
                for (int k = 0; k < kMaxLiveVoices; k++)
                    if (fVoices[k].active && !fVoices[k].releasing
                        && fVoices[k].pitch == evt.data1
                        && fVoices[k].channel == evt.channel
                        && fVoices[k].source == evt.source) {
                        // CC64 down takes the release instead of the key: the
                        // pedal decides when this note stops (Sustain.h).
                        if (fSustain.DeferNoteOff(VoiceKey(evt.channel, evt.data1)))
                            fVoices[k].sustained = true;
                        else {
                            fVoices[k].releasing = true;
                            fVoices[k].off       = blockStart;
                        }
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
        MidiNote note{ (int)v.pitch, (int)v.vel, v.start, len };
        // The bend phase this voice was born with: the render subtracts it from
        // the block's, so the wheel moves the note from where it is.
        note.bendPhaseFrames = v.bend;
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
    UpdateLiveVoices(e, bs, frames);
    const VoiceExpression liveExpr = LiveExpression();
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
            b.instrument->Render(b.liveNotes, nb, frames, bs, 1.0f, liveExpr);
        const float mgl = b.midiGainL.load(std::memory_order_relaxed);
        const float mgr = b.midiGainR.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; i++) {
            nb[i * 2 + 0] *= mgl;
            nb[i * 2 + 1] *= mgr;
        }
        // No external keys here: monitor-only renders live input with no
        // project playing, so there is no source material to tap, and a keyed
        // insert detects internally for as long as it lasts (the documented
        // no-key behavior). Nothing is delayed either — with no key there is no
        // input timeline to align to.
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
//
// It renders in SEGMENTS: one per straight run of transport positions the
// callback covers (M4.2). Without a loop that is the whole block; with one it is
// [pos, loopEnd) and then [loopStart, ...) for the rest, so the loop's jump
// lands on the exact frame rather than on a block boundary. Everything inside
// the segment — fades, automation, MIDI notes, the click, the meters — is
// rendered at the position it actually plays at, and only the position is the
// driver's business.
void Engine::Graph::Render(Engine& e, float* out, size_t frames) {
    if (e.fMonitorOnly.load(std::memory_order_relaxed)) {
        RenderMonitorOnly(e, out, frames);
        return;
    }
    if (!e.fPlaying.load()) {
        e.fPeakL.store(0.0f); e.fPeakR.store(0.0f);
        return;
    }

    Frame pos = e.fPlayhead.load();
    size_t done = 0;
    while (done < frames) {
        if (fLooping && pos >= fLoopEnd) {
            // The loop end: the transport jumps to the loop start. The audio
            // for it is already in the streams' rings — their disk threads
            // produce in playback order across the seam — so this is a jump,
            // not a rebuild: nothing is re-opened, nothing is re-built, and the
            // device keeps running.
            pos = fLoopStart;
            e.fWraps.fetch_add(1, std::memory_order_relaxed);
        } else if (!fLooping && pos >= fEndFrame) {
            // Past all content (and not looping): the main thread will Stop().
            e.fFinished.store(true);
            e.fPeakL.store(0.0f);
            e.fPeakR.store(0.0f);
            return;
        }
        size_t n = frames - done;
        if (fLooping) {
            const size_t beforeWrap = SegmentFrames(pos, n, fLoopStart, fLoopEnd);
            if (beforeWrap < n) n = beforeWrap;   // 0 is handled by the wrap above
        }
        RenderSegment(e, out + done * 2, n, pos,
                      fInPlaceJoin
                          ? e.fWraps.load(std::memory_order_relaxed)
                                - fWrapsAtRequest
                          : 0);
        done += n;
        pos += static_cast<Frame>(n);
    }
    e.fPlayhead.store(pos);
}

void Engine::Graph::RenderSegment(Engine& e, float* out, size_t frames,
                                  Frame blockStart, uint64_t wraps) {
    // Live MIDI monitoring: drain incoming events and advance held voices, then
    // rebuild fLiveNotes for this block. Rendered per armed MIDI bus below.
    UpdateLiveVoices(e, blockStart, frames);

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

        // A node that takes an external key has an INPUT, so its own material
        // has to sit where that input lands — delayed by its PDC input latency
        // (Bus::inDelay) — or the detector would compare this block's audio
        // with a key from another timeline moment. Zero for every ordinary
        // node, so this costs a pointer choice. Buses have no own material.
        const bool delayOwn = !b.isBus && b.inDelay.d > 0;
        float* mb = delayOwn ? fMaterial.data() : nb;
        if (delayOwn) std::memset(mb, 0, nfloats * sizeof(float));

        for (TrackStream* s : b.streams)    // audio leaves (fader is per-stream)
            s->Mix(mb, frames, blockStart, wraps);
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
                // Bend and the mod wheel for this block: the recorded lane's, so
                // a stored controller is what a bounced file would render too.
                const VoiceExpression expr =
                    ExpressionAt(b.bend, b.events, blockStart,
                                 blockStart + (Frame)frames);
                if (b.instrument)
                    b.instrument->Render(b.notes, mb, frames, blockStart, from, to,
                                         expr);
                b.chanL = cgl;
                b.chanR = cgr;
            }
            if (live) {                      // live keyboard through this voice
                // Live notes carry the expression from the keyboard: the same
                // path a recorded lane takes, built from what arrived instead of
                // from the event list.
                const VoiceExpression liveExpr = LiveExpression();
                if (b.instrument)
                    b.instrument->Render(b.liveNotes, mb, frames, blockStart, 1.0f,
                                         liveExpr);
            }
            const float mgl = b.midiGainL.load(std::memory_order_relaxed);
            const float mgr = b.midiGainR.load(std::memory_order_relaxed);
            for (size_t i = 0; i < frames; i++) {
                mb[i * 2 + 0] *= mgl;
                mb[i * 2 + 1] *= mgr;
            }
        }
        // Now that the node's own material is complete, delay it to this node's
        // input latency and SUM it into the node buffer: the incoming sends
        // already sit at that position and must not be delayed a second time.
        if (delayOwn) b.inDelay.ProcessAdd(mb, nb, frames, 1.0f);
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
        // plain fully-wet slot is a bare Process, as before. An insert with a
        // routed external key is handed this block of it first: the source
        // node's post-fader output (the same tap a post-fader send takes) run
        // through the per-edge PDC delay, so the key sits on this node's input
        // timeline and — in the block the insert processes — exactly where the
        // exporter's key sits. fxKeyNode is a pre-resolved index: no id lookups
        // and no allocation on the audio thread. A source whose node is not
        // audible this block was skipped, and its buffer was zeroed with every
        // other node's, so a muted key track keys with silence.
        for (size_t fi = 0; fi < b.fx.size(); fi++) {
            if (b.fx[fi] && fi < b.fxKeyNode.size() && b.fxKeyNode[fi] >= 0) {
                std::memset(fKeyScratch.data(), 0, nfloats * sizeof(float));
                b.fxKeyDelay[fi].ProcessAdd(
                    fNodeBufs[(size_t)b.fxKeyNode[fi]].data(),
                    fKeyScratch.data(), frames, 1.0f);
                b.fx[fi]->SetSidechain(fKeyScratch.data(), (int)frames);
            }
            RunInsertSlot(b.fx[fi].get(), b.fxDryDelay[fi],
                      b.fxBypass[fi].load(std::memory_order_relaxed),
                      b.fxMix[fi].load(std::memory_order_relaxed),
                      nb, frames, fScratch.data());
        }

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

    // Master bus FX on the summed output (before gain/metering). A master
    // insert's sidechain key is aligned to the master sink (see Load), so it is
    // staged exactly like a track insert's — every node has been rendered by
    // now, so any track can be a key here.
    for (size_t fi = 0; fi < fMasterFx.size(); fi++) {
        if (fMasterFx[fi] && fi < fMasterFxKeyNode.size()
            && fMasterFxKeyNode[fi] >= 0) {
            std::memset(fKeyScratch.data(), 0, nfloats * sizeof(float));
            fMasterFxKeyDelay[fi].ProcessAdd(
                fNodeBufs[(size_t)fMasterFxKeyNode[fi]].data(),
                fKeyScratch.data(), frames, 1.0f);
            fMasterFx[fi]->SetSidechain(fKeyScratch.data(), (int)frames);
        }
        RunInsertSlot(fMasterFx[fi].get(), fMasterFxDelay[fi],
                  fMasterFxBypass[fi].load(std::memory_order_relaxed),
                  fMasterFxMix[fi].load(std::memory_order_relaxed),
                  out, frames, fScratch.data());
    }
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
    // ... and the clip flags for the same block (M3.3): a clip the 60 Hz poll
    // never sees because a later block overwrote the peak is still latched.
    if (ReachesClipLevel(pl))
        e.fClipFlags.fetch_or(kMeterClipL, std::memory_order_relaxed);
    if (ReachesClipLevel(pr))
        e.fClipFlags.fetch_or(kMeterClipR, std::memory_order_relaxed);

    // Master RMS + stereo correlation over the meter's window, from the same
    // block (M3.3). Preallocated at build; arithmetic only.
    fStereoMeter.Process(out, static_cast<int>(frames));
    e.fRmsL.store(fStereoMeter.RmsL());
    e.fRmsR.store(fStereoMeter.RmsR());
    e.fCorr.store(fStereoMeter.Correlation());

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
                         Frame minEndFrame, LoadMode mode, bool noLoop) {
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
    const bigtime_t tSnap0 = BuildTimingOn() ? system_time() : 0;
    req->project       = project;          // the snapshot: see BuildRequest
    if (BuildTimingOn())
        std::fprintf(stderr, "Engine: build timing: snapshot %.1f ms\n",
                     (double)(system_time() - tSnap0) / 1000.0);
    req->startFrame    = startFrame;
    req->minEndFrame   = minEndFrame;
    req->mode          = mode;
    req->monitorOnly   = fMonitorOnly.load(std::memory_order_relaxed);
    req->gen           = gen;
    req->outputRate    = fOutputRate;
    req->playerBufferBytes = fPlayerBufferBytes;
    req->noLoop        = noLoop;
    // The loop the graph will walk, and how far round the transport already is:
    // the builder resolves the first (LoopActive), the RT's in-place join needs
    // the second to know how many passes a build spanned (M4.2).
    req->blockFrames   = fPlayerBufferBytes > 0
        ? (Frame)(fPlayerBufferBytes / (2 * sizeof(float))) : fBufferFrames;
    req->wrapsAtRequest = fWraps.load(std::memory_order_relaxed);
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
        const bigtime_t tBuild0 = system_time();
        std::unique_ptr<Graph> g = BuildGraph(*req, &rc);
        fLastBuildUs.store(system_time() - tBuild0, std::memory_order_relaxed);

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
    fFinished.store(g != nullptr && !g->fLooping
                    && fStartFrame >= g->EndFrame());
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

// A callback duration as a share of the buffer it had to fill, in percent.
// Both numbers (the buffer size and the output rate) are set on the window
// thread before a load, so reading them here is the same single-writer pattern
// the rest of this class uses.
float Engine::LoadPercentFor(bigtime_t callbackUs) const {
    const double frames = (double)fBufferFrames;
    const double rate   = (double)fOutputRate;
    if (frames <= 0.0 || rate <= 0.0) return 0.0f;
    const double budgetUs = frames / rate * 1e6;
    if (budgetUs <= 0.0) return 0.0f;
    double pct = (double)callbackUs / budgetUs * 100.0;
    if (!(pct > 0.0)) pct = 0.0;     // NaN or negative -> 0
    if (pct > 999.0)  pct = 999.0;   // a readout, not a measurement of infinity
    return (float)pct;
}

void Engine::PlayTrampoline(void* cookie, void* buffer, size_t size,
                            const media_raw_audio_format& format) {
    Engine* self = static_cast<Engine*>(cookie);
    const size_t frames = size / (sizeof(float) * format.channel_count);
    // The callback's own cost (M3.1): the clock is read HERE, around the
    // render, and not inside FillBuffer — the RT rule forbids a syscall in the
    // callback body, and everything this figure needs is the duration of the
    // one call below. Two plain atomic stores, no lock, no allocation.
    const bigtime_t t0 = system_time();
    self->FillBuffer(static_cast<float*>(buffer), frames);
    const bigtime_t tookUs = system_time() - t0;
    self->fLastCallbackUs.store(tookUs, std::memory_order_relaxed);
    // Peak hold for the transport's readout: a plain compare-and-store. The
    // pulse takes it with an exchange, so what it reports is the worst block
    // since it last looked (an average would hide the overrun that clicks).
    if (tookUs > self->fPeakCallbackUs.load(std::memory_order_relaxed))
        self->fPeakCallbackUs.store(tookUs, std::memory_order_relaxed);
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
                    const std::vector<uint64_t>& identity,
                    const std::vector<uint64_t>& stateHash,
                    const std::vector<EffectDesc>& descs,
                    const std::vector<TrackId>& keySrc,
                    std::atomic<bool>* bypass, std::atomic<float>* mix) {
        if (fx.size() != descs.size()) { allMatched = false; return; }
        for (size_t i = 0; i < fx.size(); i++) {
            // The insert's identity (type AND plugin), not the type alone: a
            // different plugin of the same type at this index is a different
            // effect, and pushing the new descriptor's params into the old
            // instance would be a chain edit that silently did nothing.
            if (i >= types.size() || types[i] != descs[i].type
                || i >= identity.size()
                || identity[i] != FxIdentity(descs[i])) {
                allMatched = false;   // an effect was replaced at this slot
                continue;
            }
            // A plugin's own state is applied at INSTANTIATION and nothing can
            // push it into a running instance — so a changed state (a preset
            // just loaded onto this insert, say) is structural, even though the
            // type at the slot still matches, and the caller must rebuild.
            if (i >= stateHash.size() ||
                stateHash[i] != FxStateHash(descs[i].state)) {
                allMatched = false;
                continue;
            }
            // An external-sidechain SOURCE is structural, not pushable —
            // unlike bypass and the wet/dry mix below. Changing it moves a
            // graph edge: the source has to render before this node, the
            // consumer's input latency changes, and the key's delay line is
            // sized from that. Pushing the descriptor alone would leave the key
            // delay pointing at the old source's timeline. So report a
            // mismatch and let the caller rebuild, exactly like a type change.
            if (i < keySrc.size() && keySrc[i] != descs[i].sidechainSource) {
                allMatched = false;
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
        if (t) sync(b.fx, b.fxTypes, b.fxIdentity, b.fxStateHash, t->fx,
                    b.fxKeySrc, b.fxBypass.get(), b.fxMix.get());
    }
    sync(g->fMasterFx, g->fMasterFxTypes, g->fMasterFxIdentity,
         g->fMasterFxStateHash, project.masterFx, g->fMasterFxKeySrc,
         g->fMasterFxBypass.get(), g->fMasterFxMix.get());
    return allMatched;
}

void Engine::CaptureFxStates(Project& project) {
    // One insert: ask the live instance for its own state and store it on the
    // descriptor it came from. An insert the engine could not build (no
    // nullptr check needed -- fx[i] is null for an unavailable plugin) and a
    // plugin with no state to give both leave the stored state alone: writing
    // an empty string here would erase a patch the project still holds.
    auto capture = [](IEffect* fx, EffectDesc& d) {
        if (!fx) return;
        std::string state;
        if (fx->SaveState(&state) && !state.empty()) d.state = std::move(state);
    };
    // The graph is a swap slot the reclaimer frees, so an off-RT reader must
    // pin it (see GraphPin) -- the model is edited on this thread while the RT
    // callback keeps rendering, which is exactly the case the pin exists for.
    GraphPin pin(*this);
    Graph* g = pin.Active();
    if (!g) return;   // no graph yet: the model is the only source of truth
    for (Graph::Bus& b : g->fBuses) {
        Track* t = project.FindTrack(b.id);
        if (!t) continue;
        for (size_t i = 0; i < b.fx.size() && i < t->fx.size(); i++) {
            // Only the descriptor this instance was BUILT from. The model can
            // have been edited since the build (a chain replace that kept the
            // chain's length does not rebuild the engine), and writing one
            // plugin's patch onto whatever descriptor sits at the index now
            // would be a wrong state saved to disk.
            if (i < b.fxIdentity.size() &&
                b.fxIdentity[i] == FxIdentity(t->fx[i]))
                capture(b.fx[i].get(), t->fx[i]);
        }
    }
    // The master chain DROPS inserts that fail to build, so fMasterFx is not
    // index-aligned with project.masterFx; the identity check is the same
    // alignment rule SyncFx uses for it. An insert whose effect is missing is
    // skipped — its stored state is what an uninstalled plugin has, not
    // something to overwrite.
    for (size_t i = 0; i < g->fMasterFx.size() && i < project.masterFx.size(); i++)
        if (i < g->fMasterFxIdentity.size() &&
            g->fMasterFxIdentity[i] == FxIdentity(project.masterFx[i]))
            capture(g->fMasterFx[i].get(), project.masterFx[i]);
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
    const bigtime_t tBuild0 = BuildTimingOn() ? system_time() : 0;
    auto graph = std::unique_ptr<Graph>(new Graph());
    Graph& g = *graph;
    g.fBuildStart = req.startFrame;
    g.fEndFrame   = 0;
    g.fOutputRate = req.outputRate;
    g.fMonFrame   = req.startFrame;   // free-running clock for monitor-only mode
    // The loop (M4.2). Resolved ONCE, here, and fixed for the graph's life: the
    // RT wrap and every disk thread's pass walker read the same answer, which is
    // what lets them meet at the seam without any handshake. A range too short
    // to walk (LoopActive) or a record start (noLoop) leaves the graph unlooped,
    // and then it simply plays the transport's range to the end.
    g.fLooping = !req.noLoop && !req.monitorOnly
                 && LoopActive(req.project.transport.loopEnabled,
                               req.project.transport.loopStart,
                               req.project.transport.loopEnd,
                               req.blockFrames);
    g.fLoopStart     = req.project.transport.loopStart;
    g.fLoopEnd       = req.project.transport.loopEnd;
    g.fWrapsAtRequest = req.wrapsAtRequest;
    g.fInPlaceJoin    = (req.mode == LoadMode::InPlace);
    const StreamLoopRange streamLoop{g.fLooping, g.fLoopStart, g.fLoopEnd};
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
    const bigtime_t tStreams0 = BuildTimingOn() ? system_time() : 0;
    size_t nStreams = 0;
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
            if (c.muted)
                continue;   // per-clip mute (M2.5): no stream, no disk thread
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
                g.fOutputRate, fades[ci].fadeIn, fades[ci].fadeOut, c.gain,
                fades[ci].inShape, fades[ci].outShape, streamLoop);
            if (s->Open() != B_OK || !s->Valid()) {
                fprintf(stderr, "Engine: skipping clip '%s'\n",
                        c.sourcePath.c_str());
                continue;
            }
            // Underruns this stream sees are counted in the ENGINE's counter
            // (M3.1): graphs come and go, the figure the transport shows does
            // not reset with one.
            s->SetXrunCounter(&fXruns);
            nStreams++;
            // Sources are resampled to the output rate on their disk thread.
            if (s->EndFrame() > g.fEndFrame)
                g.fEndFrame = s->EndFrame();
            g.fStreams.push_back(std::move(s));
        }
    }
    const bigtime_t tOpen1 = BuildTimingOn() ? system_time() : 0;

    // Start the fills in bounded WAVES, nearest the playhead first, waiting
    // inside each wave for its pre-roll. MEASURED 2026-10-12, 329 clips on the
    // 2-vCPU VM: the per-stream wait this replaces serialized into 2515 ms, and
    // starting all 329 fills at once (the obvious fix) cost 680 ms of pure
    // contention — the builder starved by fillers — while the opens and thread
    // starts themselves were 1 ms and 14 ms. A wave keeps the CPU available and
    // still has the clips the transport needs first ready when the graph lands.
    //
    // The target is a PRE-ROLL (a quarter of a second of audio, enough to cover
    // the start), not half the ring, and the cap bounds the WHOLE build — a
    // per-stream cap would let a project with many slow clips add up to minutes.
    // Missing the cap is not a failure: the disk thread keeps filling and the
    // RT's under-run resync plays whatever arrives late, exactly as it does
    // after an xrun. (A monitor-only graph has no streams at all.)
    const Frame preRoll = PrimePreRollFrames(g.fOutputRate);
    std::vector<size_t> order(g.fStreams.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) {
                         const TrackStream* sa = g.fStreams[a].get();
                         const TrackStream* sb = g.fStreams[b].get();
                         const Frame ka = StreamDueKey(sa->ClipStart(), sa->EndFrame(),
                                                       req.startFrame, g.fLooping,
                                                       g.fLoopStart, g.fLoopEnd);
                         const Frame kb = StreamDueKey(sb->ClipStart(), sb->EndFrame(),
                                                       req.startFrame, g.fLooping,
                                                       g.fLoopStart, g.fLoopEnd);
                         return ka < kb;
                     });
    std::vector<TrackStream*> filling;
    filling.reserve(order.size());
    for (size_t i : order) filling.push_back(g.fStreams[i].get());
    g.fPool.reset(new StreamPool<TrackStream>(filling));
    // Wait, best effort, for the streams the transport needs inside the pre-roll
    // window — the ones playing now and the ones starting within it — to be as
    // primed as they can be. ONE poll per round for the whole batch: a per-stream
    // poll costs 2 ms of granularity each, and 329 of those was 200 ms of pure
    // waiting (MEASURED). Everything else keeps filling in the background; the
    // pool does not stop, and a stream is only late if the transport reaches it
    // before its worker does.
    const bigtime_t primeDeadline = system_time() + kPrimeTimeoutUs;
    for (;;) {
        bool pending = false;
        for (size_t i = 0; i < filling.size(); i++) {
            if (StreamDueKey(filling[i]->ClipStart(), filling[i]->EndFrame(),
                             req.startFrame, g.fLooping, g.fLoopStart,
                             g.fLoopEnd) > preRoll)
                break;                 // sorted by due: nothing left to wait for
            if (filling[i]->NeedsPriming(preRoll)) { pending = true; break; }
        }
        if (!pending || system_time() >= primeDeadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (BuildTimingOn()) {
        const bigtime_t now = system_time();
        std::fprintf(stderr, "Engine: build timing: %zu clip streams opened in "
                             "%.1f ms, primed in %.1f ms\n",
                     nStreams, (double)(tOpen1 - tStreams0) / 1000.0,
                     (double)(now - tOpen1) / 1000.0);
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
        // Playback notes, not the raw ones: the CC64 pedal is applied to each
        // region here (a key-up under the pedal keeps the note sounding), so the
        // voices need no sustain logic of their own. Absolute-timeline frames.
        std::vector<MidiNote> notes = t.CollectPlaybackNotes();
        const bool monitor = t.armed || t.inputMonitor;   // live input synth
        const bool audible = !t.muted && (!anySolo || t.soloed || t.soloSafe);
        if ((notes.empty() && !monitor) || (!audible && !monitor))
            continue;
        Graph::Bus b;
        b.id    = t.id;
        for (const MidiNote& n : notes)
            if (n.startFrame + n.lengthFrames > g.fEndFrame)
                g.fEndFrame = n.startFrame + n.lengthFrames;
        b.events = t.CollectEvents();         // channel CC/PB (absolute frames)
        // Bend is integrated, not per-note retuned: digest the track's bend
        // events once here (off the RT thread) and stamp each note with the
        // phase it starts at, so a voice can keep its phase continuous across a
        // bend step by plain subtraction (see model/MidiExpression.h).
        b.bend = BuildBendTimeline(b.events);
        AnnotateBendPhase(notes, b.bend);
        b.notes = std::move(notes);
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

    // --- External sidechain keys (package 05) -------------------------------
    // One edge per keyed insert: source track -> the track whose chain it keys.
    // "Render the source before the consumer" is the same dependency an aux
    // send has, so the key joins the SAME edge set — which the topo order below
    // and the PDC solve after it both read. A self-key is skipped (no order can
    // read a node's own output before that node runs) and stays unrouted.
    std::vector<std::pair<TrackId, TrackId>> keyEdges;
    std::vector<TrackId>                     keySrcIds;   // deduped, for nodes
    auto addKey = [&](const EffectDesc& d, TrackId consumer) {
        if (!EffectSupportsSidechain(d.type)) return;
        const TrackId src = d.sidechainSource;
        if (src == kInvalidTrackId || src == consumer) return;
        keyEdges.push_back({src, consumer});
        for (TrackId id : keySrcIds)
            if (id == src) return;
        keySrcIds.push_back(src);
    };
    for (const Track& t : project.Tracks())
        for (const EffectDesc& d : t.fx) addKey(d, t.id);
    // A master-chain key is tapped from a track like any other (nothing keys
    // the master); the edge into the master sink imposes no topo constraint —
    // the master runs last anyway — but it is what the key's PDC delay is
    // solved against, so it is collected here with the rest.
    for (const EffectDesc& d : project.masterFx) addKey(d, kRoutingMaster);

    auto nodeIndexOf = [&](TrackId id) -> long {
        for (size_t i = 0; i < g.fBuses.size(); i++)
            if (g.fBuses[i].id == id) return (long)i;
        return -1;
    };

    // A key source with no node of its own: an audio track with no clips, a
    // MIDI track with no notes, a track whose clips all failed to open. The key
    // is tapped from the SOURCE node's output, so without a node the key would
    // silently be unrouted here — while the offline Exporter, which has a node
    // for every track, would still route that track's (silent) output through
    // its chain and hand it over as a key. The two hosts must agree, so a
    // referenced source gets an empty node.
    for (TrackId id : keySrcIds) {
        if (nodeIndexOf(id) >= 0) continue;
        const Track* t = project.FindTrack(id);
        if (!t) continue;   // names no track: unrouted, fail soft
        Graph::Bus b;
        b.id    = id;
        b.isBus = (t->type == TrackType::Bus);
        g.fBuses.push_back(std::move(b));
    }

    if (BuildTimingOn())
        std::fprintf(stderr, "Engine: build timing: everything else to here "
                             "%.1f ms, %zu buses\n",
                     (double)(system_time() - tBuild0) / 1000.0, g.fBuses.size());

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
    // A looping graph always covers the frame it wraps at: the transport's range
    // must not end inside its own loop (the RT stops consulting fEndFrame while
    // looping, and this is what keeps that consistent with Start()/IsFinished).
    if (g.fLooping && g.fLoopEnd > g.fEndFrame)
        g.fEndFrame = g.fLoopEnd;

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
            b.fxIdentity.push_back(FxIdentity(d));
            b.fxStateHash.push_back(FxStateHash(d.state));
            // Sidechain key, per fx slot. The source id is recorded now (SyncFx
            // compares against it); the node index and its PDC delay are
            // resolved once the graph is solved, below.
            b.fxKeySrc.push_back(d.sidechainSource);
        }
        b.fxKeyNode.assign(b.fx.size(), -1);
        b.fxKeyDelay.assign(b.fx.size(), FrameDelay{});
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
    // pre/post-fader flag exactly. (nodeIndexOf is defined above, where the
    // sidechain sources are resolved against it.)
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
    // output, every send destination, AND every insert that keys off it. Sends
    // and keys add extra edges, so use the general edge topo (single-output
    // ResolveRoutingOrder can't express them).
    //
    // Cycle policy (same as the sends': this is one all-or-nothing solve): an
    // edge set that cannot be ordered — a key loop (A keys B keys A), or a key
    // that disagrees with the routing (A keys B while B feeds A's bus) — fails
    // the solve, the order falls back to flat, and `sidechainSolvable` below is
    // false, so NO key is routed and every keyed insert detects internally.
    // Partial routing would be worse than none: an effect that sometimes
    // detects a key and sometimes its own signal, depending on track creation
    // order, is not something a user could reason about.
    bool sidechainSolvable = false;
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
        for (const auto& e : keyEdges) edges.push_back(e);
        std::vector<TrackId> ord;
        if (ResolveOrderWithEdges(nids, edges, ord)) {
            for (TrackId id : ord)
                for (size_t i = 0; i < g.fBuses.size(); i++)
                    if (g.fBuses[i].id == id) { g.fOrder.push_back(i); break; }
            sidechainSolvable = true;
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
    // `pdc`/`pdcKnown` outlive the block: the master chain below needs the same
    // solved graph for a master insert's sidechain key.
    PdcGraph pdc{};
    bool     pdcKnown = false;
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
        // The key edges join the PDC edge set for the same reason the sends do:
        // the source must render first, AND its path latency has to be part of
        // the solve. Adding them raises the consumer's input latency to at
        // least the key source's output latency, which is what makes
        // "consumer's own material delayed by its input latency" and "key
        // delayed by EdgeDelay(source, consumer)" the same number of frames —
        // i.e. what makes the detector see audio and key from one moment.
        for (const auto& e : keyEdges) edges.push_back(e);
        pdcKnown = ComputePdc(pnodes, edges, pdc);
        const bool ok = pdcKnown;
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
            // External sidechain keys: resolve each insert's source to a node
            // and size the delay line that aligns it with THIS node's input —
            // the same EdgeDelay a post-fader send from that source would get.
            // `ok` (the solve) doubles as the key's gate: on a cycle, and on
            // any build whose topo fell back to flat, no key is routed at all
            // and the keyed inserts detect internally (fail soft).
            for (size_t fi = 0; fi < b.fxKeyNode.size(); fi++) {
                const TrackId src = fi < b.fxKeySrc.size() ? b.fxKeySrc[fi]
                                                           : kInvalidTrackId;
                long si = -1;
                if (ok && sidechainSolvable && src != kInvalidTrackId
                    && src != b.id && project.FindTrack(src)) {
                    si = nodeIndexOf(src);
                }
                b.fxKeyNode[fi] = si;   // -1 = no key routed
                if (si >= 0)
                    b.fxKeyDelay[fi].Prepare(
                        (size_t)pdc.EdgeDelay(src, b.id));
            }
            // This node's own material is delayed by its input latency (see
            // Bus::inDelay). 0 for every node nothing feeds, which is every
            // ordinary track — length 0 is a plain, free accumulate.
            b.inDelay.Prepare(ok ? (size_t)pdc.InLat(b.id) : 0);
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
        // A master insert can take a track as its key too. Its consumer is the
        // master sink, so the alignment delay is EdgeDelay(src, master) — the
        // very delay the source's own path into the master uses, which is what
        // puts the key on the same timeline as the mix it is ducking. (The key
        // is a TRACK even here: nothing keys the master.)
        long keyNode = -1;
        if (pdcKnown && sidechainSolvable && EffectSupportsSidechain(d.type)
            && d.sidechainSource != kInvalidTrackId
            && project.FindTrack(d.sidechainSource)) {
            keyNode = nodeIndexOf(d.sidechainSource);
        }
        g.fMasterFxKeySrc.push_back(d.sidechainSource);
        g.fMasterFxKeyNode.push_back(keyNode);
        g.fMasterFxKeyDelay.emplace_back();
        if (keyNode >= 0)
            g.fMasterFxKeyDelay.back().Prepare(
                (size_t)pdc.EdgeDelay(d.sidechainSource, kRoutingMaster));
        g.fMasterFx.push_back(std::move(fx));
        g.fMasterFxTypes.push_back(d.type);
        g.fMasterFxIdentity.push_back(FxIdentity(d));
        g.fMasterFxStateHash.push_back(FxStateHash(d.state));
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

    // The master meter's RMS/correlation window: allocated HERE (the build runs
    // off the RT thread), so the callback only does arithmetic. The published
    // readings are engine-owned and reset with the new graph, as the old
    // synchronous Load reset them.
    g.fStereoMeter.Prepare(g.fOutputRate, 0.3f);
    fRmsL.store(0.0f); fRmsR.store(0.0f); fCorr.store(0.0f);
    fClipFlags.store(0, std::memory_order_relaxed);

    // Per-node mix buffers, sized to the output buffer (generous floor).
    size_t maxFrames = req.playerBufferBytes / (sizeof(float) * 2);
    if (maxFrames < 8192) maxFrames = 8192;
    g.fScratch.assign(maxFrames * 2, 0.0f);
    g.fMaterial.assign(maxFrames * 2, 0.0f);   // node material held across inDelay
    g.fKeyScratch.assign(maxFrames * 2, 0.0f); // one sidechain key while it is delayed
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
