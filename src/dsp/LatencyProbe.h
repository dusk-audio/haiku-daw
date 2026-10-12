// LatencyProbe — settle an effect's reported latency before anything is sized
// from it (M4.6).
//
// An LV2 plugin publishes its latency on an output control port during run().
// The value is therefore only authoritative once the plugin HAS run: the 4K
// EQ 2 was measured reporting 0 at activate, 27 after its first run and 0
// again later, and every latency assertion the host could make straight after
// activation is about a number the plugin has not necessarily computed yet.
//
// So the host gives the effect a short silent pre-roll and then asks it to
// re-latch. Both hosts do it, through this one function, right after
// Prepare():
//
//   - the engine's graph build (Engine::BuildGraph), so the PDC solve and
//     every delay line it sizes use the value the plugin is working with, and
//     a re-solve triggered by a latency change converges on the first rebuild;
//   - the exporter, both where it measures a chain's total latency (which
//     drives the PDC solve and the pad) and where it sizes each insert's dry
//     delay line — the offline equivalent of "re-latch after the pre-roll".
//
// Kit-free, and off the RT thread by construction: it RUNS the effect, which
// is only legal before the instance is published to the audio thread.
#pragma once

#include "IEffect.h"

#include <algorithm>
#include <cstddef>

namespace daw {

// How much of a pre-roll a settling plugin gets: three blocks of 1024 frames
// of silence. Enough for a plugin that decides its latency on its first or
// second run (the fixture's `latency-step` moves on run 2; a plugin that
// engages an oversampler does so on the first full block), and short enough
// that a graph build or an export start does not care.
inline constexpr int kLatencyProbeBlocks = 3;
inline constexpr int kLatencyProbeFrames = 1024;

// Run `e` over the pre-roll and adopt whatever it reports afterwards. Returns
// the settled value (also what LatencySamples() now returns).
//
// A non-finite or negative reading is the implementation's problem, not this
// function's: IEffect::RelatchLatency's contract is to return a usable frame
// count, and Lv2Effect clamps and rejects exactly as it does at activation.
inline int SettleLatency(IEffect& e) {
    float buf[kLatencyProbeFrames * 2];
    for (int b = 0; b < kLatencyProbeBlocks; b++) {
        // Re-zeroed every block: an effect that writes its input buffer (or
        // leaves a tail in its output) must not feed itself the previous
        // block's leftovers and settle on a state the render never sees.
        std::fill(buf, buf + kLatencyProbeFrames * 2, 0.0f);
        e.Process(buf, kLatencyProbeFrames);
    }
    return e.RelatchLatency();
}

} // namespace daw
