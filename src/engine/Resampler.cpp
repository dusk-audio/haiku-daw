#include "Resampler.h"

namespace daw {

Resampler::Resampler(double inRate, double outRate)
    : fRatio((inRate > 0 && outRate > 0) ? outRate / inRate : 1.0),
      fStep((inRate > 0 && outRate > 0) ? inRate / outRate : 1.0),
      fPos(0.0), fPrevL(0.0f), fPrevR(0.0f), fHavePrev(false) {}

void Resampler::Process(const float* in, size_t inFrames,
                        std::vector<float>& out) {
    if (inFrames == 0)
        return;

    // Seed the interpolation edge on the very first frame ever seen.
    if (!fHavePrev) {
        fPrevL = in[0];
        fPrevR = in[1];
        fHavePrev = true;
        fPos = 0.0;
    }

    size_t i = 0;   // current input frame index within this chunk
    while (i < inFrames) {
        if (fPos < 1.0) {
            // Interpolate between the previous frame and input frame i.
            const float curL = in[i * 2 + 0];
            const float curR = in[i * 2 + 1];
            const float frac = static_cast<float>(fPos);
            out.push_back(fPrevL + (curL - fPrevL) * frac);
            out.push_back(fPrevR + (curR - fPrevR) * frac);
            fPos += fStep;
        } else {
            // Advance the interpolation edge to the next input frame.
            fPos -= 1.0;
            fPrevL = in[i * 2 + 0];
            fPrevR = in[i * 2 + 1];
            i++;
        }
    }
    // fPos and fPrev carry to the next chunk; the last frame of this chunk is
    // the left edge for the first frame of the next.
}

} // namespace daw
