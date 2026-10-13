#include "Resampler.h"

namespace daw {

Resampler::Resampler(double inRate, double outRate, int channels)
    : fRatio((inRate > 0 && outRate > 0) ? outRate / inRate : 1.0),
      fStep((inRate > 0 && outRate > 0) ? inRate / outRate : 1.0),
      fPos(0.0),
      fChannels((channels < 1) ? 1 : (channels > kMaxChannels ? kMaxChannels : channels)),
      fHavePrev(false) {
    for (int c = 0; c < kMaxChannels; c++) fPrev[c] = 0.0f;
}

void Resampler::Reset() {
    fPos = 0.0;
    fHavePrev = false;
    for (int c = 0; c < kMaxChannels; c++) fPrev[c] = 0.0f;
}

void Resampler::Process(const float* in, size_t inFrames,
                        std::vector<float>& out) {
    if (inFrames == 0)
        return;

    const int ch = fChannels;   // 1 or 2; index each frame as in[i*ch + c]

    // Equal rates: pass the chunk through, as this class' contract says
    // ("a ratio of 1 passes samples through unchanged").
    //
    // The interpolation loop below cannot express that: it seeds its left edge
    // with in[0] and then emits at fPos 0, so the first frame is emitted twice
    // and the chunk's LAST frame never is -- a 1:1 decode came out one frame
    // late and one frame short, at the right total length. Only a varying
    // signal shows it (a constant one reads the same either way, which is why
    // tests/resampler_tests' constant-frame case passed over it). Found
    // independently by the M7.2 and M4.5 branches; every clip at the project's
    // own rate took this path.
    //
    // fStep is fixed at construction, so an equal-rate instance stays
    // equal-rate: there is no later chunk to hand the carried edge to.
    if (fStep == 1.0) {
        out.insert(out.end(), in, in + inFrames * (size_t)ch);
        for (int c = 0; c < ch; c++) fPrev[c] = in[(inFrames - 1) * (size_t)ch + c];
        fHavePrev = true;
        fPos = 0.0;
        return;
    }

    // Seed the interpolation edge on the very first frame ever seen.
    if (!fHavePrev) {
        for (int c = 0; c < ch; c++) fPrev[c] = in[c];
        fHavePrev = true;
        fPos = 0.0;
    }

    size_t i = 0;   // current input frame index within this chunk
    while (i < inFrames) {
        if (fPos < 1.0) {
            // Interpolate between the previous frame and input frame i.
            const float frac = static_cast<float>(fPos);
            for (int c = 0; c < ch; c++) {
                const float cur = in[i * ch + c];
                out.push_back(fPrev[c] + (cur - fPrev[c]) * frac);
            }
            fPos += fStep;
        } else {
            // Advance the interpolation edge to the next input frame.
            fPos -= 1.0;
            for (int c = 0; c < ch; c++) fPrev[c] = in[i * ch + c];
            i++;
        }
    }
    // fPos and fPrev carry to the next chunk; the last frame of this chunk is
    // the left edge for the first frame of the next.
}

} // namespace daw
