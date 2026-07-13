#include "Exporter.h"

#include "WavSource.h"
#include "WavWriter.h"
#include "Resampler.h"
#include "../synth/Synth.h"
#include "../dsp/EffectFactory.h"
#include "../dsp/IEffect.h"
#include "../model/RoutingGraph.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace daw {

namespace {

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

// Decode one audio clip's source in full, resampled to `outRate`, into an
// interleaved-stereo buffer. Returns the resampled frame count (out.size()/2).
size_t DecodeClip(const Clip& c, double outRate, std::vector<float>& out) {
    out.clear();
    WavSource src;
    if (!src.Open(c.sourcePath))
        return 0;
    if (c.sourceOffset > 0)
        src.Seek(c.sourceOffset);

    Resampler rs(src.FrameRate(), outRate);
    const float* chunk = nullptr;
    size_t frames = 0;
    while (src.ReadChunk(&chunk, &frames))
        rs.Process(chunk, frames, out);
    return out.size() / 2;
}

// Place a decoded clip into a track buffer at its timeline position, applying
// linear fade-in/out (both measured in output frames) and per-channel gain.
void PlaceClip(const Clip& c, double scale, double outRate,
               int64_t totalOut, const float* gainLR,
               std::vector<float>& trackBuf) {
    std::vector<float> decoded;
    const size_t decodedFrames = DecodeClip(c, outRate, decoded);
    if (decodedFrames == 0)
        return;

    const int64_t startOut  = ToOut(c.startFrame, scale);
    const int64_t lengthOut = ToOut(c.lengthFrames, scale);
    if (lengthOut <= 0)
        return;

    // How many frames actually play: bounded by clip length and decoded data.
    int64_t playFrames = lengthOut;
    if (playFrames > static_cast<int64_t>(decodedFrames))
        playFrames = static_cast<int64_t>(decodedFrames);

    const int64_t fadeIn  = ToOut(c.fadeInFrames, scale);
    const int64_t fadeOut = ToOut(c.fadeOutFrames, scale);

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
               double outRate) {
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
        const bool audible = !t.muted && (!anySolo || t.soloed);
        if (!audible)
            continue;
        for (const Clip& c : t.clips)
            if (c.startFrame + c.lengthFrames > projEnd)
                projEnd = c.startFrame + c.lengthFrames;
        for (const MidiNote& n : t.notes)
            if (n.startFrame + n.lengthFrames > projEnd)
                projEnd = n.startFrame + n.lengthFrames;
    }

    const int64_t totalOut = ToOut(projEnd, scale);
    if (totalOut <= 0)
        return false;   // nothing to render

    const size_t nfloats = static_cast<size_t>(totalOut) * 2;
    std::vector<float> master(nfloats, 0.0f);
    Synth synth(outRate);

    const auto& tracks = project.Tracks();

    // One mix buffer (node) per track; buses accumulate their inputs here.
    std::vector<std::vector<float>> nodeBuf(tracks.size(),
                                            std::vector<float>(nfloats, 0.0f));
    std::unordered_map<TrackId, size_t> idx;
    for (size_t i = 0; i < tracks.size(); i++)
        idx[tracks[i].id] = i;

    // Processing order: a node before every node it feeds — its output AND
    // every aux-send destination. Sends add extra edges, so use the general
    // edge topo (single-output ResolveRoutingOrder can't express them).
    std::vector<TrackId> nodeIds;
    std::vector<std::pair<TrackId, TrackId>> edges;
    for (const Track& t : tracks) {
        nodeIds.push_back(t.id);
        edges.push_back({t.id, t.output});
        for (const Send& s : t.sends)
            if (s.dest != kInvalidTrackId) edges.push_back({t.id, s.dest});
    }
    std::vector<TrackId> order;
    const bool routingOk = ResolveOrderWithEdges(nodeIds, edges, order);
    if (!routingOk)    // cycle / bad graph: fall back to flat (all to master)
        order = nodeIds;

    // Add a node's aux sends (for the given fader phase) into their dest buses.
    auto addSends = [&](const Track& t, bool pre, const float* nb) {
        for (const Send& s : t.sends) {
            if (s.preFader != pre || s.dest == kInvalidTrackId) continue;
            auto d = idx.find(s.dest);
            if (d == idx.end()) continue;
            float* db = nodeBuf[d->second].data();
            for (size_t i = 0; i < nfloats; ++i) db[i] += nb[i] * s.level;
        }
    };

    // Instantiate + run an effect chain over a whole node buffer, in blocks.
    auto applyFx = [&](const std::vector<EffectDesc>& fxDescs, float* buf) {
        std::vector<std::unique_ptr<IEffect>> chain;
        for (const EffectDesc& d : fxDescs) {
            auto e = MakeEffect(d);
            if (!e) continue;
            e->Prepare(outRate);
            chain.push_back(std::move(e));
        }
        if (chain.empty()) return;
        const int64_t kBlock = 8192;
        for (int64_t off = 0; off < totalOut; off += kBlock) {
            int64_t n = totalOut - off;
            if (n > kBlock) n = kBlock;
            float* p = buf + off * 2;
            for (auto& e : chain)
                e->Process(p, static_cast<int>(n));
        }
    };

    for (TrackId id : order) {
        auto it = idx.find(id);
        if (it == idx.end()) continue;
        const Track& t = tracks[it->second];
        const bool audible = !t.muted && (!anySolo || t.soloed);
        if (!audible)
            continue;   // muted/solo'd out: render + route nothing downstream

        float* nb = nodeBuf[it->second].data();
        const float kUnity[2] = { 1.0f, 1.0f };

        // Build the node DRY (no pan / gain yet), so a pre-fader send taps the
        // raw signal and the gain+pan fader can be a time-varying envelope.
        if (t.type == TrackType::Audio) {
            for (const Clip& c : t.clips) {
                if (c.sourcePath.empty()) continue;
                PlaceClip(c, scale, outRate, totalOut, kUnity, nodeBuf[it->second]);
            }
        } else if (t.type == TrackType::Midi) {
            std::vector<MidiNote> notes = t.notes;
            if (scale != 1.0)
                for (MidiNote& n : notes) {
                    n.startFrame   = ToOut(n.startFrame, scale);
                    n.lengthFrames = ToOut(n.lengthFrames, scale);
                }
            synth.Render(notes, nb, static_cast<size_t>(totalOut), 0, 1.0f);
        }   // Bus: nb already holds the summed upstream (dry).

        addSends(t, /*pre=*/true, nb);          // pre-fader taps (dry)

        // Fader = gain * equal-power pan. Automated per sample when a lane has
        // points; otherwise a single constant multiply (fast path).
        const bool automated = t.gainAuto.Count() > 0 || t.panAuto.Count() > 0;
        if (!automated) {
            float gLR[2];
            EqualPowerGains(t.gain, t.pan, &gLR[0], &gLR[1]);
            for (int64_t i = 0; i < totalOut; ++i) {
                nb[i * 2 + 0] *= gLR[0];
                nb[i * 2 + 1] *= gLR[1];
            }
        } else {
            for (int64_t i = 0; i < totalOut; ++i) {
                const Frame pf = static_cast<Frame>(i / scale);   // project frame
                const float g = t.gainAuto.ValueAt(pf, t.gain);
                const float p = t.panAuto.ValueAt(pf, t.pan);
                float gLR[2];
                EqualPowerGains(g, p, &gLR[0], &gLR[1]);
                nb[i * 2 + 0] *= gLR[0];
                nb[i * 2 + 1] *= gLR[1];
            }
        }

        applyFx(t.fx, nb);
        addSends(t, /*pre=*/false, nb);         // post-fader taps

        // Route this node into its output (a bus) or the master mix.
        float* dst = master.data();
        if (routingOk && t.output != kRoutingMaster) {
            auto d = idx.find(t.output);
            if (d != idx.end()) dst = nodeBuf[d->second].data();
        }
        for (size_t i = 0; i < nfloats; ++i)
            dst[i] += nb[i];
    }

    // Master bus FX chain (applied to the summed mix before master gain).
    {
        std::vector<std::unique_ptr<IEffect>> masterFx;
        for (const EffectDesc& d : project.masterFx) {
            auto fx = MakeEffect(d);
            if (!fx) continue;
            fx->Prepare(outRate);
            masterFx.push_back(std::move(fx));
        }
        if (!masterFx.empty()) {
            const int64_t kBlock = 8192;
            for (int64_t off = 0; off < totalOut; off += kBlock) {
                int64_t n = totalOut - off;
                if (n > kBlock) n = kBlock;
                float* pm = master.data() + off * 2;
                for (auto& fx : masterFx)
                    fx->Process(pm, static_cast<int>(n));
            }
        }
    }

    // Master gain, clamp to [-1,1], convert to interleaved int16.
    const float mg = project.masterGain;
    std::vector<int16_t> pcm(nfloats);
    for (size_t i = 0; i < nfloats; ++i) {
        float s = master[i] * mg;
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = static_cast<int16_t>(std::lround(s * 32767.0f));
    }

    WavWriter writer;
    if (!writer.Open(outPath, static_cast<int>(outRate + 0.5), 2))
        return false;
    if (!writer.WriteInt16(pcm.data(), pcm.size()))
        return false;
    return writer.Close();
}

} // namespace daw
