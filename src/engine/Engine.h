// Engine — the playback engine.
//
// Pulls decoded audio from disk (any IAudioSource: WAV, AIFF, FLAC, Ogg
// Vorbis) through a lock-free RingBuffer
// filled by a per-track disk thread, and mixes it in the BSoundPlayer
// real-time callback. One TrackStream per playing clip; the callback sums
// them, so extending to true multitrack (milestone 3) is just more streams.
//
// Threading contract (the rule that governs the whole DAW):
//   - The audio callback (RT thread) only reads rings + atomics. No alloc,
//     no locks, no file I/O.
//   - Disk threads do all file reading and fill the rings.
//   - The main thread starts/stops and polls IsFinished().
//
// M4.1 — the graph is built OFF the window thread:
//   - Everything a rebuild replaces lives in one Engine::Graph (streams, buses,
//     FX, buffers, the mix order, the meter scratch). A rebuild is: snapshot
//     the project on the caller's thread, build a Graph on the builder thread,
//     publish it with ONE atomic pointer exchange (GraphSwap), and free the
//     graph it replaced on the reclaim thread once no thread can hold it (the
//     callback generation for the RT side, GraphPin for the UI side).
//   - The RT callback loads that pointer exactly once per block and renders the
//     whole block from it; it never allocates, locks, logs or opens anything.
//   - The BSoundPlayer is persistent: it is created once per output format and
//     survives every rebuild, so an edit during playback has no device gap and
//     no `BSoundPlayer` re-create. Only Start/Stop bracket playback.
//   - A position-changing rebuild (`LoadMode::NewPosition`: play, seek, record
//     start) detaches the active graph first — the RT goes silent
//     and the playhead stands still until the new graph is in, which is what a
//     seek did before, minus the frozen window. A content rebuild
//     (`LoadMode::InPlace`: a structural edit while playing) leaves the old
//     graph playing and lets the new one join at the current playhead through
//     the per-stream rebase (StreamRebase.h), so the edit is heard with no gap.
//
// M4.2 — the loop is walked inside one graph:
//   - The loop range is part of the graph (built from the snapshot), so the RT
//     callback and every disk thread derive the seam from the same description.
//     The callback splits its block at the loop end and renders the rest at the
//     loop start (sample-exact); the disk threads produce each clip's audio in
//     PLAYBACK ORDER across the wraps, pre-rolling the loop start into their
//     rings. No rebuild, no device re-create, no seek at the seam —
//     `Engine::Wraps()` counts the wraps and nothing else moves.
//   - A loop-range edit is not a content edit: the range the transport walks is
//     part of the position it plays at, so it re-positions the engine (the
//     window's kMsgTransportRange -> StartPlayback) and the new graph walks the
//     new range. An edit that leaves the range alone — a clip moved, an insert
//     added — keeps its in-place join, which walks the passes the build spanned
//     (StreamLoop.h).
//
// Haiku-only: depends on the Media Kit (BSoundPlayer).
#pragma once

#include "GraphSwap.h"
#include "RingBuffer.h"
#include "AudioFormats.h"   // OpenAudioSource (WAV / AIFF / FLAC / Ogg)
#include "Resampler.h"
#include "FrameDelay.h"
#include "InsertSlot.h"
#include "StreamRebase.h"
#include "StreamLoop.h"
#include "StreamOrder.h"
#include "StreamPool.h"
#include "../model/Project.h"
#include "../model/RoutingGraph.h"
#include "../model/Pdc.h"
#include "../model/MidiExpression.h"   // BendPoint / VoiceExpression
#include "../model/Sustain.h"         // SustainLatch (CC64, live form)
#include "../dsp/IEffect.h"
#include "../dsp/Loudness.h"
#include "../dsp/Stretch.h"     // the disk thread's WSOLA stage (M7.2)
#include "../dsp/MeterMath.h"   // StereoMeter + the clip flag bits (M3.3)
#include "../synth/IInstrument.h"
#include "../synth/InstrumentFactory.h"
#include "../midi/IMidiInput.h"
#include "../midi/MidiEventRing.h"   // the audition ring (package 51)
#include "../midi/MidiRouting.h"
#include "Metronome.h"
#include "IMonitorSource.h"

#include <SoundPlayer.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace daw {

// One clip's playback: a decoder, a ring, and the disk thread feeding it,
// plus where the clip sits on the timeline and the track's gain.
class TrackStream {
public:
    // startFrame/lengthFrames/seekProjectDelta are in output (timeline) frames;
    // sourceOffset is in source frames. The stream resamples the source to
    // outputRate on its disk thread, so the ring is at the output rate.
    // `stretchRatio` is the clip's output-duration multiplier (1.0 = play as
    // recorded): the disk thread then runs the decoded, rate-matched audio
    // through a Stretcher before the ring, and the source the pass consumes is
    // that much shorter or longer. The ring stays 1 output frame per timeline
    // frame, so nothing on the RT side knows (see model/ClipStretch.h).
    TrackStream(TrackId track, const std::string& path, Frame startFrame,
                Frame lengthFrames, Frame sourceOffset, float gain, float pan,
                bool audible, Frame seekProjectDelta, float outputRate,
                Frame fadeIn, Frame fadeOut, float clipGain = 1.0f,
                FadeShape fadeInShape = FadeShape::Linear,
                FadeShape fadeOutShape = FadeShape::Linear,
                StreamLoopRange loop = StreamLoopRange{},
                double stretchRatio = 1.0);
    ~TrackStream();

    // Open the clip's file, set up the pass cursor and seek the source. No
    // thread yet, no waiting: MEASURED 2026-10-12, this is 0.09 ms per clip over
    // 329 clips, so a build can afford to open every stream before any of them
    // starts filling. False when the file cannot be read at all.
    status_t Open();

    // Producer side, called by a StreamPool worker (StreamPool.h): is there room
    // for more audio? False when the ring is full, or when this stream has
    // nothing left to produce — either way a worker should move on to its next
    // stream rather than spin here.
    bool NeedsFilling() const {
        if (!fSource) return false;
        if (fProducingDone()) return false;
        // The source has ended: this pass's remaining frames will never be
        // produced (they play as silence, exactly as they did before the pool),
        // and a builder waiting on this stream must stop waiting rather than
        // burn its budget on audio that does not exist.
        if (fSourceEof.load(std::memory_order_relaxed)) return false;
        // A meaningful amount: refilling in single floats would make a worker
        // walk all its streams to move a handful of samples.
        return fRing.WriteAvailable() >= kMinFillFloats;
    }

    // Push audio into the ring until it is full enough, the pass is done or the
    // source ends (may block on the file read; never called from the RT thread).
    // True when it moved something — the pool uses that to decide whether it has
    // any work at all, so a stream whose source has ended cannot make a worker
    // spin. Bounded per call (kFillChunksPerCall) so one stream cannot hold a
    // worker while its other streams wait.
    bool FillOnce();

    // Does this stream still have less than `wantFrames` of audio ready AND
    // more it could produce? That is the question a build's short wait asks
    // between polls: false means "as primed as it can get" (the ring is full, or
    // the source has ended, or the stream has nothing left to play), which is
    // what stops a build waiting out its budget for audio that does not exist.
    bool NeedsPriming(Frame wantFrames) const {
        if (wantFrames < 0) wantFrames = 0;
        return fRing.ReadAvailable() < wantFrames * 2 && NeedsFilling();
    }


    // Called from the RT callback. Adds this stream's contribution for the
    // block [blockStart, blockStart+frames) into the interleaved stereo
    // `out`. Never blocks; on underrun it contributes silence. Always drains
    // the ring inside the clip window (even when inaudible) so an unmute
    // mid-playback stays sample-aligned with the playhead.
    //
    // The FIRST call after this stream's graph was published also re-aligns the
    // ring: see RebaseTo.
    //
    // `wraps` is the engine's loop-wrap counter (how many times the transport
    // has crossed the loop end, ever). Only the first call reads it, to know how
    // many loop passes the transport ran through while this graph was being
    // built (M4.2).
    void Mix(float* out, size_t frames, Frame blockStart, uint64_t wraps);

    // Live mix update from the UI thread: recompute per-channel gains and the
    // audible flag. Lock-free (atomic stores); the RT callback picks them up
    // at the next block. No thread restart, no reallocation.
    void SetMix(float gain, float pan, bool audible);

    // Update only the gain/pan (equal-power), leaving audibility as-is. Called
    // per block by automation from the RT thread.
    void SetGainPan(float gain, float pan);

    // Update only audibility, leaving gain/pan as-is (used by UpdateMix for
    // automated tracks, where automation owns the gain/pan).
    void SetAudible(bool audible);

    // The track's input trim and polarity (M3.4). Separate atoms from the
    // fader's: they are applied to this clip's contribution on the way into
    // the track, so a gain/pan automation pass — which overwrites fGainL/R
    // every block — can never drop them.
    void SetTrimPhase(float trim, bool invert) {
        fTrim.store(trim, std::memory_order_relaxed);
        fPhase.store(invert, std::memory_order_relaxed);
    }

    // The engine's xrun counter (M3.1): Mix increments it once per block in
    // which this clip's ring could not supply a frame. The ENGINE outlives
    // every graph it publishes, so the pointer stays valid for the stream's
    // whole life, and Mix only runs from the RT callback, which only runs
    // while the engine is alive.
    void SetXrunCounter(std::atomic<uint64_t>* counter) { fXruns = counter; }

    TrackId Track() const { return fTrackId; }
    Frame ClipStart() const { return fStart; }
    // The source ran out of data (a clip longer than its file, which the model
    // allows). Set by the fill worker, read by NeedsFilling()/the builder.
    std::atomic<bool> fSourceEof{false};
    Frame EndFrame() const { return fStart + fLength; }
    bool  Valid() const { return fSource && fSource->IsValid(); }
    float SourceRate() const { return fSource ? fSource->FrameRate() : 0.0f; }

    // Drop the frames this stream would have consumed since the graph was built
    // (the ring's origin is fStart + fSeekDelta) up to `timelineFrame`, so an
    // in-place rebuild joins the running transport in sync instead of playing
    // that far behind it for the life of the clip — under a loop, walking the
    // passes the transport ran through meanwhile (StreamLoop.h). RT thread only,
    // from Mix; RingBuffer::Skip is O(1) and anything the ring does not hold yet
    // is carried in the existing under-run debt (fSkipDebt).
    void RebaseTo(Frame timelineFrame, uint64_t wraps);

private:
    void DiskLoop();             // producer thread body
    // Seek the source to the start of the pass the cursor now holds, and drop
    // the resampler's carried state. Disk thread only.
    void SeekToPass();
    // One stretched pass's worth of progress: feed the resampled source (capped
    // at the pass's span) through the stretcher and push what it produces into
    // the ring. Disk thread only; `remaining` is the pass's output frames left.
    bool FillStretched(Frame remaining);

    TrackId     fTrackId;
    std::string fPath;
    Frame       fStart;
    Frame       fLength;
    Frame       fSourceOffset;  // source-frame offset into the file
    Frame       fSeekDelta;     // output frames into the clip to skip on start
    float       fOutputRate;    // engine output rate to resample to
    Frame       fFadeIn;        // fade-in length (timeline frames)
    Frame       fFadeOut;       // fade-out length (timeline frames)
    FadeShape   fFadeInShape  = FadeShape::Linear;   // the curves they render
    FadeShape   fFadeOutShape = FadeShape::Linear;   // with (M2.5)
    float       fClipGain = 1.0f;  // per-clip linear gain
    StreamLoopRange fLoop;      // the loop this stream plays under (M4.2)
    // The disk thread's pass walker (StreamLoop.h): which range of the clip it
    // is producing, and whether there is one left at all.
    ProduceCursor fCursor;
    // ... and the same fact for the BUILD thread: the walker is producer state,
    // and WaitPrimed must not wait out its whole timeout on a stream that has
    // nothing left to produce (a clip behind the playhead, or one the loop has
    // no pass left for). Set by the fill worker, read by the builder.
    std::atomic<bool> fDone{false};
    bool fProducingDone() const {
        return fDone.load(std::memory_order_relaxed);
    }
    // The unpushed remainder of the last decoded chunk, carried between
    // FillOnce calls (so a ring that filled mid-chunk resumes there) and the
    // scratch it points into. Touched only by this stream's own fill worker.
    const float* fChunk       = nullptr;   // points into fResampled
    size_t       fChunkFloats = 0;         // remaining floats to push
    size_t       fChunkOffset = 0;
    static constexpr size_t kMinFillFloats = 4096;   // ~1 block of stereo frames
    // Chunks one FillOnce call may push before returning to its worker.
    static constexpr int    kFillChunksPerCall = 8;
    bool FillStep();                 // one chunk's worth; FillOnce loops on it
    Frame       fPassWritten = 0;  // output frames of the current pass already
                                   // pushed into the ring (disk thread only)
    int64_t     fSkipDebt = 0;     // ring frames to drop to resync after xrun
    // One-shot: the first Mix after this stream's graph became active re-aligns
    // the ring (the publisher set nothing else; the origin is derivable from
    // fStart + fSeekDelta). Written by the builder before the graph is
    // published, cleared by the RT thread on first use — the atomic publish of
    // the graph orders those two.
    bool        fRebasePending = true;
    std::atomic<float> fGainL{0.0f};   // per-channel gain after equal-power pan
    std::atomic<float> fGainR{0.0f};
    std::atomic<bool>  fAudible{true};
    std::atomic<uint64_t>* fXruns = nullptr;   // the engine's counter (M3.1)
    std::atomic<float> fTrim{1.0f};    // input trim, pre-fader (M3.4)
    std::atomic<bool>  fPhase{false};  // polarity flip (M3.4)

    // The factory's reader for whatever the clip's file is: WAV, AIFF, FLAC or
    // Ogg Vorbis. Owned (the concrete readers are not copyable and their
    // lifetime is per-clip-stream), and null until Prepare() opens it.
    std::unique_ptr<IAudioSource> fSource;
    std::unique_ptr<Resampler> fResampler;   // source rate -> output rate
    std::vector<float>         fResampled;    // disk-thread scratch buffer
    RingBuffer  fRing;

    // --- the stretch stage (M7.2) -------------------------------------
    // Null — and every field below unused — unless the clip's ratio is
    // something other than 1.0, so an unstretched clip's disk path is exactly
    // the decode -> resample -> ring it always was.
    double      fStretchRatio = 1.0;
    std::unique_ptr<Stretcher> fStretcher;   // built once, in Open()
    std::vector<float> fStretchIn;       // resampled source not yet taken
    size_t      fStretchInOff    = 0;    // frames into fStretchIn
    size_t      fStretchInFrames = 0;
    std::vector<float> fStretchOut;      // stretcher output not yet pushed
    size_t      fStretchOutOff    = 0;
    size_t      fStretchOutFrames = 0;
    int64_t     fStretchFed    = 0;      // source frames fed this pass
    int64_t     fStretchSpan   = 0;      // source frames this pass may take
    int64_t     fStretchDrop   = 0;      // output frames to drop (warm start)
    bool        fStretchFlushed = false;
};

class Engine {
public:
    Engine();
    ~Engine();

    // ------------------------------------------------------------------
    // The graph: everything a rebuild replaces (M4.1).
    //
    // Built on the builder thread from a project snapshot and published to the
    // RT callback with a single atomic exchange. Its data is public on purpose:
    // it is the engine's private working set (the RT body, the UI-side live
    // updates and the builder all read it), not an API.
    //
    // After publication the only writers are the UI thread's live-parameter
    // paths (UpdateMix, SyncFx, SetFxParamLive) — the same atomics-only rule as
    // before the split — and the RT thread itself, which owns everything else.
    // ------------------------------------------------------------------
    class Graph {
    public:
        // One mix bus per track (audio / midi / bus). It sums its content (and,
        // for a bus, its upstream inputs), applies its FX, and routes into
        // `output` (another bus) or the master.
        struct Bus {
            TrackId                               id;
            TrackId                               output = kInvalidTrackId;  // 0 = master
            std::vector<TrackStream*>             streams;   // audio, owned by fStreams
            std::vector<MidiNote>                 notes;     // MIDI (empty otherwise)
            std::vector<MidiClipEvent>            events;    // MIDI CC/PB (channel)
            // The track's bend events, digested at build (sorted, phase-summed)
            // so the RT thread evaluates the bend for a block with a binary
            // search and never scans the event list. Empty for a track with no
            // pitch bend.
            std::vector<BendPoint>                bend;
            std::unique_ptr<IInstrument>          instrument;// voice (MIDI)
            // Live mix params: written by the UI thread (UpdateMix) and read by
            // the RT callback (FillBuffer). Atomic + relaxed so a concurrent
            // fader move can't tear a float into a NaN/garbage gain.
            // Independent scalars with no cross-field invariant, so relaxed
            // ordering is sufficient.
            std::atomic<float>                    midiGainL{1.0f};  // equal-power
            std::atomic<float>                    midiGainR{1.0f};
            std::atomic<float>                    busGainL{1.0f};   // bus fader
            std::atomic<float>                    busGainR{1.0f};
            // Input trim + polarity (M3.4), applied where the node's OWN
            // material is scaled (the MIDI fader / the bus fader). Separate
            // from the gain atoms above for the same reason the streams' are:
            // automation writes those every block.
            std::atomic<float>                    trim{1.0f};
            std::atomic<bool>                     phaseInv{false};
            bool                                  isBus   = false;
            std::atomic<bool>                     audible{true};
            bool                                  liveMonitor = false;  // armed MIDI: synth live input
            // Live-input route for this track: which endpoint (Midi Kit producer
            // id, 0 = any) and which MIDI channel (1..16, 0 = all) its monitored
            // notes may come from, so two keyboards drive two tracks
            // independently. Atomic because arming/assignment can change while
            // the player runs.
            std::atomic<int32_t>                  inEndpoint{0};
            std::atomic<int>                      inChannel{0};
            // This bus's share of the live voices, rebuilt per block on the RT
            // thread from the shared voice pool. Reserved at build so the RT
            // rebuild never allocates.
            std::vector<MidiNote>                 liveNotes;
            // The piano roll's audition clicks for this bus (package 51):
            // voices from Engine::AuditionNote, rebuilt per block like
            // liveNotes and rendered through the same instrument. Unlike
            // liveNotes this needs no armed track — the click sounds on the
            // region's own track.
            std::vector<MidiNote>                 auditionNotes;
            std::vector<std::unique_ptr<IEffect>> fx;
            std::vector<EffectType>               fxTypes;  // parallel to fx (SyncFx match)
            // Parallel to fx, both hashes rather than the strings: they are
            // compared on every SyncFx call, including during a knob drag.
            //   fxIdentity — hash of (type, plugin id). Two LV2 inserts at one
            //     index are the same EffectType, so the type alone cannot tell a
            //     replaced plugin from the one the chain was built with; and
            //     CaptureFxStates must not write one insert's patch into a
            //     descriptor that now holds a different plugin.
            //   fxStateHash — hash of the descriptor's stored state. A plugin's
            //     own state is applied at INSTANTIATION and cannot be pushed, so
            //     a changed state is structural (rebuild) even though the type
            //     matches.
            std::vector<uint64_t>                 fxIdentity;
            std::vector<uint64_t>                 fxStateHash;
            // Per-insert slot state, parallel to fx (see model/Effect.h). Atomic
            // because the UI thread pushes them through SyncFx while the RT
            // callback reads them per block — like the fader gains above:
            // independent scalars with no cross-field invariant, so relaxed
            // ordering is sufficient. Held as arrays rather than
            // std::vector<std::atomic<>> (which cannot grow, an atomic being
            // neither copyable nor movable), sized once at build, the same shape
            // as fNodePeakL/R.
            std::unique_ptr<std::atomic<bool>[]>  fxBypass;
            std::unique_ptr<std::atomic<float>[]> fxMix;
            // Per-insert dry-path delay of that effect's own LatencySamples().
            // Both the soft-bypass path and the wet/dry blend need the dry
            // signal delayed by exactly that, and they are mutually exclusive,
            // so one line serves both. Length 0 (a plain accumulate, no cost)
            // for every zero-latency effect — which is every built-in but the
            // look-ahead limiter.
            std::vector<FrameDelay>               fxDryDelay;
            // Aux sends: destination node index into fBuses, linear level, and a
            // PDC delay line aligning this send to the dest's input latency.
            // Taps this node's post-FX output. Dest node indices are resolved at
            // build, so the RT callback does no id lookups. The topo order
            // (built over output + send edges) guarantees each dest is processed
            // after us.
            struct SendTarget {
                size_t     dest;
                float      level;
                FrameDelay delay;   // PDC: aligns this send to dest.inputLatency
            };
            std::vector<SendTarget>               sendTargets;
            // External sidechain keys (package 05), one entry per fx slot
            // (parallel to `fx`, like fxBypass/fxMix):
            //   fxKeySrc   — the descriptor's source track id as built, kept so
            //                SyncFx can tell a source CHANGE from a param push: a
            //                source change moves graph edges, so it is structural;
            //   fxKeyNode  — that source resolved to a node index, or -1 for "no
            //                key routed this graph" (no source, a self-key, a
            //                source not in this graph, or a failed topo/PDC solve
            //                — a cycle. The insert then detects internally: fail
            //                soft);
            //   fxKeyDelay — the PDC delay line that aligns the key with THIS
            //                node's input, prepared to EdgeDelay(src, this) at
            //                build. Carries state across blocks, like SendTarget's.
            std::vector<TrackId>                  fxKeySrc;
            std::vector<long>                     fxKeyNode;
            std::vector<FrameDelay>               fxKeyDelay;
            // This node's own material is delayed by its PDC input latency before
            // the fader/fx run (length 0 — a no-op — for every node nothing
            // feeds). A keyed node is the first node that has BOTH its own
            // material and an input: its key arrives delayed to that latency, so
            // its material has to sit at the same buffer position or the detector
            // would compare audio and key frames from different timeline moments.
            FrameDelay                            inDelay;
            // PDC delay line on this node's OUTPUT edge (into `output` or the
            // master sink), aligning it to that destination's input latency.
            // Zero-length (a plain accumulate) when nothing on the path is
            // latent.
            FrameDelay                            outDelay;
            // Gain/pan automation (RT-owned snapshot, copied from the Track at
            // build). When a lane has points, the engine drives this node's
            // gain/pan per block from the lane (absolute, overriding the static
            // fader); UpdateMix leaves automated nodes alone.
            AutomationLane                        gainAuto;
            AutomationLane                        panAuto;
            std::vector<FxAutoLane>               fxAuto;   // effect-param automation
            float                                 statGain = 1.0f;  // ValueAt default
            float                                 statPan  = 0.0f;
            bool                                  hasAuto  = false;
            // Last MIDI channel gains (CC7 x CC11, placed by CC10) applied at
            // the end of the previous block. The next block ramps from here to
            // its own target so a stepped controller glides instead of clicking.
            // Negative = not yet established (first block after a build/seek),
            // which snaps instead of gliding from a stale value.
            float                                 chanL    = -1.0f;
            float                                 chanR    = -1.0f;

            // The atomic members make Bus non-copyable and suppress the implicit
            // move, but fBuses is a std::vector<Bus> that moves on growth/erase.
            // Hand-write a noexcept move that transfers the atomics by value
            // (all moves happen at build, single-threaded, before the build is
            // published).
            Bus() = default;
            Bus(Bus&& o) noexcept
                : id(o.id), output(o.output),
                  streams(std::move(o.streams)), notes(std::move(o.notes)),
                  events(std::move(o.events)),
                  bend(std::move(o.bend)),
                  instrument(std::move(o.instrument)),
                  midiGainL(o.midiGainL.load(std::memory_order_relaxed)),
                  midiGainR(o.midiGainR.load(std::memory_order_relaxed)),
                  busGainL(o.busGainL.load(std::memory_order_relaxed)),
                  busGainR(o.busGainR.load(std::memory_order_relaxed)),
                  trim(o.trim.load(std::memory_order_relaxed)),
                  phaseInv(o.phaseInv.load(std::memory_order_relaxed)),
                  isBus(o.isBus),
                  audible(o.audible.load(std::memory_order_relaxed)),
                  liveMonitor(o.liveMonitor),
                  inEndpoint(o.inEndpoint.load(std::memory_order_relaxed)),
                  inChannel(o.inChannel.load(std::memory_order_relaxed)),
                  liveNotes(std::move(o.liveNotes)),
                  auditionNotes(std::move(o.auditionNotes)),
                  fx(std::move(o.fx)), fxTypes(std::move(o.fxTypes)),
                  fxIdentity(std::move(o.fxIdentity)),
                  fxStateHash(std::move(o.fxStateHash)),
                  fxBypass(std::move(o.fxBypass)), fxMix(std::move(o.fxMix)),
                  fxDryDelay(std::move(o.fxDryDelay)),
                  sendTargets(std::move(o.sendTargets)),
                  fxKeySrc(std::move(o.fxKeySrc)),
                  fxKeyNode(std::move(o.fxKeyNode)),
                  fxKeyDelay(std::move(o.fxKeyDelay)),
                  inDelay(std::move(o.inDelay)),
                  outDelay(std::move(o.outDelay)),
                  gainAuto(std::move(o.gainAuto)), panAuto(std::move(o.panAuto)),
                  fxAuto(std::move(o.fxAuto)),
                  statGain(o.statGain), statPan(o.statPan), hasAuto(o.hasAuto),
                  chanL(o.chanL), chanR(o.chanR) {}
            Bus& operator=(Bus&& o) noexcept {
                if (this == &o) return *this;
                id = o.id; output = o.output;
                streams = std::move(o.streams); notes = std::move(o.notes);
                events = std::move(o.events);
                bend = std::move(o.bend);
                instrument = std::move(o.instrument);
                midiGainL.store(o.midiGainL.load(std::memory_order_relaxed), std::memory_order_relaxed);
                midiGainR.store(o.midiGainR.load(std::memory_order_relaxed), std::memory_order_relaxed);
                busGainL.store(o.busGainL.load(std::memory_order_relaxed), std::memory_order_relaxed);
                busGainR.store(o.busGainR.load(std::memory_order_relaxed), std::memory_order_relaxed);
                // The M3.4 pair rides these hand-written moves too: a bus moved
                // after its trim was set would otherwise come back at unity.
                trim.store(o.trim.load(std::memory_order_relaxed), std::memory_order_relaxed);
                phaseInv.store(o.phaseInv.load(std::memory_order_relaxed), std::memory_order_relaxed);
                isBus = o.isBus;
                audible.store(o.audible.load(std::memory_order_relaxed), std::memory_order_relaxed);
                liveMonitor = o.liveMonitor;
                inEndpoint.store(o.inEndpoint.load(std::memory_order_relaxed), std::memory_order_relaxed);
                inChannel.store(o.inChannel.load(std::memory_order_relaxed), std::memory_order_relaxed);
                liveNotes = std::move(o.liveNotes);
                // The audition's list rides these hand-written moves too: a bus
                // moved after its reserve (the graph build's push_back) would
                // otherwise come back with capacity 0, and the RT hand-off's
                // `size() < capacity()` guard would then silently drop every
                // click (the whole feature dead, with no crash to show for it).
                auditionNotes = std::move(o.auditionNotes);
                fx = std::move(o.fx); fxTypes = std::move(o.fxTypes);
                fxIdentity = std::move(o.fxIdentity);
                fxStateHash = std::move(o.fxStateHash);
                fxBypass = std::move(o.fxBypass); fxMix = std::move(o.fxMix);
                fxDryDelay = std::move(o.fxDryDelay);
                sendTargets = std::move(o.sendTargets);
                fxKeySrc = std::move(o.fxKeySrc);
                fxKeyNode = std::move(o.fxKeyNode);
                fxKeyDelay = std::move(o.fxKeyDelay);
                inDelay = std::move(o.inDelay);
                outDelay = std::move(o.outDelay);
                gainAuto = std::move(o.gainAuto); panAuto = std::move(o.panAuto);
                fxAuto = std::move(o.fxAuto);
                statGain = o.statGain; statPan = o.statPan; hasAuto = o.hasAuto;
                chanL = o.chanL; chanR = o.chanR;
                return *this;
            }
        };

        // A live-monitored note held on the keyboard (or in its release tail).
        struct LiveVoice {
            bool    active    = false;
            bool    releasing = false;   // note-off seen; ringing out the tail
            // The pedal (CC64) took this voice's note-off instead of releasing
            // it: it stays silent-free until the pedal lifts, or until the key
            // is struck again. Never true for a voice already `releasing`.
            // See model/Sustain.h.
            bool    sustained = false;
            uint8_t pitch     = 0;
            uint8_t vel       = 0;
            uint8_t channel   = 0;
            Frame   start     = 0;       // engine frame the note-on landed on
            Frame   off       = 0;       // engine frame of note-off (if releasing)
            int32_t source    = 0;       // endpoint the note-on arrived from
            // Bend phase the voice was born with (engine frames): the live
            // accumulator at its note-on, so a wheel move bends it from where
            // it started rather than jumping its phase.
            double  bend      = 0.0;
            // An audition voice (Engine::AuditionNote): it renders through the
            // bus whose id is Engine::fAuditionTrack rather than through the
            // armed tracks' monitor route, and switching monitoring off must
            // not release it. Last member so the existing brace initializers
            // stay valid.
            bool    audition  = false;
        };
        static constexpr int kMaxLiveVoices = 64;

        // The key a live voice is held under: channel * 128 + pitch, so the
        // latch tracks two keyboards' channels apart. (Two endpoints on the
        // same channel share a key; the note-off match in UpdateLiveVoices
        // still tells those voices apart by source.)
        static int VoiceKey(uint8_t channel, uint8_t pitch) {
            return (int)channel * 128 + (int)pitch;
        }

        // The RT body. `e` supplies the state that outlives this graph (the
        // transport, the live toggles, the UI-facing publishing storage).
        void Render(Engine& e, float* out, size_t frames);

        // One segment of a callback: the body of Render for the block
        // [blockStart, blockStart+frames). Render calls it once per block, or
        // twice when the block straddles the loop end — the second segment
        // starting at the loop start, which is what makes the loop's jump
        // sample-exact (M4.2). It does not touch the playhead: the position is
        // the driver's. `wraps` is Engine::fWraps minus this graph's
        // fWrapsAtRequest: the pass count the in-place join needs.
        void RenderSegment(Engine& e, float* out, size_t frames, Frame blockStart,
                           uint64_t wraps);

        // Drop every live voice, so a note held at Stop can't sustain (up to
        // kHeld) or re-sound if the same engine restarts. Safe once the player
        // is stopped. The live controllers are cleared with them: a bend or a
        // pedal left "down" by a keyboard that is no longer sending would
        // otherwise colour the next start.
        // Which track's bus hears the audition clicks, snapshotted at BUILD
        // (from BuildRequest::auditionTrack): the bus set and the routing then
        // cannot disagree -- routing by the Engine's live atomic while the
        // graph was built for another target silently dropped clicks.
        TrackId fAuditionTrack = kInvalidTrackId;

        void ResetVoices() {
            for (LiveVoice& v : fVoices) v.active = false;
            fLiveNotes.clear();
            for (Bus& b : fBuses) b.auditionNotes.clear();
            for (int i = 0; i < 128; i++) fLiveCc[i] = 0;
            fLiveBendRatio = 1.0f;
            fLiveBendPhase = 0.0;
            fSustain.Reset();
        }

        // Worker thread, just before the graph is published: write the current
        // per-track live-input routes onto the buses (atomics).
        void ApplyRoutes(const std::vector<MidiInputRoute>& routes);

        Frame BuildStart() const { return fBuildStart; }
        Frame EndFrame()   const { return fEndFrame; }
        float OutputRate() const { return fOutputRate; }

        // Everything below is the graph's working set (see the class comment).
        Frame                                     fBuildStart = 0;   // ring origins
        Frame                                     fEndFrame   = 0;
        float                                     fOutputRate = 48000.0f;
        // The loop this graph plays under (M4.2), resolved at build from the
        // snapshot with LoopActive() — so a range too short to walk is off, and
        // the streams got the same answer this graph wraps by. Fixed for the
        // graph's life: a loop edit re-positions the engine rather than editing
        // content in place, which is what lets the disk threads and the RT
        // callback agree on the seam without any handshake at all.
        bool                                      fLooping    = false;
        Frame                                     fLoopStart  = 0;
        Frame                                     fLoopEnd    = 0;
        // The engine's wrap counter when this graph was REQUESTED, and whether
        // this graph joins a transport that kept rolling (an in-place rebuild).
        // Together they tell the join how many passes the transport ran through
        // while this graph was being built (LoopRebaseSkipFrames). A
        // position-changing load needs neither: its playhead is set to its own
        // origin and nothing advances until it is published, so it joins with no
        // skip at all — and counting the wraps the graph it replaced performed
        // in the meantime would skip a loop's worth of audio.
        uint64_t                                  fWrapsAtRequest = 0;
        bool                                      fInPlaceJoin = false;
        std::vector<std::unique_ptr<TrackStream>> fStreams;
        // The workers that keep those streams filled (StreamPool.h). Declared
        // AFTER fStreams on purpose: members are destroyed in reverse order, so
        // the pool's threads are stopped and joined before any stream it is
        // filling is destroyed — on this graph's reclaim thread, exactly where
        // the per-stream threads used to be joined.
        std::unique_ptr<StreamPool<TrackStream>>  fPool;
        std::vector<Bus>                          fBuses;
        std::vector<std::vector<float>>           fNodeBufs;  // one mix buffer per node
        std::vector<size_t>                       fOrder;     // node indices, topo order
        std::unique_ptr<std::atomic<float>[]>     fNodePeakL; // per-node output peak
        std::unique_ptr<std::atomic<float>[]>     fNodePeakR;
        std::vector<std::unique_ptr<IEffect>>     fMasterFx;  // master bus chain
        std::vector<EffectType>                   fMasterFxTypes;  // parallel (SyncFx)
        std::vector<uint64_t>                     fMasterFxIdentity;   // parallel
        std::vector<uint64_t>                     fMasterFxStateHash;  // parallel
        // Master-chain insert state, same shape and rules as Bus::fxBypass/
        // fxMix/fxDryDelay above. Note fMasterFx DROPS effects that fail to
        // build, so these are parallel to fMasterFx, not to project.masterFx.
        std::unique_ptr<std::atomic<bool>[]>      fMasterFxBypass;
        std::unique_ptr<std::atomic<float>[]>     fMasterFxMix;
        std::vector<FrameDelay>                   fMasterFxDelay;
        // Master-chain external sidechain keys, the same three parallel arrays
        // the Bus keeps (see Bus::fxKeySrc). The key is a track even when the
        // consumer is the master, so a master insert can be ducked by any track.
        std::vector<TrackId>                      fMasterFxKeySrc;
        std::vector<long>                         fMasterFxKeyNode;
        std::vector<FrameDelay>                   fMasterFxKeyDelay;
        // One block of dry signal, held while an insert's wet leg is computed
        // in place. Preallocated at build (RT never allocates); nodes and the
        // master run sequentially inside one callback, so a single buffer
        // serves them all.
        std::vector<float>                        fScratch;
        // One block that holds a node's own material while it is delayed by
        // that node's PDC input latency (only used when that latency is
        // non-zero), and one that holds a sidechain key while its per-edge delay
        // line runs. Preallocated at build; both are read and re-filled within
        // one node's turn, so a single pair serves every node and every insert.
        std::vector<float>                        fMaterial;
        std::vector<float>                        fKeyScratch;
        Frame                                     fMonFrame = 0;  // monitor clock
        LiveVoice                                 fVoices[kMaxLiveVoices];
        std::vector<MidiNote>                     fLiveNotes; // rebuilt each block (RT)
        // Live channel controllers, kept from the incoming event stream so a
        // bend, a wheel or the pedal reaches the voices that are already
        // sounding. Fixed arrays written and read only on the RT thread — no
        // allocation, no locks. They live with the voice pool they drive, so a
        // graph carries the live state that belongs to its own voices.
        //
        // These are GRAPH-wide, not per endpoint: a second keyboard's wheel
        // would move every monitored track (the note path demuxes by endpoint,
        // this does not). Stated in the record as the known limit of live
        // expression.
        uint8_t                                   fLiveCc[128] = {};   // 0..127, CC64 = pedal
        float                                     fLiveBendRatio = 1.0f;
        // ∫(bendRatio − 1) dt over the graph's life, in frames: the live twin
        // of MidiExpression's Δ, advanced once per block from fLiveBendRatio. A
        // note-on stamps its value into the new voice, so a wheel move never
        // jumps the phase of a note that is already sounding.
        double                                    fLiveBendPhase = 0.0;
        SustainLatch                              fSustain;            // live CC64 latch
        Metronome                                 fMetronome; // click generator
        TempoMap                                  fTempoMap;  // for tempo-synced fx
        double                                    fLastFxBpm = 0.0;  // last pushed bpm
        Loudness                                  fLoudness;  // master BS.1770 meter
        // The master meter's RMS/correlation window. Prepared at build (off the
        // RT thread), so the callback only does arithmetic. Per graph, because
        // building it must never touch state a running callback is reading.
        StereoMeter                               fStereoMeter;
        std::vector<float>                        fMonBuf;   // RT scratch for monitor reads
        // Resampled-monitor state (when the monitor rate != output rate): a
        // linear pull-resampler carried across blocks. RT-only.
        std::vector<float>                        fMonSrc;   // source-rate scratch
        double                                    fMonPhase = 0.0;
        float                                     fMonPrevL = 0.0f, fMonPrevR = 0.0f;

    private:
        // RT-only helpers, split out of Render for readability.
        void UpdateLiveVoices(Engine& e, Frame blockStart, size_t frames);

        // The live keyboard's own drain (one event at a time into the voice
        // pool): its note-on takes a free voice or steals the oldest, its
        // note-off releases the matching one. Called from UpdateLiveVoices
        // while a live input is attached.
        void DrainLiveInput(IMidiInput* in, Frame blockStart);

        // The piano roll's audition clicks (package 51): their own ring, the
        // same voice pool, and a note-on that renders through the bus named by
        // Engine::fAuditionTrack rather than through a monitored route. Runs
        // whether or not a keyboard is attached.
        void DrainAudition(Engine& e, Frame blockStart);

        // Claim a free voice for a note-on, or steal the oldest. One place, so
        // the keyboard's and the audition's notes cannot drift apart.
        void TakeVoice(int pitch, int velocity, int channel, int32_t source,
                       Frame blockStart, bool audition);

        // A note-off: release the voice matching (pitch, channel, source), or
        // let the pedal defer it (Sustain.h).
        void ReleaseVoice(int pitch, int channel, int32_t source, Frame blockStart);

        // The pedal lifted at `at`: every voice it was holding stops ringing
        // there. A voice the player is still holding down is untouched — the
        // pedal only releases what it took (model/Sustain.h).
        void ReleaseSustained(Frame at);

        // The live channel expression for this block: the bend ratio and phase
        // the accumulator has reached, plus CC1. Recorded notes get theirs from
        // the track's events (MidiExpression.h); a live note uses this instead.
        VoiceExpression LiveExpression() const;

        void RenderMonitorOnly(Engine& e, float* out, size_t frames);
    };

    // ------------------------------------------------------------------
    // Loading.
    // ------------------------------------------------------------------

    // What a rebuild request means for the transport:
    enum class LoadMode {
        // The transport is (re)positioning: play, seek, record start, monitor
        // start. The active graph is detached at request time —
        // its content belongs to the old position — so the RT renders silence
        // and the playhead stands still until the new graph is published.
        // (A loop wrap is NOT one of these any more: the running graph walks
        // the loop itself, M4.2.)
        NewPosition,
        // The content changed at an unchanged position (a structural edit
        // during playback). The active graph keeps playing and keeps advancing;
        // the new graph joins at the current playhead through its per-stream
        // rebase, so the edit is heard with no gap.
        InPlace,
    };

    // Make sure the output device is open for this project. Runs on the CALLER's
    // thread (the window thread): opening the device is the one part of
    // starting playback whose failure the user is shown synchronously. The
    // player is then kept: a rebuild never re-creates it, so every rebuild
    // after the first is device-free. Re-created only when the requested sample
    // rate or SetBufferFrames value differs from the live one.
    status_t EnsurePlayer(const Project& project);

    // Snapshot the project, queue a rebuild and return — the window thread is
    // free immediately; the graph lands when the builder is done. A newer
    // request replaces a queued one (superseding a build already in flight,
    // whose result is then discarded rather than published stale). The
    // synchronous wrapper below waits. `noLoop` builds the graph without the
    // snapshot's loop (the record path; see BuildRequest).
    void RequestLoad(const Project& project, Frame startFrame, Frame minEndFrame,
                     LoadMode mode, bool noLoop = false);

    // Old synchronous entry point (prototypes and the monitor path): ensure the
    // player, request, wait, and return the build's status.
    status_t Load(const Project& project, Frame startFrame = 0,
                  Frame minEndFrame = 0);

    // Wait for every request issued so far to have completed. False on timeout.
    bool WaitForLoad(bigtime_t timeoutUs = 10000000);

    void Start();
    void Stop();

    // ---- counters for the tests (M4.1) --------------------------------
    // Graphs published to the RT side (each is one completed successful load).
    uint64_t GraphsPublished() const {
        return fGraphsPublished.load(std::memory_order_relaxed);
    }
    // Requests whose build has finished, successful or not.
    uint64_t LoadsCompleted() const {
        return fCompletedGen.load(std::memory_order_acquire);
    }
    // The most recent completed build's status (B_OK, B_ENTRY_NOT_FOUND, ...).
    status_t LastLoadStatus() const {
        return (status_t)fLoadStatus.load(std::memory_order_relaxed);
    }
    // Is the player's callback running now? (Start sets it, Stop clears it.)
    // The window needs it to tell "the clicks own this engine" from "the
    // transport does, and it merely looks the same".
    bool PlayerRunning() const {
        return fPlayerRunning.load(std::memory_order_acquire);
    }
    // Audition notes actually rendered by the callback (package 51): the count
    // is the whole end-to-end proof of the click path that a test can make
    // without ears -- a bus that lost its reserved list (the hand-written move)
    // or a graph built for another target renders none of them.
    uint64_t AuditionNotesRendered() const {
        return fAuditionRendered.load(std::memory_order_relaxed);
    }
    // How many BSoundPlayers have been opened (a rebuild must not move this).
    uint64_t PlayersOpened() const {
        return fPlayersOpened.load(std::memory_order_relaxed);
    }
    // How many times the player has been started (a rebuild must not move this
    // either: stop/start is what would make an edit audible as a gap).
    uint64_t PlayerStarts() const {
        return fPlayerStarts.load(std::memory_order_relaxed);
    }
    // How many times the transport has crossed a loop end (M4.2). A seamless
    // loop leaves GraphsPublished, PlayersOpened and PlayerStarts exactly where
    // they were and moves only this one — which is what the functional test
    // asserts, and what the window's pulse watches to know a record pass has
    // come round again.
    uint64_t Wraps() const {
        return fWraps.load(std::memory_order_relaxed);
    }
    // How long the most recent graph build took, in microseconds, measured on
    // the builder thread around BuildGraph. This is the plan's audio-start
    // budget measured where it belongs: the window's own message latency is not
    // the engine's, and on a loaded VM it is larger than the build itself.
    bigtime_t LastBuildUs() const {
        return fLastBuildUs.load(std::memory_order_relaxed);
    }
    // ---- the audio thread's own load, and its xruns (M3.1) ----------------
    // How long the last content callback took, in microseconds. Measured in
    // PlayTrampoline AROUND FillBuffer: the callback body itself stays
    // clock-free (no syscall, allocation, lock or I/O in FillBuffer — the RT
    // rule), and this is the one place the audio thread reads the clock.
    bigtime_t LastCallbackUs() const {
        return fLastCallbackUs.load(std::memory_order_relaxed);
    }
    // The worst callback since this was last called, and clears it. A load
    // figure that matters is the PEAK: an average over a second hides exactly
    // the block that overran and caused the click. The 60 Hz pulse reads it,
    // so what it reports is the worst block in the last ~16 ms window.
    bigtime_t TakePeakCallbackUs() {
        return fPeakCallbackUs.exchange(0, std::memory_order_relaxed);
    }
    // A callback duration as a share of the buffer it had to fill, in percent
    // (0..999). The budget is the buffer duration at the output rate, which is
    // what an overrun would exceed.
    float LoadPercentFor(bigtime_t callbackUs) const;
    // ... for the callback that ran last.
    float CallbackLoadPercent() const { return LoadPercentFor(LastCallbackUs()); }
    // Frames the callback must fill, and the rate it fills them at.
    size_t BufferFrames() const { return fBufferFrames; }
    // Ring underruns the RT side has seen: one per block in which a clip's
    // stream could not supply its frames (the `fSkipDebt` path in
    // TrackStream::Mix). Counted on the audio thread, read here.
    uint64_t Xruns() const {
        return fXruns.load(std::memory_order_relaxed);
    }

    // Is a graph in place for the RT side to render?
    bool HasGraph() const { return fGraphs->Active() != nullptr; }
    // Is a rebuild in flight?
    bool GraphPending() const {
        return fRequestGen.load(std::memory_order_acquire)
               > fCompletedGen.load(std::memory_order_acquire);
    }

    // Recompute every stream's gain/pan/audibility from the model, live,
    // without rebuilding the graph. Safe to call from the UI thread while
    // playing (writes atomics the RT callback reads).
    void UpdateMix(const Project& project);

    // Push effect-parameter edits into the running graph without a rebuild, so
    // knob tweaks take effect during playback. Only chains whose structure still
    // matches (same effect types, same count) are updated via SetParam (RT-safe);
    // add/remove/reorder still needs a rebuild (the caller requests one, in
    // place or at a new position). Returns true if every chain matched and was
    // synced; false if any chain's structure changed. Safe to call from the UI
    // thread while playing.
    bool SyncFx(const Project& project);

    // Tell every effect the current tempo (for tempo-synced params, e.g. a
    // delay locked to note divisions). Off-RT; call at Load + on tempo change.
    void SetFxTempo(double bpm);

    // Has any live insert's effect moved its own latency? (M4.6)
    //
    // An LV2 plugin may publish a new latency at any time (4K EQ 2 was
    // measured 0 -> 27 -> 0), and the delay lines that compensate it were
    // sized once, from the value the graph was BUILT with. Comparing each
    // effect's ObservedLatencySamples() with its committed LatencySamples()
    // decides whether the graph has to be re-solved.
    //
    // OFF the RT thread (the UI pulse calls it): it pins the active graph and
    // reads atomics the RT thread publishes — no locks, no plugin calls. A
    // true answer means "rebuild in place", which re-settles every effect
    // (LatencyProbe.h) so the new PDC solve and the new delay lines use the
    // value the plugin is really running with, and GraphSwap publishes them
    // at a block boundary.
    //
    // Asking twice about the same unchanged value answers false the second
    // time: the record of what a rebuild was already requested for is what
    // stops a plugin whose report never matches a fresh instance's settle from
    // asking for a rebuild on every pulse.
    bool FxLatencyMoved();

    // Pull every live insert's own state (IEffect::SaveState) into the matching
    // descriptor of `project` — what a save, an export snapshot or an autosave
    // calls first, so the file (and the render) holds the patch the engine is
    // playing. The model's params describe the control ports and nothing else; a
    // synth's patch exists only in the running instance until this runs.
    //
    // Off the RT thread (lilv allocates), and safe to call while the transport
    // rolls: LV2's state save threading class is exactly "may run alongside
    // Process". An insert the engine has no instance for keeps whatever state
    // the project already stores.
    void CaptureFxStates(Project& project);

    // Push a single effect parameter into the running graph live (for smooth
    // knob-drag response during playback, before the edit is committed). RT-safe
    // SetParam; `master` targets the master chain, else the track's node.
    void SetFxParamLive(TrackId track, bool master, int fxIndex, int slot,
                        float value);

    // Publish a watched insert's live parameter values for its open native
    // editor. Addressed exactly like SetFxParamLive (same track/master/fxIndex),
    // and resolved against the RUNNING chain on every block, so a rebuild cannot
    // leave it pointing at a freed instance. fxIndex < 0 stops watching `slot`.
    //
    // Several editors can be open at once (one per insert, in different
    // windows), so watches are slots the caller allocates: the same insert may
    // not be watched twice, and a slot is reusable once stopped.
    //
    // This exists because automation drives the plugin's control ports from the
    // audio thread and never touches the model, so the engine's copy is the only
    // place those values can be read from -- and a UI must not read them off the
    // instance the audio thread is running.
    static constexpr int kWatchSlots = 4;
    void SetFxWatch(int slot, TrackId track, bool master, int fxIndex) {
        if (slot < 0 || slot >= kWatchSlots) return;
        // Forget what was published: a NEW watched insert whose values happen to
        // match the old one's would otherwise change nothing and never publish,
        // leaving the freshly opened editor with no frame at all.
        fWatchN[slot].store(-1, std::memory_order_relaxed);
        fWatchMaster[slot].store(master, std::memory_order_relaxed);
        fWatchFx[slot].store(fxIndex, std::memory_order_relaxed);
        fWatchTrack[slot].store(fxIndex < 0 ? kInvalidTrackId : track,
                                std::memory_order_relaxed);
    }

    // Publish every watched insert's values NOW, from the calling thread, for
    // when there is no audio block to do it: the transport is stopped and a
    // live parameter write just changed something (the generic panel with a
    // native editor open). Returns immediately while the audio callback runs --
    // then the block publishes on its own, and this must not race it.
    void PublishFxWatchNow();

    static constexpr int kWatchMax = 64;
    // One watched insert's parameter values as of the last audio block. Written
    // by the RT thread, read here; the array is bracketed by a seqlock
    // generation (odd = mid-write) so a reader cannot see a torn frame. Returns
    // the count copied and the generation, which the caller compares against its
    // previous call: the same value means nothing changed and the caller can
    // skip the frame entirely. 0 values means this slot is not watching, or the
    // watched insert is not in the running chain.
    int WatchedFxParams(int slot, float* out, int maxSlots,
                        uint32_t* generation) const {
        if (slot < 0 || slot >= kWatchSlots) return 0;
        uint32_t g0 = 0, g1 = 0;
        int c = 0;
        do {
            g0 = fWatchGen[slot].load(std::memory_order_acquire);
            if (g0 & 1u) continue;              // mid-write; retry
            const int n = fWatchN[slot].load(std::memory_order_relaxed);
            // Nothing has ever been published for this slot (no watch, or the
            // watched insert is not in the running chain). 0 values, and the
            // caller's generation is left alone rather than clobbered.
            if (n < 0) return 0;
            c = n < maxSlots ? n : maxSlots;
            for (int i = 0; i < c; i++) out[i] = fWatchValues[slot][i];
            // The fence goes AFTER the copy. The closing load is an acquire, and
            // acquire orders nothing before it -- so without this the value
            // loads may sit above it, and the g0 == g1 test would then accept a
            // torn frame (visible on a weakly-ordered machine only, which is
            // exactly why it must not be left to a reader to notice).
            std::atomic_thread_fence(std::memory_order_acquire);
            g1 = fWatchGen[slot].load(std::memory_order_acquire);
        } while ((g0 & 1u) || g0 != g1);
        if (generation) *generation = g0;
        return c;
    }

    // Toggle the metronome click (RT-safe atomic).
    void SetMetronome(bool on) { fMetronomeOn.store(on); }
    // The click's level and sound (M3.5, the Preferences window). RT-safe
    // atomics like the toggle: a change is audible on the next block, and the
    // default (0.3, click) is exactly what the engine always played.
    void SetMetronomeGain(float gain) { fMetronomeGain.store(gain); }
    void SetMetronomeSound(int sound) { fMetronomeSound.store(sound); }
    float MetronomeGain() const { return fMetronomeGain.load(); }
    int   MetronomeSound() const { return fMetronomeSound.load(); }

    // Monitor section (RT-safe atomics). These affect only what is played out,
    // NOT the peak meters (which reflect the true mix). Dim drops the monitor
    // ~20 dB; Mono sums L+R to both channels for mono/phase checking.
    void SetMonitorDim(bool on)  { fMonitorDim.store(on); }
    void SetMonitorMono(bool on) { fMonitorMono.store(on); }

    // Input monitoring: mix a live input source into the output. The source is
    // only mixed when its rate matches the engine's output rate (no RT-side
    // resampling). Both are RT-safe atomic stores.
    void SetMonitorSource(IMonitorSource* src) { fMonSource.store(src); }
    void SetInputMonitor(bool on) { fInputMonitor.store(on); }

    // Live MIDI monitoring: while recording, incoming events from this input are
    // synthesized in real time through each armed MIDI track's instrument, so
    // the player hears themselves. RT-safe atomic; nullptr disables. The events
    // come from MidiInputPort::MonitorInput() (a ring separate from the record
    // drain, so the RT thread is the sole consumer).
    void SetLiveMidi(IMidiInput* in) { fLiveMidi.store(in); }

    // --- audition (package 51) ---------------------------------------------
    // The piano roll's clicks. An audition note renders through the instrument
    // + fader + FX of ONE track — the region's own — which is what makes an
    // audition the note the region will actually play, rather than a bare
    // synth blip. It needs no keyboard and no armed track, which is why it has
    // its own small ring rather than riding the live-input path. The engine
    // must be running: the window raises the monitor-only engine for the
    // clicks when the transport is stopped (MainWindow::EnsureAuditionEngine),
    // and the track must be AUDIBLE (a muted track's clicks stay silent, as
    // its playback does).
    void SetAuditionTrack(TrackId track) {
        fAuditionTrack.store(track, std::memory_order_relaxed);
    }
    TrackId AuditionTrack() const {
        return fAuditionTrack.load(std::memory_order_relaxed);
    }
    // `on` false with the note still down releases it; a released voice rings
    // out its own release tail. RT-safe: a lock-free push into the ring the
    // callback drains (a full ring drops the event, as the live input's does).
    void AuditionNote(int pitch, int velocity, bool on);

    // Per-track live-input routing. The caller (MainWindow) resolves each MIDI
    // track's endpoint NAME to a Midi Kit producer id when it opens the input
    // and pushes the result here; the RT thread then filters purely on ids. A
    // track with no entry stays permissive (hears every source), so this is a
    // no-op until inputs are actually assigned. Safe to call while playing; the
    // worker re-applies the list to a graph it is about to publish.
    void SetMidiRoutes(const std::vector<MidiInputRoute>& routes);

    // After detaching a monitor/live-MIDI source (SetMonitorSource(nullptr) or
    // SetLiveMidi(nullptr)), call this before destroying that source object.
    // Returns true once no in-flight RT callback can still dereference the old
    // pointer (immediately so when the player isn't running). Returns false if
    // quiescence could NOT be confirmed within a bounded wait — in that case the
    // caller must NOT free the source; it must Stop() the engine first (Stop
    // blocks until the RT thread truly quiesces) and only then destroy it.
    // Closes the teardown-ordering use-after-free window on those raw pointers.
    bool QuiesceMonitorInput();

    // Monitor-only: run the output for live MIDI monitoring WITHOUT playing the
    // project (no clips, no playhead advance) — used to hear an armed MIDI track
    // while idle. Set before Load()/Start(). The RT callback renders only the
    // live voices through each armed MIDI track's instrument + fader.
    void SetMonitorOnly(bool on) { fMonitorOnly.store(on); }

    // Output buffer size in frames (per channel); applied when the player is
    // next (re)created, i.e. before the device opens. Smaller = lower latency,
    // higher xrun risk. Call before Load().
    void SetBufferFrames(size_t n) { if (n >= 32) fBufferFrames = n; }

    // True once the playhead has passed the end of all clips.
    bool  IsFinished() const { return fFinished.load(); }
    Frame Playhead() const { return fPlayhead.load(); }

    // Master output peak (abs) of the last mixed block, per channel, in
    // [0, 1+]. Written by the RT callback, read by the UI meter poll.
    float PeakL() const { return fPeakL.load(); }
    float PeakR() const { return fPeakR.load(); }

    // Master meter readings for the M3.3 meter, from the same post-master-gain
    // block the peaks come from: the stereo RMS over a sliding window, and the
    // correlation coefficient between the channels (-1..+1, 0 with no signal).
    // The window lives in a StereoMeter the graph was given at build, so this
    // costs arithmetic only — no allocation, locks or I/O on the RT side.
    float RmsL() const { return fRmsL.load(); }
    float RmsR() const { return fRmsR.load(); }
    float Correlation() const { return fCorr.load(); }

    // Clip flags, one bit per channel (kMeterClipL/R in dsp/MeterMath.h): the
    // RT callback latches a bit at digital full scale, the UI reads the word
    // and clears it here. Reading and clearing are separate so a clip between
    // the two is not lost (ClearClipFlags is a fetch_and).
    uint32_t ClipFlags() const { return fClipFlags.load(std::memory_order_relaxed); }
    void ClearClipFlags(uint32_t mask) {
        fClipFlags.fetch_and(~mask, std::memory_order_relaxed);
    }

    // Per-track output peak (post-FX node level) for track meters. 0 if the
    // track isn't a live node or nothing is playing.
    float TrackPeakL(TrackId id) const;
    float TrackPeakR(TrackId id) const;

    // Master loudness of the mix, updated per block by the RT callback. LUFS
    // (momentary 400 ms / short-term 3 s) and true peak in dBTP. Silence reads
    // Loudness::kSilenceLufs / kSilenceDb.
    float LufsMomentary() const { return fLufsM.load(); }
    float LufsShort()     const { return fLufsS.load(); }
    float TruePeakDb()    const { return fTpDb.load(); }

    float OutputRate() const { return fOutputRate; }

    // Effect metering for the editor. The UI names one track to watch; the RT
    // thread copies each of that track's effects' meter (scalar) + the spectrum
    // of one analyzing effect (e.g. the EQ) into flat storage the UI reads, so
    // the UI never touches the RT-owned effect objects. ~0 = master chain,
    // kInvalidTrackId = disabled.
    void SetMeterFocus(TrackId track) {
        fMeterTrack.store(track, std::memory_order_relaxed);
    }
    static constexpr int kMeterFxMax = 16;
    float MeterGrDb(int fxIndex) const {
        return (fxIndex >= 0 && fxIndex < kMeterFxMax)
               ? fMeterGr[fxIndex].load(std::memory_order_relaxed) : 0.0f;
    }
    // Spectrum of the analyzing effect + which fx index it belongs to. The
    // spectrum array is published with a seqlock: the RT thread rewrites it
    // every block, so the UI retries its copy until it reads a stable frame
    // (avoids a torn mix of two frames — a data race on the plain-float array).
    int MeterSpectrum(float* out, int maxBins, int* fxIndex) const {
        uint32_t g0, g1;
        int c = 0;
        do {
            g0 = fMeterSpecGen.load(std::memory_order_acquire);
            if (g0 & 1u) continue;   // odd = mid-write; retry
            const int n = fMeterSpecN.load(std::memory_order_relaxed);
            c = n < maxBins ? n : maxBins;
            for (int i = 0; i < c; i++) out[i] = fMeterSpec[i];
            g1 = fMeterSpecGen.load(std::memory_order_acquire);
        } while ((g0 & 1u) || g0 != g1);
        if (fxIndex) *fxIndex = fMeterSpecFx.load(std::memory_order_relaxed);
        return c;
    }

private:
    // ------------------------------------------------------------------
    // Off-RT access to the active graph.
    //
    // The RT callback is protected by the callback-generation rule (see
    // fRetireGen): a graph retired at generation R is freed only after R+2
    // boundaries. The UI thread's live-parameter and meter paths have no such
    // bound — a window thread preempted for two audio blocks while holding the
    // pointer would be reading (and writing atomics into) a graph the reclaimer
    // had freed. So they count themselves in first, and the reclaimer waits for
    // the count to fall back to zero.
    //
    // Pin BEFORE loading the pointer — that ordering is what makes a reclaimer
    // that reads zero safe: if it did not see this pin, the pin came after its
    // read, and the graph it is about to free was already replaced by the
    // publish that preceded that read, so this load returns the new graph.
    // ------------------------------------------------------------------
    class GraphPin {
    public:
        explicit GraphPin(const Engine& e) : fEngine(e) {
            fEngine.fGraphUsers.fetch_add(1, std::memory_order_acquire);
        }
        ~GraphPin() {
            fEngine.fGraphUsers.fetch_sub(1, std::memory_order_release);
        }
        GraphPin(const GraphPin&) = delete;
        GraphPin& operator=(const GraphPin&) = delete;

        Graph* Active() const { return fEngine.fGraphs->Active(); }

    private:
        const Engine& fEngine;
    };

    // One queued rebuild: the model SNAPSHOT (taken on the caller's thread —
    // the window thread owns the model and may be editing it while the worker
    // builds), the position, and the transport intent.
    struct BuildRequest {
        Project      project;
        Frame        startFrame  = 0;
        Frame        minEndFrame = 0;
        LoadMode     mode        = LoadMode::NewPosition;
        bool         monitorOnly = false;
        // The track the piano roll's audition clicks render through (package
        // 51). Set from the Engine at request time, like monitorOnly: the
        // builder then keeps that track's bus alive even when its region
        // carries no notes, which is exactly when a click has to sound.
        TrackId      auditionTrack = kInvalidTrackId;
        // Build the graph without a loop even when the snapshot's transport has
        // one enabled. The record path sets it: a record pass rolls a long
        // transport through the loop end and the window re-aligns the engine at
        // the seam itself (see MainWindow's pulse), because the take layout is
        // measured from the engine's roll (M4.2 keeps that path as it is).
        bool         noLoop      = false;
        // The loop the graph plays under, resolved by the builder from the
        // snapshot and LoopActive(); see Graph::fLooping.
        Frame        blockFrames = 512;   // what the device renders per callback
        uint64_t     wrapsAtRequest = 0;  // Engine::fWraps when this was queued
        uint64_t     gen         = 0;
        // The device format the graph must be built for, captured with the
        // request (the player cannot be swapped out from under a build).
        float        outputRate  = 48000.0f;
        size_t       playerBufferBytes = 0;
    };

    // Builder thread: wait for a request, build a Graph from its snapshot,
    // publish it if it is still the newest one, and record the result.
    void BuilderLoop();
    void PublishCompletion(uint64_t gen, status_t rc);
    void PublishGraph(std::unique_ptr<Graph> g);
    void DetachGraph();

    // Build the whole graph from a snapshot. Builder thread only: it opens
    // files, spawns disk threads, allocates and primes rings. Returns nullptr
    // on failure (or on shutdown, mid-build) with *rc set.
    std::unique_ptr<Graph> BuildGraph(const BuildRequest& req, status_t* rc);

    // RT helper: copy the given chain's meters into the flat UI storage. The
    // spectrum write is bracketed by a seqlock generation bump (odd while
    // writing) so the UI reader can detect a torn copy and retry.
    template <class Chain>
    void CaptureFxMeters(const Chain& fx) {
        fMeterSpecGen.fetch_add(1, std::memory_order_acq_rel);   // -> odd
        int specFx = -1, specN = 0;
        for (int i = 0; i < (int)fx.size() && i < kMeterFxMax; i++) {
            if (!fx[i]) { fMeterGr[i].store(0.0f, std::memory_order_relaxed); continue; }
            fMeterGr[i].store(fx[i]->MeterDb(), std::memory_order_relaxed);
            if (specFx < 0) {
                const int n = fx[i]->Spectrum(fMeterSpec, kMeterSpecMax);
                if (n > 0) { specFx = i; specN = n; }
            }
        }
        fMeterSpecN.store(specN, std::memory_order_relaxed);
        fMeterSpecFx.store(specFx, std::memory_order_relaxed);
        fMeterSpecGen.fetch_add(1, std::memory_order_acq_rel);   // -> even
    }

    // RT helper: publish the values of every watch slot that addresses THIS
    // chain. Skipped per slot when nothing changed, so the generation the UI
    // compares is a CHANGE counter and a still insert costs it nothing.
    template <class Chain>
    void CaptureFxWatches(TrackId track, bool masterChain, const Chain& fx) {
        for (int s = 0; s < kWatchSlots; s++) {
            if (fWatchMaster[s].load(std::memory_order_relaxed) != masterChain)
                continue;
            if (!masterChain
                && fWatchTrack[s].load(std::memory_order_relaxed) != track)
                continue;
            const int idx = fWatchFx[s].load(std::memory_order_relaxed);
            if (idx < 0 || idx >= (int)fx.size() || !fx[(size_t)idx]) continue;
            float tmp[kWatchMax];
            const int n = fx[(size_t)idx]->ControlValues(tmp, kWatchMax);
            if (n <= 0) continue;
            if (n == fWatchN[s].load(std::memory_order_relaxed)) {
                bool same = true;
                for (int i = 0; i < n; i++)
                    if (tmp[i] != fWatchValues[s][i]) { same = false; break; }
                if (same) continue;
            }
            fWatchGen[s].fetch_add(1, std::memory_order_acq_rel);   // -> odd
            for (int i = 0; i < n; i++) fWatchValues[s][i] = tmp[i];
            fWatchN[s].store(n, std::memory_order_relaxed);
            fWatchGen[s].fetch_add(1, std::memory_order_acq_rel);   // -> even
        }
    }

    static void PlayTrampoline(void* cookie, void* buffer, size_t size,
                               const media_raw_audio_format& format);
    // The RT callback body: ONE acquire load of the active graph, then render
    // the whole block from it. No graph -> silence, and no playhead advance
    // (a rebuild that has not landed yet must not run the transport ahead of
    // the audio).
    void FillBuffer(float* out, size_t frames);

    // RT-only tempo push into the graph's effect chains.
    void ApplyTempo(Graph& g, double bpm);

    // ---- device ---------------------------------------------------------
    std::unique_ptr<BSoundPlayer> fPlayer;
    float  fOutputRate = 48000.0f;
    size_t fBufferFrames = 512;   // output buffer frames/channel (~10.7ms@48k)
    // What the live player was opened for, so EnsurePlayer can tell a rebuild
    // (keep it) from a format change (re-create it).
    double fPlayerRequestRate   = 0.0;
    size_t fPlayerRequestFrames = 0;
    size_t fPlayerBufferBytes   = 0;   // what the device actually negotiated
    std::atomic<uint64_t> fPlayersOpened{0};
    std::atomic<uint64_t> fPlayerStarts{0};
    // ---- what the callback costs (M3.1) ----------------------------------
    // Written by the RT thread around FillBuffer (one clock read per callback)
    // and by TrackStream::Mix on an underrun; read by the window's pulse.
    std::atomic<bigtime_t> fLastCallbackUs{0};
    std::atomic<bigtime_t> fPeakCallbackUs{0};   // max since TakePeakCallbackUs
    std::atomic<uint64_t>  fXruns{0};            // ring underruns (one per block)

    // ---- the swappable graph --------------------------------------------
    // Held by pointer so teardown can be explicit: the destructor stops the
    // player and joins the builder, THEN resets this, which drains the reclaim
    // thread while the engine it reads (fPlayerRunning, fCallbackGen) is still
    // alive.
    std::unique_ptr<GraphSwap<Graph>> fGraphs;

    // ---- transport + live state the swap must not touch ------------------
    std::atomic<Frame> fPlayhead{0};
    std::atomic<bool>  fPlaying{false};
    std::atomic<bool>  fFinished{false};
    std::atomic<float> fPeakL{0.0f};
    std::atomic<float> fPeakR{0.0f};
    // The M3.3 master meter: RMS per channel, the stereo correlation, and the
    // latched clip bits (RT writes, UI reads/clears). Engine-owned, so a graph
    // swap does not lose a clip the user has not acknowledged yet.
    std::atomic<float>    fRmsL{0.0f};
    std::atomic<float>    fRmsR{0.0f};
    std::atomic<float>    fCorr{0.0f};
    std::atomic<uint32_t> fClipFlags{0};
    std::atomic<float> fMasterGain{1.0f};
    std::atomic<bool>  fMetronomeOn{false};
    // What the click sounds like (M3.5): the level it has always had, and the
    // click timbre the Preferences window offers.
    std::atomic<float> fMetronomeGain{0.3f};
    std::atomic<int>   fMetronomeSound{0};
    std::atomic<bool>  fMonitorDim{false};
    std::atomic<bool>  fMonitorMono{false};
    std::atomic<IMonitorSource*> fMonSource{nullptr};
    std::atomic<bool>  fInputMonitor{false};
    std::atomic<IMidiInput*> fLiveMidi{nullptr};  // live-monitor input
    std::atomic<bool>  fMonitorOnly{false};       // idle monitor mode
    // The piano roll's audition (package 51): which track the clicks render
    // through, and the events themselves. The ring is UI-producer / RT-consumer
    // like the live input's, and small — a click is a note-on and a note-off,
    // and a burst is at most a fast arpeggio being typed.
    std::atomic<TrackId> fAuditionTrack{kInvalidTrackId};
    MidiEventRing      fAuditionRing{64};
    // Counted on the RT thread, one per audition note the instrument renders.
    std::atomic<uint64_t> fAuditionRendered{0};
    // Bumped once per completed RT callback; QuiesceMonitorInput() waits on it
    // to bound the monitor/live-MIDI source teardown UAF window, and the graph
    // reclaimer waits on it to know a retired graph is out of the callback.
    std::atomic<uint64_t> fCallbackGen{0};
    // Loop wraps performed by the transport, ever (M4.2). Bumped by the RT
    // callback at each wrap, read by the window (the record seam, the test
    // hooks) and by the in-place join to learn how many passes a build spanned.
    std::atomic<uint64_t> fWraps{0};
    // True only while the BSoundPlayer is actually running: set BEFORE
    // fPlayer->Start() and cleared AFTER fPlayer->Stop() returns (Stop blocks
    // until the last callback exits). Unlike fPlaying — which Stop() clears
    // before the player has drained — this is a sound "no callback in flight"
    // signal for QuiesceMonitorInput()'s fast path and for the reclaimer.
    std::atomic<bool>  fPlayerRunning{false};
    // The callback generation at the most recent graph publish. A graph retired
    // after it may be freed once two further boundaries have passed (the
    // callback that could still have loaded the old pointer has run to
    // completion) or immediately when the player is not running. See the
    // reclaim predicate in the Engine constructor.
    std::atomic<uint64_t> fRetireGen{0};
    // Off-RT users of the active graph (GraphPin). The reclaimer waits for this
    // to be zero as well. Mutable: the meter readers are const.
    mutable std::atomic<int> fGraphUsers{0};
    // A latency re-solve already asked for, per insert (M4.6): the identity
    // hash the graph stores for the insert and the observed latency the
    // rebuild was requested with. Touched only by FxLatencyMoved(), which the
    // UI pulse calls on one thread — no lock, and no RT access.
    struct LatencyAsked { uint64_t identity; int observed; };
    std::vector<LatencyAsked> fLatencyAsked;
    std::atomic<float> fLufsM{Loudness::kSilenceLufs};
    std::atomic<float> fLufsS{Loudness::kSilenceLufs};
    std::atomic<float> fTpDb{Loudness::kSilenceDb};
    Frame  fStartFrame = 0;   // playhead position playback begins at

    // ---- the builder -----------------------------------------------------
    std::mutex              fBuildMutex;
    std::condition_variable fBuildCv;
    std::unique_ptr<BuildRequest> fRequest;   // the newest queued request
    std::atomic<uint64_t>   fRequestGen{0};   // requests issued
    std::atomic<uint64_t>   fCompletedGen{0}; // requests whose build finished
    std::atomic<int>        fLoadStatus{(int)B_OK};
    std::atomic<uint64_t>   fGraphsPublished{0};
    std::atomic<bigtime_t>  fLastBuildUs{0};   // the last BuildGraph's duration
    std::atomic<bool>       fShutdown{false};
    std::thread             fBuilder;
    // Live-input routes, kept so a rebuild re-applies them. Guarded by
    // fRouteMutex: SetMidiRoutes runs on the window thread while the builder
    // reads them for a graph it is about to publish (neither is the RT thread).
    std::mutex fRouteMutex;
    std::vector<MidiInputRoute> fMidiRoutes;

    // ---- UI-facing publishing storage (engine-owned: an editor's watch must
    // survive every rebuild) ----------------------------------------------
    std::atomic<TrackId> fMeterTrack{kInvalidTrackId};
    std::atomic<float>   fMeterGr[kMeterFxMax];   // per-fx scalar meter (GR dB)
    static constexpr int kMeterSpecMax = 256;
    float                fMeterSpec[kMeterSpecMax] = {};
    std::atomic<int>     fMeterSpecN{0};
    std::atomic<int>     fMeterSpecFx{-1};
    std::atomic<uint32_t> fMeterSpecGen{0};   // seqlock generation (odd = writing)

    // Watched inserts (the open native editors' inserts) -- see SetFxWatch.
    // fWatchN < 0 means nothing has been published yet, so the first block
    // after a watch is set always publishes and the editor is never left
    // showing a value the engine does not have.
    // Zero-initialised, which is already "no watch": fWatchTrack 0 is
    // kInvalidTrackId (no bus has it) and fWatchMaster false (the master chain
    // is not scanned for it). SetFxWatch is what puts a real watch in a slot.
    std::atomic<TrackId>  fWatchTrack[kWatchSlots] {};
    std::atomic<bool>     fWatchMaster[kWatchSlots] {};
    std::atomic<int>      fWatchFx[kWatchSlots] {};
    float                 fWatchValues[kWatchSlots][kWatchMax] = {};
    std::atomic<int>      fWatchN[kWatchSlots] {};
    std::atomic<uint32_t> fWatchGen[kWatchSlots] {};   // seqlock (odd = writing)
};

} // namespace daw
