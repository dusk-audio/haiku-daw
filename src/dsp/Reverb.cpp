#include "Reverb.h"

#include "duskverb/DattorroPlateVintage.h"   // verbatim DuskVerb plate engine

#include <algorithm>
#include <cmath>

namespace daw {

namespace {
// Base delay lengths at the 44.1 kHz calibration rate (DuskVerb's
// kBaseSampleRate convention); scaled to the runtime rate in Prepare(). Comb
// tunings are the classic Freeverb primes; allpass tunings are the canonical
// Schroeder diffuser primes. The right channel is offset by kStereoSpread for a
// decorrelated stereo image (Freeverb's stereospread).
constexpr int    kCombTuning[Reverb::kNumCombs]      = { 1557, 1617, 1491, 1422 };
constexpr int    kAllpassTuning[Reverb::kNumAllpass] = { 556, 441 };
constexpr int    kStereoSpread = 23;
constexpr double kBaseSampleRate = 44100.0;   // DuskVerb calibration anchor

// Fixed in-loop damping (Freeverb-style one-pole LP inside each comb feedback)
// and the fixed diffuser coefficient for the Schroeder allpasses.
constexpr float kDamping        = 0.25f;
constexpr float kAllpassG       = 0.5f;

// Tiny DC bias on the feedback path so quiet tails never fall into denormal
// range (mirrors DspUtils::kDenormalPrevention). Inaudible, keeps the FPU fast.
constexpr float kDenormal = 1.0e-15f;
} // namespace

Reverb::Reverb(double roomSize, double mix)
    : fRoomSize(roomSize), fMix(mix) {
    BuildLines();   // self-safe if Process runs before Prepare
}

Reverb::~Reverb() = default;   // out-of-line: plate type complete here

void Reverb::SetParams(double roomSize, double mix) {
    fRoomSize = roomSize;
    fMix      = mix;
    // Feedback is read live in Process(); no need to rebuild the lines.
}

void Reverb::Prepare(double sampleRate) {
    if (sampleRate > 0)
        fSampleRate = sampleRate;
    BuildLines();
    if (fAlgo == (int)ReverbAlgo::DuskPlate) BuildPlate();
}

// (Re)create the DuskVerb plate engine (allocates — call off the RT thread).
// The engine's post-prepare defaults are the calibrated Vintage Vocal Plate;
// the DAW's Size / Decay / Tone params ride on top.
void Reverb::BuildPlate() {
    fPlate.reset(new DattorroPlateVintage());
    fPlate->prepare(fSampleRate, kPlateChunk);
    fInL.assign(kPlateChunk, 0.0f);  fInR.assign(kPlateChunk, 0.0f);
    fOutL.assign(kPlateChunk, 0.0f); fOutR.assign(kPlateChunk, 0.0f);
    const double sz = std::clamp(fRoomSize, 0.0, 1.0);
    const double tn = std::clamp(fTone, 0.0, 1.0);
    fPlate->setDecayTime((float)std::max(0.1, fDecay));
    fPlate->setSize((float)sz);
    fPlate->setTrebleMultiply((float)(0.5 + tn));   // 0.5..1.5
}

void Reverb::BuildLines() {
    const double scale = fSampleRate / kBaseSampleRate;
    for (int c = 0; c < 2; c++) {
        const int spread = (c == 0) ? 0 : kStereoSpread;
        for (int i = 0; i < kNumCombs; i++) {
            fComb[c][i].Init((int)std::lround((kCombTuning[i] + spread) * scale));
            fCombLP[c][i] = 0.0f;
        }
        for (int i = 0; i < kNumAllpass; i++)
            fAllpass[c][i].Init((int)std::lround((kAllpassTuning[i] + spread) * scale));
    }
}

void Reverb::Reset() {
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < kNumCombs; i++)   { fComb[c][i].Clear(); fCombLP[c][i] = 0.0f; }
        for (int i = 0; i < kNumAllpass; i++) fAllpass[c][i].Clear();
    }
    if (fPlate) fPlate->clearBuffers();
}

void Reverb::Process(float* stereo, int frames) {
    // DuskVerb plate: de-interleave in chunks, run the engine (100% wet), then
    // mix wet/dry back. The engine tail is always live; `mix` blends it in.
    if (fAlgo == (int)ReverbAlgo::DuskPlate && fPlate) {
        const float wet = (float)std::max(0.0, std::min(1.0, fMix));
        const float dry = 1.0f - wet;
        int off = 0;
        while (off < frames) {
            const int n = std::min(frames - off, kPlateChunk);
            for (int i = 0; i < n; i++) {
                fInL[i] = stereo[(off + i) * 2 + 0];
                fInR[i] = stereo[(off + i) * 2 + 1];
            }
            fPlate->process(fInL.data(), fInR.data(),
                            fOutL.data(), fOutR.data(), n);
            for (int i = 0; i < n; i++) {
                stereo[(off + i) * 2 + 0] = fInL[i] * dry + fOutL[i] * wet;
                stereo[(off + i) * 2 + 1] = fInR[i] * dry + fOutR[i] * wet;
            }
            off += n;
        }
        return;
    }

    // Freeverb room-size map: feedback = roomSize*0.28 + 0.7 (~0.7..0.98).
    double rs = fRoomSize;
    if (rs < 0.0) rs = 0.0;
    if (rs > 1.0) rs = 1.0;
    const float combFb = (float)(0.7 + 0.28 * rs);
    const float wet = (float)std::max(0.0, std::min(1.0, fMix));
    const float dry = 1.0f - wet;

    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < 2; c++) {
            const float in = stereo[i * 2 + c];

            // Parallel low-pass-feedback comb bank (Freeverb LBCF).
            float acc = 0.0f;
            for (int k = 0; k < kNumCombs; k++) {
                Line& L = fComb[c][k];
                const float y = L.buf[L.pos];          // delayed output
                // One-pole damping inside the feedback loop.
                fCombLP[c][k] = y * (1.0f - kDamping) + fCombLP[c][k] * kDamping;
                L.buf[L.pos] = in + fCombLP[c][k] * combFb + kDenormal;
                if (++L.pos >= (size_t)L.size) L.pos = 0;
                acc += y;
            }
            acc /= (float)kNumCombs;

            // Series canonical Schroeder allpass diffusers (DattorroTank form):
            //   vd = read; vn = in + g*vd; write vn; out = vd - g*vn.
            for (int k = 0; k < kNumAllpass; k++) {
                Line& L = fAllpass[c][k];
                const float vd = L.buf[L.pos];
                const float vn = acc + kAllpassG * vd + kDenormal;
                L.buf[L.pos] = vn;
                if (++L.pos >= (size_t)L.size) L.pos = 0;
                acc = vd - kAllpassG * vn;
            }

            stereo[i * 2 + c] = in * dry + acc * wet;
        }
    }
}

void Reverb::SetParam(int slot, float v) {
    switch (slot) {
        case 0:
            fRoomSize = v;
            if (fPlate) fPlate->setSize((float)std::clamp((double)v, 0.0, 1.0));
            break;
        case 1: fMix = v; break;
        case 2: fAlgo = (int)(v + 0.5f); break;   // rebuilt on next Prepare
        case 3:
            fDecay = v;
            if (fPlate) fPlate->setDecayTime((float)std::max(0.1f, v));
            break;
        case 4:
            fTone = v;
            if (fPlate) fPlate->setTrebleMultiply(
                (float)(0.5 + std::clamp((double)v, 0.0, 1.0)));
            break;
    }
}

} // namespace daw
