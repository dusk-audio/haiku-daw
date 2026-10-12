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

#include <string>

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

    // Hand this block's EXTERNAL sidechain key to the effect: interleaved
    // stereo, `frames` frames, exactly the block the next Process() call will
    // receive. The host calls it immediately before Process(), on the audio
    // thread, for every insert whose EffectDesc names a sidechain source; an
    // insert with no source is never called at all.
    //
    // Contract, all three parts forced by the RT context:
    //   - the pointer is valid ONLY during the following Process() call — the
    //     host re-supplies it every block, and the effect must not retain it
    //     past that call (see dsp/SidechainKey.h for the storage that does
    //     this correctly);
    //   - the host may pass nullptr (or never call this) whenever no key is
    //     routed this block: a cycle in the routing graph, a source that no
    //     longer exists, an insert the host chose not to key. An effect whose
    //     keyed path is enabled must then behave exactly as it does with
    //     keying off — internal detection, never silence and never a stale
    //     key. Failing soft is the contract, because the alternative is a
    //     mute button becoming a mute on some other track;
    //   - the effect must not keep a pointer to the caller's buffer.
    //
    // Default: no-op, which keeps every effect that was written before this
    // existed source- and behavior-compatible (they simply never detect on a
    // key).
    virtual void SetSidechain(const float* /*stereo*/, int /*frames*/) {}

    // Processing latency this effect adds, in FRAMES (per-channel samples, NOT
    // interleaved floats) at the Prepare() rate: the number of frames by which
    // its output lags its input (a look-ahead limiter, a linear-phase FIR, an
    // FFT-block processor). This is the universal audio convention (LV2/VST
    // report latency in frames); the whole PDC path — Pdc::EdgeDelay,
    // FrameDelay::Prepare, the Exporter's pad/trim — is in frames end to end, so
    // no per-channel doubling is ever applied. Plugin-delay
    // compensation (Phase Y) reads this after Prepare() to delay-align sibling
    // signal paths so a latent effect on one track doesn't smear against a dry
    // sibling. Default 0 = zero-latency (all the built-in effects: their
    // feedback/IIR state adds no reported delay). An effect that reports N
    // delays its output by exactly N frames.
    //
    // This is the value the HOST HAS COMMITTED TO: every delay line sized from
    // it (PDC edges, RunInsertSlot's dry delay, the sidechain key routes) was
    // built once and cannot be resized under the running graph. It must
    // therefore stay constant for as long as that graph lives, and Process()
    // must never change it. A latency that MOVES is reported through
    // ObservedLatencySamples() and adopted through RelatchLatency(), both off
    // the RT thread, and only into the NEXT graph (M4.6 — see the two calls
    // below, and 02-lv2-host-PR.md for why).
    virtual int LatencySamples() const { return 0; }

    // The latency this effect would report NOW, which can differ from
    // LatencySamples() after it has been run: an LV2 plugin is allowed to move
    // its latency port at any time (4K EQ 2 was measured going 0 -> 27 -> 0 as
    // its oversampler engages), and the host has to be able to notice without
    // touching the plugin from a foreign thread.
    //
    // Called OFF the RT thread (the engine's latency poll, from the UI pulse).
    // An implementation that reads a plugin's port must return host-side state
    // the RT thread has already PUBLISHED, never read the plugin itself.
    //
    // The default returns LatencySamples(): an effect whose latency cannot move
    // (every built-in, and any plugin that keeps the port constant) reports no
    // change, so the poll stays free.
    virtual int ObservedLatencySamples() const { return LatencySamples(); }

    // Adopt the latency this effect currently OBSERVES as the committed one
    // (what LatencySamples() will now return), and return it. The host calls it
    // off the RT thread, after giving the effect a short pre-roll —
    // dsp/LatencyProbe.h's SettleLatency() — and BEFORE it sizes anything from
    // the value (the engine's graph build, the exporter's PDC solve).
    //
    // The pre-roll is the point: a plugin's latency port is only authoritative
    // once the plugin has run, so latching straight after activation is how v1
    // reported 0 for a plugin actually running at 27.
    //
    // Default: return LatencySamples() unchanged — an effect that cannot move
    // is already committed to the right value.
    virtual int RelatchLatency() { return LatencySamples(); }

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

    // The effect's OWN state — what its control ports do not describe (a synth's
    // patch, an amp sim's cabinet). Serialize it into an opaque string the host
    // stores in EffectDesc.state and hands back to LoadState, or return false
    // for "this effect has no such notion" (every built-in, and a plugin with no
    // state:interface).
    //
    // Off the RT thread by construction: both calls may allocate, and a caller
    // must never make them from the audio path. SaveState may run while
    // Process() is running (it is called from the UI thread while the transport
    // rolls) — LV2's state save threading class allows exactly that, which is
    // what keeps a save from stopping the audio.
    virtual bool SaveState(std::string* /*out*/) const { return false; }

    // Restore a string SaveState produced. Called off the RT thread BEFORE the
    // effect is first Process()ed (LV2's instantiation threading class forbids
    // doing it on a running instance); a failure leaves the effect at its
    // defaults, which is the same thing an unreadable project does for params.
    virtual bool LoadState(const std::string& /*state*/) { return false; }

    virtual const char* Name() const = 0;
};

} // namespace daw
