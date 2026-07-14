// IEffect — the common interface every DSP effect implements.
//
// One identical interface across all effect backends (built-in C++ now; a
// stable add-on ABI / LV2 later) so the engine's per-track effect chain never
// cares what produced an effect. Effects process interleaved-stereo float in
// place, the same buffer layout the mixer uses.
//
// Real-time contract: Process() runs on the audio path — no allocation, no
// locks, no I/O. Allocate in Prepare() (called off the RT thread before
// playback), and only do arithmetic on preallocated state in Process().
//
// Kit-free (STL only) so effects build and unit-test on any host.
#pragma once

namespace daw {

class IEffect {
public:
    virtual ~IEffect() = default;

    // Called before playback with the stream/output sample rate. Compute
    // coefficients and size internal buffers here (allocation allowed).
    virtual void Prepare(double sampleRate) = 0;

    // Process `frames` interleaved-stereo frames in place (2*frames floats).
    // RT-safe: arithmetic on preallocated state only.
    virtual void Process(float* stereo, int frames) = 0;

    // Clear internal state (filter memory, delay lines) without changing
    // parameters — e.g. on seek so stale audio doesn't bleed across a jump.
    virtual void Reset() = 0;

    // Set parameter `slot` to `value` (same slot order as the EffectDesc params
    // / editor knobs). Used for per-block parameter automation, so it must be
    // RT-safe: recompute coefficients only, no allocation. Slots that would need
    // a reallocation (e.g. resizing a delay line) may clamp/ignore. Default:
    // no-op (an effect opts in by overriding).
    virtual void SetParam(int /*slot*/, float /*value*/) {}

    virtual const char* Name() const = 0;
};

} // namespace daw
