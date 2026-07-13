#include "Exporter.h"

#include "WavSource.h"
#include "WavWriter.h"
#include "Resampler.h"
#include "../synth/Synth.h"
#include "../dsp/EffectFactory.h"
#include "../dsp/IEffect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
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
    std::vector<float> trackBuf(nfloats, 0.0f);

    Synth synth(outRate);

    for (const Track& t : project.Tracks()) {
        const bool audible = !t.muted && (!anySolo || t.soloed);
        if (!audible)
            continue;

        std::fill(trackBuf.begin(), trackBuf.end(), 0.0f);

        float gainLR[2];
        EqualPowerGains(t.gain, t.pan, &gainLR[0], &gainLR[1]);

        if (t.type == TrackType::Audio) {
            for (const Clip& c : t.clips) {
                if (c.sourcePath.empty())
                    continue;
                PlaceClip(c, scale, outRate, totalOut, gainLR, trackBuf);
            }
        } else {   // Midi
            // Synth timing is in output frames; rescale note positions when the
            // export rate differs from the project rate.
            std::vector<MidiNote> notes = t.notes;
            if (scale != 1.0) {
                for (MidiNote& n : notes) {
                    n.startFrame   = ToOut(n.startFrame, scale);
                    n.lengthFrames = ToOut(n.lengthFrames, scale);
                }
            }
            // Render dry (unity), then apply the same equal-power gain/pan the
            // audio path uses — always, so a centered MIDI track gets the same
            // -3 dB center attenuation as a centered audio track.
            synth.Render(notes, trackBuf.data(),
                         static_cast<size_t>(totalOut), 0, 1.0f);
            for (int64_t i = 0; i < totalOut; ++i) {
                trackBuf[i * 2 + 0] *= gainLR[0];
                trackBuf[i * 2 + 1] *= gainLR[1];
            }
        }

        // Per-track effect chain, prepared at the output rate. Process in
        // blocks so the frame count fits an int and to bound working set.
        std::vector<std::unique_ptr<IEffect>> chain;
        for (const EffectDesc& d : t.fx) {
            auto fx = MakeEffect(d);
            if (!fx) continue;
            fx->Prepare(outRate);
            chain.push_back(std::move(fx));
        }
        if (!chain.empty()) {
            const int64_t kBlock = 8192;
            for (int64_t off = 0; off < totalOut; off += kBlock) {
                int64_t n = totalOut - off;
                if (n > kBlock) n = kBlock;
                float* p = trackBuf.data() + off * 2;
                for (auto& fx : chain)
                    fx->Process(p, static_cast<int>(n));
            }
        }

        for (size_t i = 0; i < nfloats; ++i)
            master[i] += trackBuf[i];
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
