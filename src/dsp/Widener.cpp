#include "Widener.h"

#include <cmath>

namespace daw {

Widener::Widener(double width, double pan, double gain)
    : fWidth(width), fPan(pan), fGain(gain) {
    Recompute();
}

void Widener::SetParams(double width, double pan, double gain) {
    fWidth = width;
    fPan   = pan;
    fGain  = gain;
    Recompute();
}

void Widener::Prepare(double /*sampleRate*/) {
    // No sample-rate-dependent state; just (re)derive coefficients.
    Recompute();
}

// Equal-power pan law normalized so the center (pan=0) is unity gain on both
// channels, keeping width=1/pan=0/gain=1 an exact no-op.
//   t = (pan+1) * pi/4  maps pan [-1..+1] -> t [0..pi/2]
//   panL = cos(t)*sqrt2, panR = sin(t)*sqrt2   (both = 1 at t=pi/4)
void Widener::Recompute() {
    double pan = fPan;
    if (pan < -1.0) pan = -1.0;
    if (pan >  1.0) pan =  1.0;

    const double t = (pan + 1.0) * (M_PI / 4.0);
    fPanL = std::cos(t) * M_SQRT2 * fGain;
    fPanR = std::sin(t) * M_SQRT2 * fGain;
}

void Widener::Reset() {
    // Stateless — nothing to clear.
}

void Widener::Process(float* stereo, int frames) {
    const double width = fWidth;
    const double panL  = fPanL;
    const double panR  = fPanR;

    for (int i = 0; i < frames; i++) {
        const double l = stereo[i * 2];
        const double r = stereo[i * 2 + 1];

        const double mid  = (l + r) * 0.5;
        const double side = (l - r) * 0.5;

        const double nl = (mid + side * width) * panL;
        const double nr = (mid - side * width) * panR;

        stereo[i * 2]     = static_cast<float>(nl);
        stereo[i * 2 + 1] = static_cast<float>(nr);
    }
}

} // namespace daw
