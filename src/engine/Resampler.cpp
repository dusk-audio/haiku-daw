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

void Resampler::Process(const float* in, size_t inFrames,
                        std::vector<float>& out) {
    if (inFrames == 0)
        return;

    const int ch = fChannels;   // 1 or 2; index each frame as in[i*ch + c]

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
