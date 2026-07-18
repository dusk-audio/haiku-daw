// LookaheadLimiter — a live, real-time look-ahead brickwall limiter IEffect.
//
// The FIRST built-in effect that reports a non-zero IEffect::LatencySamples():
// it delays its output by a fixed look-ahead window so the gain reduction for a
// peak is fully in place by the time that peak reaches the output. That reported
// latency is what makes it the on-target exercise for Phase Y plugin-delay
// compensation — dropped on one track it delays that path by La frames, and the
// PDC solver (model/Pdc.h) + the engine's per-edge FrameDelay / the offline
// Exporter's pad+trim delay-align every sibling path so the limited track stays
// time-aligned instead of smearing. Until this effect existed every built-in
// reported 0, so every delay line was length 0 and PDC was never exercised.
//
// Relationship to dsp/Limiter (the offline export limiter): that one runs over a
// finished buffer and realizes its look-ahead as an anticipatory backward pass
// with ZERO reported latency (no PDC needed on the master sink). This one is the
// streaming/live counterpart — it CANNOT see the future, so it carries a real
// La-frame delay line and reports it. It detects the SAMPLE peak (not the 4x-
// oversampled true peak the offline limiter uses); an RT true-peak FIR would add
// its own look-ahead and muddy the exact latency contract, so the live insert
// stays sample-domain. The guarantee is therefore exact in the sample domain:
// for every output frame the applied gain g <= target/peak, so |out| <= ceiling.
//
// Method (per input frame i, output frame emitted is input i-La):
//   1. peak_i = max(|L|,|R|) of the input-gained frame.
//   2. windowMax = max peak over the look-ahead window [i-La, i] (an O(1)
//      monotonic-decreasing deque over a preallocated ring — no alloc).
//   3. gTarget = min(1, target/windowMax). Since the emitted frame's own peak is
//      within that window, gTarget <= target/peak_emitted — the ceiling holds.
//   4. gain snaps DOWN to gTarget instantly (the drop is already La ahead of the
//      peak at the output) and recovers toward it with an exponential release.
//   5. apply the gain to the DELAYED (La-old) frame.
//
// RT contract: Prepare() sizes the delay ring + deque storage; Process() is pure
// arithmetic on that preallocated state (no alloc / locks / I/O). LatencySamples()
// is fixed at Prepare() and constant for the Process() lifetime (the graph sizes
// its compensating delays from it once) — SetParam on the look-ahead slot is
// therefore ignored, per the IEffect contract for reallocating params.
//
// Kit-free (STL only), C++17, host-testable.
#pragma once

#include "IEffect.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace daw {

class LookaheadLimiter : public IEffect {
public:
    // ceilingDb    = output ceiling the signal must never exceed (dBFS, <= 0)
    // lookaheadMs  = look-ahead / attack window; also the reported latency
    // releaseMs    = gain-recovery time constant after a peak passes
    // inGainDb     = gain applied into the limiter (drive the signal to the ceiling)
    LookaheadLimiter(double ceilingDb = -1.0, double lookaheadMs = 5.0,
                     double releaseMs = 60.0, double inGainDb = 0.0);

    void Prepare(double sampleRate) override;
    void Process(float* stereo, int frames) override;
    void Reset() override;
    void SetParam(int slot, float value) override;

    // Fixed at Prepare(): the look-ahead window in frames (>= 1). This is what
    // PDC compensates.
    int LatencySamples() const override { return fLatency; }

    // Deepest gain reduction over the last block, in dB (<= 0), for the editor.
    float MeterDb() const override { return fGrDb.load(std::memory_order_relaxed); }

    const char* Name() const override { return "Limiter"; }

private:
    void Recompute();   // derive target / input gain / release coef from params

    // Parameters.
    double fCeilingDb   = -1.0;
    double fLookaheadMs = 5.0;
    double fReleaseMs   = 60.0;
    double fInGainDb    = 0.0;
    double fSampleRate  = 48000.0;

    // Derived (Recompute / Prepare).
    double fTargetLin = 1.0;   // ceiling * safety (linear)
    double fInGainLin = 1.0;
    double fAlphaR    = 0.0;   // release smoothing coef
    int    fLatency   = 0;     // La, look-ahead in frames (set in Prepare, >= 1)

    // Delay ring: La interleaved-stereo frames (2*La floats).
    std::vector<float> fDelay;
    std::size_t        fWrite = 0;

    // Monotonic-decreasing deque (circular) for the sliding window maximum of the
    // input peak over the last La+1 frames. Preallocated; front holds the max.
    std::vector<float>       fMqVal;   // peak values, non-increasing head->tail
    std::vector<std::int64_t> fMqIdx;  // input index each value was seen at
    std::size_t fMqHead = 0;
    std::size_t fMqCount = 0;

    std::int64_t fCounter = 0;   // running input frame index
    double       fGcur    = 1.0; // live applied gain (linear)

    std::atomic<float> fGrDb{0.0f};
};

} // namespace daw
