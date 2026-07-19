// RunInsertSlot — run ONE effect-chain insert over a block, honouring that
// insert's bypass and wet/dry mix (see EffectDesc in model/Effect.h).
//
// This is the single definition of what an insert slot DOES. Both hosts call
// it: the RT engine's per-block chain loop (Engine::FillBuffer) and the offline
// Exporter's applyFx. That matters more than the usual don't-repeat-yourself
// argument — the bounce is required to be sample-identical to live playback, and
// two hand-maintained copies of this logic can only converge by luck. Sharing
// the function makes that identity structural instead of a claim.
//
// Real-time contract: RT-safe. Arithmetic and memcpy over caller-preallocated
// state only — no allocation, no locks, no I/O. The caller owns the delay line
// and the scratch buffer and sizes both off the RT thread.
//
// Kit-free (STL only), so it host-unit-tests anywhere (see tests/insertslot_tests).
#pragma once

#include "FrameDelay.h"
#include "../dsp/IEffect.h"

#include <cstddef>
#include <cstring>

namespace daw {

// Run insert `e` over `buf` (interleaved stereo, `frames` frames), in place.
//
//   dryDelay — this insert's dry-path delay line, sized by the caller to the
//              effect's own LatencySamples() (length 0 for a zero-latency
//              effect, which is every built-in but the look-ahead limiter).
//   bypassed — soft bypass; see below.
//   mix      — wet/dry blend, clamped by the caller to [0,1] (ClampFxMix).
//   dry      — scratch of at least frames*2 floats, preallocated by the caller.
//
// A null `e` (an effect that failed to build, e.g. an unavailable plugin) is a
// no-op, leaving the chain index-aligned with the model's descriptor list.
inline void RunInsertSlot(IEffect* e, FrameDelay& dryDelay, bool bypassed,
                          float mix, float* buf, std::size_t frames,
                          float* dry) {
    if (!e) return;
    const std::size_t nf    = frames * 2;   // interleaved floats
    const std::size_t bytes = nf * sizeof(float);

    if (bypassed) {
        // SOFT bypass: the insert stops processing but keeps REPORTING its
        // LatencySamples(), so the PDC solve and every delay line sized from it
        // stay valid — no graph rebuild, no seam, no click. IEffect requires
        // that value to be constant across a Prepare()/Process() lifetime
        // anyway, so a hard bypass that changed it was never an option.
        //
        // Keeping the reported latency obliges us to keep the REAL one. Merely
        // skipping Process would drop N samples of actual delay while the graph
        // still compensates for N, landing this path N samples EARLY against
        // the rest of the mix — an audible flam on every toggle, precisely the
        // artifact soft bypass exists to prevent. So a latent insert passes its
        // signal through the delay line alone. N == 0 needs no line at all and
        // costs nothing.
        if (dryDelay.d == 0) return;
        std::memcpy(dry, buf, bytes);
        std::memset(buf, 0, bytes);
        dryDelay.ProcessAdd(dry, buf, frames, 1.0f);
        return;
    }

    if (mix >= 1.0f) {   // fully wet: the common path, a bare Process
        // A LATENT insert also clocks its delay line with the dry input. The
        // line contributes nothing here, but it must already hold the last N
        // frames if the user toggles bypass on — otherwise the bypass branch
        // above would read a stale tail and emit it as a burst. Push() writes
        // only the ring and never touches `buf`, so this cannot perturb a
        // sample: an offline host that skips it (its slot state is fixed for a
        // whole render, so no toggle can occur) still renders identically.
        if (dryDelay.d != 0) dryDelay.Push(buf, frames);
        e->Process(buf, (int)frames);
        return;
    }

    // out = dry*(1-mix) + wet*mix, with the dry leg delayed by the effect's own
    // latency so the two legs stay phase-aligned instead of comb-filtering.
    // (mix == 1 returned above, so the dry leg always contributes here.)
    std::memcpy(dry, buf, bytes);
    e->Process(buf, (int)frames);
    for (std::size_t k = 0; k < nf; ++k) buf[k] *= mix;
    dryDelay.ProcessAdd(dry, buf, frames, 1.0f - mix);
}

} // namespace daw
