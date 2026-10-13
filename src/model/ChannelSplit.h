// ChannelSplit — pure channel-addressing math for multichannel capture (M5).
//
// A capture is interleaved at the device's native channel count; each armed
// track's take is a mono or stereo file built from that one capture. Which
// device channel a track reads (its address) and the de-interleave itself live
// here, kit-free and header-only, so they can be tested on any host: the parts
// that cannot be tested here are the node enumeration and the Media Kit
// connect, and those are on the hardware click list (`docs/PLAN_1.0.md` M5.4).
//
// Header-only, STL only.
#pragma once

#include "Project.h"    // InputSource / TrackChannels
#include "types.h"

#include <cstddef>
#include <string>

namespace daw {

// The sample format a take is written in, a preference (AppSettings::
// takeFormat). 24-bit PCM is the default -- a take should keep what the
// interface handed over; 16-bit matches the pre-M5 takes; 32-bit float keeps a
// float device's samples exactly (no conversion at all).
enum class TakeFormat { Int16 = 0, Int24 = 1, Float32 = 2 };

// A setting value -> format. Anything unknown becomes the default: a settings
// file written by a newer build must not be able to leave the recorder in a
// format it cannot write.
inline TakeFormat TakeFormatFromInt(int v) {
    switch (v) {
        case 0:  return TakeFormat::Int16;
        case 2:  return TakeFormat::Float32;
        default: return TakeFormat::Int24;
    }
}

// The WavWriter arguments for a format (OpenFormat's bitsPerSample + floatFmt).
inline void TakeFormatBits(TakeFormat f, int* bits, bool* float32) {
    switch (f) {
        case TakeFormat::Int16:   *bits = 16; *float32 = false; break;
        case TakeFormat::Float32: *bits = 32; *float32 = true;  break;
        default:                  *bits = 24; *float32 = false; break;
    }
}

// One file of a capture: which device channels it takes and how it is stored.
// `path` is the caller's (NextFreeWavPaths picks it); the addressing is the
// track's InputSource resolved for the take's channel width.
struct TakeSpec {
    std::string path;
    bool stereo  = true;   // false: a mono take, from srcL only
    int  srcL    = 0;      // 0-based channel on the capture device
    int  srcR    = -1;     // right channel when stereo; <0 duplicates srcL
    int  bits    = 24;     // PCM width, or 32 with float32
    bool float32 = false;  // 32-bit IEEE float instead of PCM
};

inline int TakeChannels(const TakeSpec& s) { return s.stereo ? 2 : 1; }

// The take `track` wants: its address from the model, its width from
// Track::channels, its format from the preference. An address that was never
// set (a fresh track) is the device's first pair, which is what the pre-M5
// recorder captured.
inline TakeSpec PlanTake(const std::string& path, const InputSource& in,
                         TrackChannels width, TakeFormat fmt) {
    TakeSpec s;
    s.path   = path;
    s.stereo = (width == TrackChannels::Stereo);
    s.srcL   = in.channelL < 0 ? 0 : in.channelL;
    s.srcR   = s.stereo ? (in.channelR < 0 ? s.srcL : in.channelR) : -1;
    TakeFormatBits(fmt, &s.bits, &s.float32);
    return s;
}

// Clamp a spec's address to the channels the capture actually negotiated. An
// address that does not exist any more (a smaller interface than the one the
// project was saved with) falls back to the device's FIRST channel rather than
// reading outside the block or failing the whole take: the take still lands,
// on the channels the device has. `deviceChannels <= 0` (the format is not
// known yet) leaves the spec alone.
inline void ClampToDevice(TakeSpec* s, int deviceChannels) {
    if (s == nullptr || deviceChannels <= 0) return;
    const int last = deviceChannels - 1;
    if (s->srcL < 0 || s->srcL > last) s->srcL = 0;
    if (s->stereo && (s->srcR < 0 || s->srcR > last)) s->srcR = s->srcL;
}

// Pull one take's samples out of an interleaved capture block. `src` holds
// `frames` frames of `srcChannels` samples each; `dst` receives `frames`
// samples when mono, or `2 * frames` interleaved L/R when stereo. Returns the
// number of floats written.
//
// A channel index outside the block reads silence -- never past the block: a
// device that renegotiated its channel count mid-take must not make the disk
// thread read someone else's memory. A stereo address whose right channel is
// missing or out of range takes the LEFT one twice (a mono source fanned out,
// not a dead channel).
inline size_t Deinterleave(const float* src, size_t frames, int srcChannels,
                           int srcL, int srcR, bool stereo, float* dst) {
    if (src == nullptr || dst == nullptr || srcChannels <= 0) return 0;
    const bool hasL = (srcL >= 0 && srcL < srcChannels);

    if (!stereo) {
        for (size_t f = 0; f < frames; f++)
            dst[f] = hasL ? src[f * (size_t)srcChannels + (size_t)srcL] : 0.0f;
        return frames;
    }

    const bool hasR = (srcR >= 0 && srcR < srcChannels);
    for (size_t f = 0; f < frames; f++) {
        const float* base = src + f * (size_t)srcChannels;
        const float l = hasL ? base[srcL] : 0.0f;
        const float r = hasR ? base[srcR] : l;
        dst[f * 2 + 0] = l;
        dst[f * 2 + 1] = r;
    }
    return frames * 2;
}

} // namespace daw
