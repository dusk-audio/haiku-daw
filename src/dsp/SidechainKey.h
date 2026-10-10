// SidechainKey — the little piece of state every keyed dynamics effect needs:
// the current block's external key, and the one detector reduction both the
// internal and the keyed path run on.
//
// The interesting design is on the IEffect side (IEffect::SetSidechain): the
// host resolves a track reference into audio and hands the effect that block's
// key, on the audio thread, immediately before Process(). An effect therefore
// only has to know how to (a) hold a pointer for the length of one call and
// (b) turn a stereo frame into the scalar its detector has always detected on.
// Both live here so Compressor and Gate cannot drift, and so a future keyed
// effect (a ducking delay, a vocoder) inherits the same rules.
//
// Kit-free (STL only), host-testable.
#pragma once

#include <cmath>

namespace daw {

// The flat-param slot the external-key toggle occupies in EVERY keyed effect:
// appended after the effect's own parameters (Compressor 0-4, Gate 0-4), so
// every older project's stored slots keep their meaning. The two effects read
// it through this name, and the editor's source picker writes it.
inline constexpr int kExtKeySlot = 5;

// What SetSidechain stores: a pointer to this block's key, valid only until
// the following Process() returns. Default-constructed = no key, so an effect
// that is never handed one behaves exactly as it did before this feature.
struct SidechainKey {
    const float* block = nullptr;   // interleaved stereo, one Process() long

    // Host entry point (IEffect::SetSidechain). A null pointer or an empty
    // block clears the key rather than keeping a stale one.
    void Set(const float* stereo, int frames) {
        block = (stereo && frames > 0) ? stereo : nullptr;
    }

    bool Active() const { return block != nullptr; }
};

// One frame's detector level. Both dynamics effects detect the stereo-linked
// PEAK of their input — max(|L|, |R|) — which is the scalar that made their
// gain computer, their ratios and their thresholds mean what they mean.
//
// An external key is reduced to the same scalar by MONO-SUMMING it, halved so
// that a centre-panned key at the same amplitude as the main signal reads the
// same level (an unscaled L+R sum would read +6 dB and silently move every
// threshold). One function for both paths, so the keyed detector cannot end up
// with different ballistics than the internal one.
inline double DetectorLevel(const float* frame, bool monoKey) {
    if (monoKey)
        return std::fabs(0.5 * ((double)frame[0] + (double)frame[1]));
    return std::max(std::fabs((double)frame[0]), std::fabs((double)frame[1]));
}

} // namespace daw
