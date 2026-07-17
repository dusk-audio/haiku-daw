// Limiter — offline look-ahead true-peak limiter for the export/mastering path.
//
// Given a finished interleaved-stereo master buffer, it detects the 4x-
// oversampled TRUE peak (inter-sample, dBTP) per sample and applies a stereo-
// linked gain-reduction envelope so the output true peak never exceeds a
// ceiling. This is the real limiter the export loudness-normalization was a
// gain-backoff placeholder for: normalization can now push the program up to a
// target LUFS and let the limiter — not a whole-program attenuation — hold the
// ceiling, so quiet material reaches target loudness instead of the whole mix
// being pulled down by a single peak.
//
// It runs OFFLINE over the whole buffer, so the look-ahead attack is realized
// as an anticipatory backward smoothing pass (a dip that ramps in before each
// peak) with ZERO added latency — no delay line, no PDC needed on the master
// sink. A live IEffect version (which would report LatencySamples() and feed
// Phase Y plugin-delay-compensation) is a later addition.
//
// Guarantee: the applied gain g[n] <= ceil / truePeak[n] for every sample, so
// the sample-domain output is bounded and the oversampled true peak stays at or
// under the ceiling (within the slow-varying-gain approximation the whole
// true-peak-limiter family relies on).
//
// Kit-free (STL only), C++17, host-testable.
#pragma once

#include <cstddef>

namespace daw {

class Limiter {
public:
    Limiter() = default;
    Limiter(float ceilingDb, float attackMs, float releaseMs)
        : fCeilingDb(ceilingDb), fAttackMs(attackMs), fReleaseMs(releaseMs) {}

    // Limit `frames` interleaved-stereo frames (2*frames floats) in place at
    // `sampleRate` Hz. Returns the maximum gain reduction applied, in dB
    // (>= 0; 0.0 means the signal was already under the ceiling). A no-op if
    // stereo is null, frames is 0, or sampleRate <= 0.
    float Process(float* stereo, std::size_t frames, double sampleRate);

    void  SetCeilingDb(float db) { fCeilingDb = db; }
    void  SetAttackMs(float ms)  { fAttackMs  = ms; }
    void  SetReleaseMs(float ms) { fReleaseMs = ms; }
    float CeilingDb() const { return fCeilingDb; }

private:
    float fCeilingDb = -1.0f;   // dBTP ceiling the output must not exceed
    float fAttackMs  = 2.0f;    // look-ahead / attack time
    float fReleaseMs = 60.0f;   // gain-recovery time constant
};

} // namespace daw
