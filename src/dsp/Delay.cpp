#include "Delay.h"

#include <algorithm>
#include <cmath>

namespace daw {

// Max delay time the line can hold. The buffer is sized once (Prepare) to this,
// so changing the delay time only moves the read tap — no realloc, RT-safe.
static constexpr double kMaxDelaySec = 2.0;

// Note divisions as a multiple of one beat (a quarter note in x/4). Order is the
// serialized division index; append only.
static const double kDivBeats[Delay::kDivisionCount] = {
    1.0,        // 1/4
    1.5,        // 1/4.  dotted quarter
    1.0 / 3.0,  // 1/4T  quarter triplet
    0.5,        // 1/8
    0.75,       // 1/8.  dotted eighth
    1.0 / 6.0,  // 1/8T  eighth triplet
    0.25,       // 1/16
};
static const char* kDivNames[Delay::kDivisionCount] = {
    "1/4", "1/4.", "1/4T", "1/8", "1/8.", "1/8T", "1/16",
};

const char* Delay::DivisionName(int i) {
    if (i < 0 || i >= kDivisionCount) i = 0;
    return kDivNames[i];
}

Delay::Delay(double delaySeconds, double feedback, double mix,
             bool sync, int division)
    : fDelaySec(delaySeconds), fFeedback(feedback), fMix(mix),
      fSync(sync), fDivision(division) {
    fBuf.assign(2, 0.0f);   // self-safe if Process runs before Prepare
}

void Delay::SetParams(double delaySeconds, double feedback, double mix) {
    fDelaySec = delaySeconds;
    fFeedback = feedback;
    fMix      = mix;
    UpdateTap();
}

void Delay::UpdateTap() {
    // Effective delay time: a note division of the tempo when synced, else the
    // manual time. Clamp the tap to the allocated capacity.
    double sec = fDelaySec;
    if (fSync && fBpm > 0.0) {
        int d = fDivision;
        if (d < 0 || d >= kDivisionCount) d = 0;
        sec = kDivBeats[d] * (60.0 / fBpm);
    }
    int frames = (int)std::lround(sec * fSampleRate);
    if (frames < 1)          frames = 1;
    if (frames > fCapFrames) frames = fCapFrames;
    fDelayFrames = frames;
}

void Delay::Prepare(double sampleRate) {
    if (sampleRate > 0)
        fSampleRate = sampleRate;
    // Size the line once to the maximum delay; the tap moves within it.
    fCapFrames = (int)std::lround(kMaxDelaySec * fSampleRate);
    if (fCapFrames < 1) fCapFrames = 1;
    fBuf.assign((size_t)fCapFrames * 2, 0.0f);
    fPos = 0;
    UpdateTap();
}

void Delay::Reset() {
    std::fill(fBuf.begin(), fBuf.end(), 0.0f);
    fPos = 0;
}

void Delay::Process(float* stereo, int frames) {
    // Clamp feedback below unity or the echo diverges to +Inf.
    float fb = (float)fFeedback;
    if (fb < 0.0f) fb = 0.0f;
    if (fb > 0.99f) fb = 0.99f;
    float wet = (float)fMix;
    if (wet < 0.0f) wet = 0.0f;
    if (wet > 1.0f) wet = 1.0f;   // clamp like the other effects (no phase flip)
    const float dry = 1.0f - wet;
    const int cap = fCapFrames;
    const int tap = fDelayFrames;

    for (int i = 0; i < frames; i++) {
        // Read the delayed sample `tap` frames behind the write cursor.
        int rp = (int)fPos - tap;
        if (rp < 0) rp += cap;
        for (int c = 0; c < 2; c++) {
            const float in = stereo[i * 2 + c];
            const float d  = fBuf[(size_t)rp * 2 + c];
            stereo[i * 2 + c] = in * dry + d * wet;
            fBuf[fPos * 2 + c] = in + d * fb;   // feed the echo back in
        }
        fPos++;
        if (fPos >= (size_t)cap)
            fPos = 0;
    }
}

void Delay::SetParam(int slot, float v) {
    // All slots are now RT-safe: time moves the tap within the fixed buffer, so
    // no reallocation happens in Process().
    switch (slot) {
        case 0: fDelaySec  = v;            UpdateTap(); break;   // time (s)
        case 1: fFeedback  = v;                         break;
        case 2: fMix       = v;                         break;
        case 3: fSync      = (v >= 0.5f);  UpdateTap(); break;   // sync on/off
        case 4: fDivision  = (int)(v + 0.5f); UpdateTap(); break; // division idx
        default: break;
    }
}

void Delay::SetTempo(double bpm) {
    if (bpm > 0.0) { fBpm = bpm; UpdateTap(); }
}

} // namespace daw
