// FrameDelay — an integer-sample stereo delay line for plugin-delay
// compensation in the RT engine.
//
// A PDC edge delays one signal path by a fixed number of samples so it lines up
// with a slower sibling path (see model/Pdc.h). Because the engine mixes block
// by block, the delay must carry state ACROSS blocks — this holds that state in
// a small ring. Prepare() (off the RT thread) sizes the ring; ProcessAdd() is
// RT-safe (only arithmetic on the preallocated ring, no alloc/locks/I/O). A
// delay of 0 keeps no ring and is a plain accumulate, so an uncompensated graph
// (every built-in effect reports zero latency today) pays nothing.
//
// Kit-free, header-only, so it host-unit-tests anywhere.
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace daw {

struct FrameDelay {
    std::vector<float> buf;   // ring of `d` interleaved-stereo frames (2*d floats)
    std::size_t        d = 0; // delay in frames
    std::size_t        w = 0; // ring write index (frames)

    // Size the ring to `delayFrames` frames of delay (off the RT thread).
    void Prepare(std::size_t delayFrames) {
        d = delayFrames;
        buf.assign(d * 2, 0.0f);
        w = 0;
    }

    // Clear the held tail without resizing (e.g. on seek, so stale audio doesn't
    // bleed across a jump).
    void Reset() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        w = 0;
    }

    // Add `in` (interleaved stereo, `frames` frames) delayed by `d` frames into
    // `dst`, scaled by `level`. RT-safe. A read-then-write ring of length d
    // yields exactly d samples of delay; the tail carries into the next block.
    void ProcessAdd(const float* in, float* dst, std::size_t frames, float level) {
        if (d == 0) {                                  // no delay: plain accumulate
            const std::size_t n = frames * 2;
            for (std::size_t i = 0; i < n; ++i) dst[i] += in[i] * level;
            return;
        }
        for (std::size_t i = 0; i < frames; ++i) {
            const float ol = buf[w * 2 + 0], orr = buf[w * 2 + 1];
            buf[w * 2 + 0] = in[i * 2 + 0];
            buf[w * 2 + 1] = in[i * 2 + 1];
            dst[i * 2 + 0] += ol * level;
            dst[i * 2 + 1] += orr * level;
            if (++w >= d) w = 0;
        }
    }
};

} // namespace daw
