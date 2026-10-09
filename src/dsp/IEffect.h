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

    // Inform the effect of the current tempo (BPM), for tempo-synced params
    // (e.g. a delay locked to note divisions). Called off the RT thread at Load
    // and whenever the tempo changes. Default: no-op.
    virtual void SetTempo(double /*bpm*/) {}

    // Processing latency this effect adds, in FRAMES (per-channel samples, NOT
    // interleaved floats) at the Prepare() rate: the number of frames by which
    // its output lags its input (a look-ahead limiter, a linear-phase FIR, an
    // FFT-block processor). This is the universal audio convention (LV2/VST
    // report latency in frames); the whole PDC path — Pdc::EdgeDelay,
    // FrameDelay::Prepare, the Exporter's pad/trim — is in frames end to end, so
    // no per-channel doubling is ever applied. Plugin-delay
    // compensation (Phase Y) reads this after Prepare() to delay-align sibling
    // signal paths so a latent effect on one track doesn't smear against a dry
    // sibling. Must be constant across a Prepare()/Process() lifetime (the graph
    // sizes its delay lines from it once). Default 0 = zero-latency (all the
    // built-in effects: their feedback/IIR state adds no reported delay). An
    // effect that reports N delays its output by exactly N frames.
    virtual int LatencySamples() const { return 0; }

    // Live metering for the effect editor (read off the RT thread; effects
    // update these in Process). Default: no meter.
    //   MeterDb  — a single scalar (e.g. a compressor's gain reduction, <= 0).
    //   Spectrum — up to `maxBins` magnitude values in dB (newest analysis);
    //              returns the count written, 0 if the effect has no spectrum.
    virtual float MeterDb() const { return 0.0f; }
    virtual int   Spectrum(float* /*magDb*/, int /*maxBins*/) const { return 0; }

    // Current values of this effect's parameters, in slot order, up to
    // `maxSlots`; returns how many were written. 0 (the default) means the
    // effect has no such notion -- a built-in whose parameters the model
    // already holds exactly.
    //
    // For a plugin this is the ONLY place its live parameter state exists:
    // automation drives SetParam from the audio thread and nothing tells the
    // model, so a native editor can learn what automation did only from here.
    // The engine's own audio thread is what reads it (see
    // Engine::WatchedFxParams); a UI must never call this on a playing
    // instance, which is the whole reason the engine publishes a copy.
    virtual int ControlValues(float* /*out*/, int /*maxSlots*/) const { return 0; }

    virtual const char* Name() const = 0;
};

} // namespace daw
