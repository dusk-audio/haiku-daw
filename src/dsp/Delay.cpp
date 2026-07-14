#include "Delay.h"

#include <algorithm>
#include <cmath>

namespace daw {

Delay::Delay(double delaySeconds, double feedback, double mix)
    : fDelaySec(delaySeconds), fFeedback(feedback), fMix(mix) {
    fBuf.assign(2, 0.0f);   // self-safe if Process runs before Prepare
}

void Delay::SetParams(double delaySeconds, double feedback, double mix) {
    fDelaySec  = delaySeconds;
    fFeedback  = feedback;
    fMix       = mix;
    Prepare(fSampleRate);   // resize the line if the delay time changed
}

void Delay::Prepare(double sampleRate) {
    if (sampleRate > 0)
        fSampleRate = sampleRate;
    fDelayFrames = (int)std::lround(fDelaySec * fSampleRate);
    if (fDelayFrames < 1)
        fDelayFrames = 1;
    fBuf.assign((size_t)fDelayFrames * 2, 0.0f);
    fPos = 0;
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
    const float wet = (float)fMix;
    const float dry = 1.0f - wet;

    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < 2; c++) {
            const float in = stereo[i * 2 + c];
            const float d  = fBuf[fPos * 2 + c];
            stereo[i * 2 + c] = in * dry + d * wet;
            fBuf[fPos * 2 + c] = in + d * fb;   // feed the echo back in
        }
        fPos++;
        if (fPos >= (size_t)fDelayFrames)
            fPos = 0;
    }
}

void Delay::SetParam(int slot, float v) {
    // Slot 0 (time) would resize the line -> not RT-safe; skip. Feedback/mix are
    // read live in Process(), so set them directly.
    if (slot == 1) fFeedback = v;
    else if (slot == 2) fMix = v;
}

} // namespace daw
