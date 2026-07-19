#include "Exporter.h"

#include "WavSource.h"
#include "WavWriter.h"
#include "Resampler.h"
#include "FrameDelay.h"
#include "InsertSlot.h"
#include "../synth/IInstrument.h"
#include "../synth/InstrumentFactory.h"
#include "../dsp/EffectFactory.h"
#include "../dsp/IEffect.h"
#include "../dsp/Loudness.h"
#include "../dsp/Limiter.h"
#include "../model/RoutingGraph.h"
#include "../model/Pdc.h"
#include "../model/MidiControl.h"
#include "../model/Crossfade.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

namespace daw {

namespace {

// Flush-to-zero + denormals-are-zero for this render thread. Offline effect
// feedback tails (reverb/delay/EQ decays) otherwise hit denormals and stall the
// FPU into microcode — here it just slows the export, but keep parity with the
// RT engine which sets the same flags per callback.
inline void EnableDenormalFlush() {
#if defined(__x86_64__) || defined(__i386__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

// Equal-power pan, identical to Engine::EqualPowerGains: pan -1 = hard left,
// 0 = center (-3 dB each channel), +1 = hard right. Folds the track gain into
// the returned per-channel gains.
void EqualPowerGains(float gain, float pan, float* outL, float* outR) {
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    const float theta = (pan * 0.5f + 0.5f) * float(M_PI) * 0.5f;
    *outL = gain * std::cos(theta);
    *outR = gain * std::sin(theta);
}

// Convert a project-frame position to an output-frame position.
inline int64_t ToOut(Frame projFrame, double scale) {
    return static_cast<int64_t>(static_cast<double>(projFrame) * scale + 0.5);
}

// Decode one audio clip's source resampled to `outRate` into an interleaved-
// stereo buffer, stopping once `maxOutFrames` output frames are available (0 =
// no limit). Bounding the decode keeps a 2 s clip cut from a 1 h file from
// decoding — and buffering — the whole hour. Returns out.size()/2.
size_t DecodeClip(const Clip& c, double outRate, std::vector<float>& out,
                  int64_t maxOutFrames = 0) {
    out.clear();
    WavSource src;
    if (!src.Open(c.sourcePath))
        return 0;
    if (c.sourceOffset > 0)
        src.Seek(c.sourceOffset);

    Resampler rs(src.FrameRate(), outRate);
    const float* chunk = nullptr;
    size_t frames = 0;
    while (src.ReadChunk(&chunk, &frames)) {
        rs.Process(chunk, frames, out);
        if (maxOutFrames > 0 && (int64_t)(out.size() / 2) >= maxOutFrames)
            break;   // enough decoded to cover the clip window
    }
    return out.size() / 2;
}

// Place a decoded clip into a track buffer at its timeline position, applying
// linear fade-in/out (both measured in output frames) and per-channel gain.
void PlaceClip(const Clip& c, double scale, double outRate,
               int64_t totalOut, const float* gainLR,
               std::vector<float>& trackBuf,
               Frame effFadeIn, Frame effFadeOut) {
    const int64_t startOut  = ToOut(c.startFrame, scale);
    const int64_t lengthOut = ToOut(c.lengthFrames, scale);
    if (lengthOut <= 0)
        return;

    std::vector<float> decoded;
    // Only decode enough source to cover the clip window (+ a small resampler
    // slack) rather than the entire source file.
    const size_t decodedFrames = DecodeClip(c, outRate, decoded, lengthOut + 64);
    if (decodedFrames == 0)
        return;

    // How many frames actually play: bounded by clip length and decoded data.
    int64_t playFrames = lengthOut;
    if (playFrames > static_cast<int64_t>(decodedFrames))
        playFrames = static_cast<int64_t>(decodedFrames);

    const int64_t fadeIn  = ToOut(effFadeIn, scale);
    const int64_t fadeOut = ToOut(effFadeOut, scale);

    for (int64_t i = 0; i < playFrames; ++i) {
        const int64_t dst = startOut + i;
        if (dst < 0) continue;
        if (dst >= totalOut) break;

        float env = 1.0f;
        if (fadeIn > 0 && i < fadeIn)
            env *= static_cast<float>(i) / static_cast<float>(fadeIn);
        if (fadeOut > 0 && i >= lengthOut - fadeOut) {
            const int64_t rem = lengthOut - i;   // frames until clip end
            float f = static_cast<float>(rem) / static_cast<float>(fadeOut);
            if (f < 0.0f) f = 0.0f;
            if (f < env)  env = f;   // combine as min so overlapping fades behave
        }
        env *= c.gain;   // per-clip gain, after the [0,1] fade envelope

        trackBuf[dst * 2 + 0] += decoded[i * 2 + 0] * gainLR[0] * env;
        trackBuf[dst * 2 + 1] += decoded[i * 2 + 1] * gainLR[1] * env;
    }
}

} // namespace

bool ExportWav(const Project& project, const std::string& outPath,
               double outRate, int bitDepth, ExportNormalize norm) {
    EnableDenormalFlush();
    if (bitDepth != 16 && bitDepth != 24 && bitDepth != 32) bitDepth = 16;
    const double projRate = project.sampleRate;
    if (outRate <= 0.0)
        outRate = projRate;
    const double scale = outRate / projRate;

    // Solo overrides mute: if any track is soloed (and not muted), only those
    // are audible; otherwise every non-muted track is audible.
    bool anySolo = false;
    for (const Track& t : project.Tracks())
        if (t.soloed && !t.muted)
            anySolo = true;

    // Project end = the furthest clip/note end over audible tracks, in project
    // frames, then converted to output frames.
    Frame projEnd = 0;
    for (const Track& t : project.Tracks()) {
        const bool audible = !t.muted && (!anySolo || t.soloed || t.soloSafe);
        if (!audible)
            continue;
        for (const Clip& c : t.clips)
            if (c.startFrame + c.lengthFrames > projEnd)
                projEnd = c.startFrame + c.lengthFrames;
        for (const MidiNote& n : t.CollectNotes())   // absolute-timeline notes
            if (n.startFrame + n.lengthFrames > projEnd)
                projEnd = n.startFrame + n.lengthFrames;
    }

    const int64_t totalOut = ToOut(projEnd, scale);
    if (totalOut <= 0)
        return false;   // nothing to render


    const auto& tracks = project.Tracks();

    std::unordered_map<TrackId, size_t> idx;
    for (size_t i = 0; i < tracks.size(); i++)
        idx[tracks[i].id] = i;

    // Processing order: a node before every node it feeds — its output AND
    // every aux-send destination. Sends add extra edges, so use the general
    // edge topo (single-output ResolveRoutingOrder can't express them). The same
    // edge set drives the PDC latency solve below.
    std::vector<TrackId> nodeIds;
    std::vector<std::pair<TrackId, TrackId>> edges;
    for (const Track& t : tracks) {
        nodeIds.push_back(t.id);
        edges.push_back({t.id, t.output});
        for (const Send& s : t.sends)
            if (s.dest != kInvalidTrackId && s.dest != t.id)
                edges.push_back({t.id, s.dest});   // skip self-send edge
    }
    std::vector<TrackId> order;
    const bool routingOk = ResolveOrderWithEdges(nodeIds, edges, order);
    if (!routingOk)    // cycle / bad graph: fall back to flat (all to master)
        order = nodeIds;

    // Plugin delay compensation. Measure each node's own fx-chain latency (sum
    // of IEffect::LatencySamples after Prepare) and solve the graph so sibling
    // paths meeting at a common node/bus/master are delay-aligned. `pad` is the
    // total leading latency the render lags by (node graph + master fx): the mix
    // is rendered into buffers padded by it, per-edge delays realign siblings,
    // then the leading `pad` frames are trimmed so the bounce stays timeline-
    // aligned (a latent plugin is made transparent, not shifted). With no latent
    // effect anywhere pad == 0, every EdgeDelay is 0, and every path below is
    // byte-identical to an uncompensated render.
    // A BYPASSED insert still contributes its latency: bypass is soft (the host
    // routes the signal through a matching delay instead of processing it), so
    // the chain's reported latency — and therefore every PDC delay solved from
    // it — is constant whether or not an insert is bypassed. That is what makes
    // toggling bypass free of rebuilds and of timing shifts.
    auto chainLatency = [&](const std::vector<EffectDesc>& fxDescs) -> int {
        int lat = 0;
        for (const EffectDesc& d : fxDescs) {
            auto e = MakeEffect(d, outRate);
            if (e) { e->Prepare(outRate); lat += e->LatencySamples(); }
        }
        return lat;
    };
    std::vector<PdcNode> pnodes;
    pnodes.reserve(tracks.size());
    for (const Track& t : tracks)
        pnodes.push_back({t.id, chainLatency(t.fx)});
    PdcGraph pdc;
    const bool pdcOk = routingOk && ComputePdc(pnodes, edges, pdc);
    const int masterFxLat = chainLatency(project.masterFx);
    // The master chain's latency delays the mix whether or not the NODE graph
    // solved, so it always contributes to the pad that gets trimmed. Dropping
    // it on the fallback path (pad = 0) shifted the whole bounce late by the
    // master chain's latency and cut that much off the tail. Only the node-graph
    // term is conditional.
    const int64_t pad = (pdcOk ? (int64_t)pdc.totalLatency : 0) + masterFxLat;
    const int64_t totalOutPadded = totalOut + pad;

    // `nfloats` / `totalOut` are the LOGICAL output length (natural content and
    // the trimmed file); `*Padded` sizes the render buffers so delayed tails
    // fit. Edge sums shift a source's contribution later by EdgeDelay frames.
    const size_t nfloats       = static_cast<size_t>(totalOut) * 2;
    const size_t nfloatsPadded = static_cast<size_t>(totalOutPadded) * 2;
    std::vector<float> master(nfloatsPadded, 0.0f);

    // A node needs its own persistent full-length buffer only if it RECEIVES
    // signal from other nodes (it is some node's output target or a send
    // destination) — such a buffer must accumulate inputs across the topo
    // order. Leaf source tracks that only feed master/a bus never accumulate,
    // so they share ONE scratch buffer. This bounds memory to
    // (#destination nodes + master + scratch) full-length buffers instead of
    // one per track, which OOMs on large sessions.
    std::vector<bool> isDest(tracks.size(), false);
    for (const Track& t : tracks) {
        if (t.output != kRoutingMaster) {
            auto d = idx.find(t.output);
            if (d != idx.end()) isDest[d->second] = true;
        }
        for (const Send& s : t.sends) {
            if (s.dest == kInvalidTrackId || s.dest == t.id) continue;
            auto d = idx.find(s.dest);
            if (d != idx.end()) isDest[d->second] = true;
        }
    }
    std::vector<int> bufSlot(tracks.size(), -1);
    std::vector<std::vector<float>> persist;
    for (size_t i = 0; i < tracks.size(); i++)
        if (isDest[i]) {
            bufSlot[i] = (int)persist.size();
            persist.emplace_back(nfloatsPadded, 0.0f);
        }
    std::vector<float> scratch(nfloatsPadded, 0.0f);
    // The mix buffer (a std::vector&) for node index i: its persistent buffer if
    // it's a destination, else the shared leaf scratch.
    auto nodeVec = [&](size_t i) -> std::vector<float>& {
        return bufSlot[i] >= 0 ? persist[bufSlot[i]] : scratch;
    };

    // Sum `src`'s content into `dst`, shifted `delay` frames later (PDC). Both
    // buffers are nfloatsPadded long; content always fits (pad bounds the max
    // shift). delay == 0 is the plain accumulate; a negative delay is clamped to
    // 0 (belt-and-suspenders against an out-of-bounds write).
    auto sumDelayed = [&](float* dst, const float* src, float level,
                          int64_t delay) {
        if (delay < 0) delay = 0;
        const int64_t off = delay * 2;
        const int64_t lim = (int64_t)nfloatsPadded - off;
        for (int64_t i = 0; i < lim; ++i)
            dst[i + off] += src[i] * level;
    };

    // Add a node's aux sends (for the given fader phase) into their dest buses,
    // delay-aligned to the destination (PDC). A POST-fader send taps the node's
    // post-fx output (latency = node.outLat, i.e. EdgeDelay). A PRE-fader send
    // taps BEFORE the fader/fx (latency = node.inLat), so it must be delayed by
    // dest.inLat - node.inLat instead — otherwise a pre-fader send from a latent
    // node lands node.fxLatency samples early.
    auto addSends = [&](const Track& t, bool pre, const float* nb) {
        // A failed routing solve means the graph had a cycle and `order` fell
        // back to flat (everything straight to master). Aux sends are part of
        // that same edge set, so honouring them here would route signal along
        // edges the fallback explicitly abandoned — and in an order that is no
        // longer topological, so a send could land in a bus already mixed.
        if (!routingOk) return;
        for (const Send& s : t.sends) {
            if (s.preFader != pre || s.dest == kInvalidTrackId
                || s.dest == t.id) continue;   // ignore a self-send
            auto d = idx.find(s.dest);
            if (d == idx.end()) continue;
            float* db = nodeVec(d->second).data();   // dest is always persistent
            // Only consult the PDC graph when it actually solved; an unsolved
            // one has no node entries, so every lookup would silently read 0.
            const int64_t delay = !pdcOk ? 0
                : (pre ? (int64_t)pdc.InLat(s.dest) - pdc.InLat(t.id)   // pre-fader
                       : (int64_t)pdc.EdgeDelay(t.id, s.dest));         // post-fader
            sumDelayed(db, nb, s.level, delay);
        }
    };

    // Instantiate + run an effect chain over a whole node buffer, in blocks.
    // `inLat` is the node's PDC input latency: its content sits at buffer offset
    // inLat, so the chain runs over the full padded buffer (priming the leading
    // inLat frames + flushing the plugin's own latency tail) and fx-param
    // automation maps buffer offset -> timeline frame as (off - inLat)/scale.
    auto applyFx = [&](const std::vector<EffectDesc>& fxDescs, float* buf,
                       const std::vector<FxAutoLane>& fxAuto, int64_t inLat) {
        // Chain kept index-aligned with fxDescs (nullptr for any skipped) so
        // effect-parameter automation can address chain[fxIndex].
        std::vector<std::unique_ptr<IEffect>> chain;
        bool any = false;
        for (const EffectDesc& d : fxDescs) {
            auto e = MakeEffect(d, outRate);
            if (e) { e->Prepare(outRate); any = true; }
            chain.push_back(std::move(e));
        }
        if (!any) return;

        const int64_t kBlock = 8192;

        // Insert-slot state, index-aligned with `chain` (see EffectDesc).
        // The dry-path delay line is sized from each effect's OWN latency:
        // RunInsertSlot uses it both to preserve a bypassed insert's real delay
        // and to keep the wet/dry legs phase-aligned. Length 0 for a
        // zero-latency effect, which is every built-in but the look-ahead
        // limiter, so both paths stay free in the common case.
        std::vector<FrameDelay> dryDelay(chain.size());
        std::vector<float>      mixes(chain.size(), 1.0f);
        std::vector<char>       bypassed(chain.size(), 0);
        for (size_t i = 0; i < chain.size(); i++) {
            if (!chain[i]) continue;   // unavailable plugin: nothing to run
            const int lat = chain[i]->LatencySamples();
            mixes[i]    = ClampFxMix(fxDescs[i].mix);
            bypassed[i] = fxDescs[i].bypassed ? 1 : 0;
            dryDelay[i].Prepare(lat > 0 ? (size_t)lat : 0);
        }
        // One block of dry scratch, reused across blocks and inserts.
        std::vector<float> dryBlock((size_t)kBlock * 2, 0.0f);

        for (int64_t off = 0; off < totalOutPadded; off += kBlock) {
            int64_t n = totalOutPadded - off;
            if (n > kBlock) n = kBlock;
            const Frame pf = off >= inLat ? (Frame)((off - inLat) / scale) : 0;
            for (const FxAutoLane& fa : fxAuto) {
                if (fa.fxIndex < 0 || fa.fxIndex >= (int)chain.size()) continue;
                if (!chain[fa.fxIndex] || fa.lane.Count() == 0) continue;
                chain[fa.fxIndex]->SetParam(fa.slot, fa.lane.ValueAt(pf, 0.0f));
            }
            float* p = buf + off * 2;
            // The identical call the RT engine makes per block, so the bounce
            // cannot drift from playback.
            for (size_t i = 0; i < chain.size(); i++)
                RunInsertSlot(chain[i].get(), dryDelay[i], bypassed[i] != 0,
                              mixes[i], p, (size_t)n, dryBlock.data());
        }
    };

    for (TrackId id : order) {
        auto it = idx.find(id);
        if (it == idx.end()) continue;
        const Track& t = tracks[it->second];
        const bool audible = !t.muted && (!anySolo || t.soloed || t.soloSafe);
        const bool isDestNode = bufSlot[it->second] >= 0;
        // A MUTE silences a node and everything routed through it. But a bus /
        // destination that is merely solo-EXCLUDED must still pass the upstream
        // it already accumulated (which may include a soloed source) downstream
        // — otherwise every stem of a bus-routed track renders silent. Only a
        // solo-excluded LEAF (no accumulated inputs) contributes nothing.
        if (t.muted)
            continue;
        if (!audible && !isDestNode)
            continue;

        // Leaf nodes reuse the shared scratch, so clear it before building this
        // node. A destination node keeps whatever upstream inputs already
        // accumulated in its persistent buffer (do NOT clear it).
        if (bufSlot[it->second] < 0)
            std::fill(scratch.begin(), scratch.end(), 0.0f);
        float* nb = nodeVec(it->second).data();
        const float kUnity[2] = { 1.0f, 1.0f };

        // Build the node's OWN material only when audible; a solo-excluded dest
        // still runs (below) to pass its accumulated upstream through its fader.
        // Build the node DRY (no pan / gain yet), so a pre-fader send taps the
        // raw signal and the gain+pan fader can be a time-varying envelope.
        if (audible && t.type == TrackType::Audio) {
            const std::vector<ClipFades> fades = ComputeCrossfades(t.clips);
            for (size_t ci = 0; ci < t.clips.size(); ci++) {
                const Clip& c = t.clips[ci];
                if (c.sourcePath.empty()) continue;
                if (c.takeGroup > 0 && !c.takeActive) continue;  // inactive take
                PlaceClip(c, scale, outRate, totalOut, kUnity,
                          nodeVec(it->second), fades[ci].fadeIn, fades[ci].fadeOut);
            }
        } else if (audible && t.type == TrackType::Midi) {
            // One voice per track, built at the OUTPUT rate. The sampler is
            // stateless like the synth, so a bounce reproduces live playback
            // block for block even though the block size differs.
            std::unique_ptr<IInstrument> inst = MakeInstrument(t.instrument, outRate);
            std::vector<MidiNote> notes = t.CollectNotes();
            if (scale != 1.0)
                for (MidiNote& n : notes) {
                    n.startFrame   = ToOut(n.startFrame, scale);
                    n.lengthFrames = ToOut(n.lengthFrames, scale);
                }
            // Render in blocks so the CC7 (volume) x CC11 (expression) channel
            // gain and the CC10 pan are re-evaluated as they step. Events stay in
            // project frames;
            // the block start (output frames) maps back via 1/scale. A small
            // block (~10 ms) keeps CC resolution close to the live engine's
            // per-buffer granularity, so a bounce steps like playback.
            const std::vector<MidiClipEvent> events = t.CollectEvents();
            const int64_t kBlk = 512;
            // Previous block's end gains: each block ramps from them to its own
            // target so a stepped controller glides, matching the live engine.
            // Negative = first block, which snaps instead of sweeping from unity.
            float lastL = -1.0f, lastR = -1.0f;
            for (int64_t off = 0; off < totalOut; off += kBlk) {
                const int64_t nn = std::min<int64_t>(kBlk, totalOut - off);
                const Frame pf = (Frame)(off / scale);   // project frame @ block
                float cgl, cgr;
                MidiChannelGains(events, pf, &cgl, &cgr);
                const StereoGain to{cgl, cgr};
                const StereoGain from = (lastL < 0.0f) ? to
                                                       : StereoGain{lastL, lastR};
                inst->Render(notes, nb + off * 2, (size_t)nn, off, from, to);
                lastL = cgl; lastR = cgr;
            }
        }   // Bus: nb already holds the summed upstream (dry).

        // PDC: this node's own material (sources) is at buffer offset 0, but a
        // dest/bus's accumulated inputs already sit at its input latency, so the
        // fader/fx run in buffer-position space offset by inLat. Sources are
        // leaves with inLat == 0, so they are unaffected.
        const int64_t nodeIn = pdcOk ? pdc.InLat(t.id) : 0;

        addSends(t, /*pre=*/true, nb);          // pre-fader taps (dry)

        // Fader = gain * equal-power pan. Automated per sample when a lane has
        // points; otherwise a single constant multiply (fast path). A constant
        // gain is position-independent, so it spans the whole padded buffer; an
        // automation lane maps buffer offset i -> timeline (i - inLat)/scale.
        const bool automated = t.gainAuto.Count() > 0 || t.panAuto.Count() > 0;
        if (!automated) {
            float gLR[2];
            EqualPowerGains(t.gain, t.pan, &gLR[0], &gLR[1]);
            for (int64_t i = 0; i < totalOutPadded; ++i) {
                nb[i * 2 + 0] *= gLR[0];
                nb[i * 2 + 1] *= gLR[1];
            }
        } else {
            for (int64_t i = 0; i < totalOutPadded; ++i) {
                const Frame pf = i >= nodeIn
                    ? static_cast<Frame>((i - nodeIn) / scale) : 0;
                const float g = t.gainAuto.ValueAt(pf, t.gain);
                const float p = t.panAuto.ValueAt(pf, t.pan);
                float gLR[2];
                EqualPowerGains(g, p, &gLR[0], &gLR[1]);
                nb[i * 2 + 0] *= gLR[0];
                nb[i * 2 + 1] *= gLR[1];
            }
        }

        applyFx(t.fx, nb, t.fxAuto, nodeIn);
        addSends(t, /*pre=*/false, nb);         // post-fader taps

        // Route this node into its output (a bus) or the master mix, delay-
        // aligned to that destination (PDC). EdgeDelay is 0 when nothing is
        // latent, so this is the plain accumulate in the common case.
        float* dst = master.data();
        TrackId destId = kRoutingMaster;
        if (routingOk && t.output != kRoutingMaster) {
            auto d = idx.find(t.output);
            if (d != idx.end()) { dst = nodeVec(d->second).data(); destId = t.output; }
        }
        sumDelayed(dst, nb, 1.0f, pdcOk ? pdc.EdgeDelay(t.id, destId) : 0);
    }

    // Master bus FX chain (applied to the summed mix before master gain). Runs
    // through the same applyFx as a track chain so per-insert bypass / wet-dry
    // behave identically on the master. It has no fx automation, and its content
    // needs no input-latency offset (the mix already sits at buffer offset 0),
    // hence the empty lane list and inLat 0.
    applyFx(project.masterFx, master.data(), {}, 0);

    // PDC trim: the whole mix lags the timeline by `pad` frames (node graph +
    // master fx). Drop that leading latency so the bounce is timeline-aligned —
    // a latent plugin becomes transparent, not a shift. `outp` is the logical
    // (trimmed) master; everything below writes/measures exactly `nfloats`.
    float* outp = master.data() + static_cast<size_t>(pad) * 2;

    // Apply master gain, then hand the float mix to the writer, which quantizes
    // to the chosen depth (TPDF-dithered for 16-bit; 24/32 have ample headroom).
    const float mg = project.masterGain;
    if (mg != 1.0f)
        for (size_t i = 0; i < nfloats; ++i) outp[i] *= mg;

    // Final safety sweep: never write a non-finite sample. A NaN/Inf from an
    // unstable effect or a NaN-bearing float source would otherwise poison the
    // exported master (and anything downstream reads/meters from it). Parity
    // with the RT engine's pre-DAC guard (P0 #1); the offline path lacked it.
    for (size_t i = 0; i < nfloats; ++i)
        if (!std::isfinite(outp[i])) outp[i] = 0.0f;

    // Loudness normalization (offline): measure the finished master's integrated
    // loudness + true peak, then apply one gain that brings it to the target
    // LUFS. Without a limiter, the gain is backed off so the true peak never
    // crosses the ceiling; with a limiter (below) the full target gain is
    // applied and the limiter holds the ceiling instead.
    if (norm.enabled) {
        Loudness meter;
        meter.Prepare(outRate);
        meter.SetIntegratedEnabled(true);   // whole-program gated loudness
        const int64_t kBlk = 8192;
        for (int64_t off = 0; off < totalOut; off += kBlk) {
            const int64_t n = std::min(kBlk, totalOut - off);
            meter.Process(outp + off * 2, static_cast<int>(n));
        }
        const float lufs = meter.IntegratedLufs();
        const float tp   = meter.TruePeakDb();
        // Only normalize measurable program (silence stays silent).
        if (lufs > Loudness::kSilenceLufs + 1.0f) {
            float gainDb = norm.targetLufs - lufs;
            if (!norm.limiter && tp + gainDb > norm.truePeakCeil)
                gainDb = norm.truePeakCeil - tp;   // gain-backoff true-peak safety
            const float g = std::pow(10.0f, gainDb / 20.0f);
            if (std::isfinite(g) && g > 0.0f)
                for (size_t i = 0; i < nfloats; ++i) outp[i] *= g;
        }
    }

    // Look-ahead true-peak limiter (offline): hold the dBTP ceiling by limiting
    // peaks rather than attenuating the whole program. Runs last, after any
    // normalization gain, so its guarantee is on the final master.
    if (norm.limiter) {
        Limiter lim(norm.truePeakCeil, /*attackMs=*/2.0f, /*releaseMs=*/60.0f);
        lim.Process(outp, static_cast<size_t>(totalOut), outRate);
    }

    WavWriter writer;
    const bool floatOut = (bitDepth == 32);
    if (!writer.OpenFormat(outPath, static_cast<int>(outRate + 0.5), 2,
                           bitDepth, floatOut))
        return false;
    if (!writer.WriteFloat(outp, nfloats, /*dither=*/bitDepth == 16))
        return false;
    return writer.Close();
}

// Replace path-hostile characters so a track name is a safe filename.
static std::string SanitizeName(const std::string& n) {
    std::string s = n;
    for (char& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '"') c = '_';
    if (s.empty()) s = "track";
    return s;
}

int ExportStems(const Project& project, const std::string& dir, double outRate) {
    int written = 0, idx = 0;
    for (const Track& t : project.Tracks()) {
        idx++;
        if (t.type == TrackType::Bus) continue;   // stems are source tracks
        // Solo this track so ExportWav renders only it (through its own fader /
        // fx / bus / master). Solo overrides mute in the mix.
        Project copy = project;
        // Isolate exactly this track: solo it, and clear soloSafe on everyone
        // (a soloSafe track would otherwise bleed into every stem).
        for (Track& ct : copy.Tracks()) {
            ct.soloed   = (ct.id == t.id);
            ct.soloSafe = false;
        }
        if (Track* ct = copy.FindTrack(t.id)) ct->muted = false;
        char pre[8];
        std::snprintf(pre, sizeof(pre), "%02d_", idx);
        const std::string path = dir + "/" + pre + SanitizeName(t.name) + ".wav";
        if (ExportWav(copy, path, outRate)) written++;
    }
    return written;
}

} // namespace daw
