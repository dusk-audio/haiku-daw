#include "Limiter.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace daw {

namespace {

// 4x polyphase true-peak FIR, identical construction to Loudness::BuildTruePeak
// (Hann-windowed sinc, DC-normalized per phase; phase 0 forced to identity so
// the estimate is never below the raw sample peak). Kept local so the limiter
// has no dependency on the meter.
constexpr int kOs   = 4;
constexpr int kTaps = 12;

struct TruePeakFir {
    double c[kOs][kTaps] = {{0}};
    TruePeakFir() {
        const double pi = 3.14159265358979323846;
        const int center = kTaps / 2 - 1;   // 5
        for (int p = 0; p < kOs; ++p) {
            const double d = static_cast<double>(p) / kOs;   // 0, .25, .5, .75
            double sum = 0.0;
            for (int t = 0; t < kTaps; ++t) {
                const double x = static_cast<double>(t) - center - d;
                const double s = (std::fabs(x) < 1e-9)
                                 ? 1.0 : std::sin(pi * x) / (pi * x);
                const double w = 0.5 - 0.5 * std::cos(2.0 * pi * (t + 0.5) / kTaps);
                c[p][t] = s * w;
                sum += c[p][t];
            }
            if (std::fabs(sum) > 1e-12)
                for (int t = 0; t < kTaps; ++t) c[p][t] /= sum;
        }
        for (int t = 0; t < kTaps; ++t)
            c[0][t] = (t == center) ? 1.0 : 0.0;
    }
};

} // namespace

float Limiter::Process(float* x, std::size_t N, double sampleRate) {
    if (!x || N == 0 || sampleRate <= 0.0)
        return 0.0f;

    const double ceilLin = std::pow(10.0, fCeilingDb / 20.0);
    if (!(ceilLin > 0.0))
        return 0.0f;
    // Aim a hair BELOW the ceiling: the headroom absorbs the small error
    // between the base-rate gain we apply and the oversampled peak it is meant
    // to control. (Multiplying the ceiling, not the detected peak — the latter
    // would raise the permitted output above the ceiling.)
    const double kSafety = 0.995;
    const double target  = ceilLin * kSafety;

    static const TruePeakFir fir;
    const int center = kTaps / 2 - 1;

    // --- 1. Per-sample required gain from the stereo-linked true peak. -------
    // Direct (random-access) convolution: for output sample m, reconstruct the
    // inter-sample values just after m from phases 1..3 (phase 0 is the sample
    // itself) and take the max magnitude over both channels.
    std::vector<float> req(N, 1.0f);
    for (std::size_t m = 0; m < N; ++m) {
        double tp = 0.0;
        for (int ch = 0; ch < 2; ++ch) {
            double localMax = std::fabs(static_cast<double>(x[m * 2 + ch]));
            for (int ph = 1; ph < kOs; ++ph) {
                double acc = 0.0;
                for (int t = 0; t < kTaps; ++t) {
                    const long idx = static_cast<long>(m) + center - t;
                    if (idx >= 0 && idx < static_cast<long>(N))
                        acc += static_cast<double>(x[idx * 2 + ch]) * fir.c[ph][t];
                }
                localMax = std::max(localMax, std::fabs(acc));
            }
            tp = std::max(tp, localMax);
        }
        if (tp > target)
            req[m] = static_cast<float>(target / tp);
    }

    // --- 2. Look-ahead window minimum: gain starts dropping La samples before
    // a peak so the reduction is in place by the time the peak arrives. --------
    // Validate the attack time before converting to a sample count: a negative /
    // NaN / infinite / absurdly large fAttackMs would otherwise cast to a bogus
    // std::size_t (UB) or overflow m + La. Clamp the look-ahead to [1, N].
    double attackSamp = fAttackMs * sampleRate / 1000.0;
    if (!std::isfinite(attackSamp) || attackSamp < 1.0) attackSamp = 1.0;
    if (attackSamp > static_cast<double>(N))            attackSamp = static_cast<double>(N);
    const std::size_t La = static_cast<std::size_t>(attackSamp);
    std::vector<float> env(N);
    {
        std::deque<std::size_t> dq;   // indices, req non-decreasing front->back
        std::size_t r = 0;
        for (std::size_t m = 0; m < N; ++m) {
            const std::size_t hi = std::min(N - 1, m + La);
            while (r <= hi) {
                while (!dq.empty() && req[dq.back()] >= req[r]) dq.pop_back();
                dq.push_back(r);
                ++r;
            }
            while (!dq.empty() && dq.front() < m) dq.pop_front();
            env[m] = req[dq.front()];
        }
    }

    // --- 3. Backward attack smoothing: round the leading edge of each dip so
    // the anticipatory ramp is smooth (no step click), never above the window
    // minimum (so the ceiling guarantee holds). -------------------------------
    const double alphaA = std::exp(-1.0 / attackSamp);   // attackSamp validated above
    {
        double s = env[N - 1];
        for (long m = static_cast<long>(N) - 1; m >= 0; --m) {
            const double g = env[m];
            s = g + (s - g) * alphaA;
            if (s > 1.0) s = 1.0;
            if (s < g) env[m] = static_cast<float>(s);   // env = min(gmin, s)
        }
    }

    // --- 4. Forward release: attack follows the envelope down instantly (it is
    // already ramped), release recovers toward it with a time constant. --------
    double releaseSamp = fReleaseMs * sampleRate / 1000.0;   // validate like attack
    if (!std::isfinite(releaseSamp) || releaseSamp < 1.0) releaseSamp = 1.0;
    const double alphaR = std::exp(-1.0 / releaseSamp);
    {
        double r2 = env[0];
        for (std::size_t m = 0; m < N; ++m) {
            const double g = env[m];
            if (g < r2) r2 = g;                    // attack: snap down
            else        r2 = g + (r2 - g) * alphaR;// release: rise slowly toward g
            env[m] = static_cast<float>(r2);
        }
    }

    // --- 5. Apply the stereo-linked gain; report the deepest reduction. ------
    float minGain = 1.0f;
    for (std::size_t m = 0; m < N; ++m) {
        const float g = env[m];
        if (g < minGain) minGain = g;
        x[m * 2 + 0] *= g;
        x[m * 2 + 1] *= g;
    }
    return (minGain < 1.0f && minGain > 0.0f)
               ? static_cast<float>(-20.0 * std::log10(minGain))
               : 0.0f;
}

} // namespace daw
