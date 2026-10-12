// Host-buildable tests for the offline windowed-sinc SincResampler.
//
// What matters for a resampler that produces a file rather than a monitor
// signal is measurable and is measured here rather than asserted in prose:
//
//   - ALIASING: a tone above the destination Nyquist must not fold back into
//     the band. The linear Resampler is measured alongside, so the threshold
//     is a real comparison and not a number nobody can calibrate.
//   - PASSBAND FLATNESS: every tone below the guard band comes back at its own
//     level, so a rate conversion does not tilt the mix. The linear resampler
//     fails this near the band edge, which is exactly why this class exists.
//   - IDENTITY: equal rates are a bit-exact passthrough, because a project
//     exported at its own rate must not change by a single sample.
//
// Plus the streaming contract the disk/export decode loop depends on: chunked
// calls equal one big call, the frame count matches the ratio, mono does not
// read past its frame, and a constant stays constant.

#include "../src/dsp/SincResampler.h"
#include "../src/engine/Resampler.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

namespace {

std::vector<float> SineFrames(size_t n, double freq, double rate, float amp = 1.0f) {
    std::vector<float> d(n * 2, 0.0f);
    for (size_t i = 0; i < n; i++) {
        const float v = amp * (float)std::sin(2.0 * M_PI * freq * (double)i / rate);
        d[i * 2 + 0] = v;
        d[i * 2 + 1] = v;
    }
    return d;
}

std::vector<float> ConstFrames(size_t n, float v) {
    return std::vector<float>(n * 2, v);
}

// One whole stream through one resampler: Process the lot, then Flush, which
// is what the exporter's decode loop does.
std::vector<float> Resample(SincResampler& r, const std::vector<float>& in) {
    std::vector<float> out;
    r.Process(in.data(), in.size() / 2, out);
    r.Flush(out);
    return out;
}

// Output frames covered by a Stream: everything except the filter's own
// tail, where the signal after the end of the data is not knowable and is
// read as silence (the fade-out Flush documents).
constexpr size_t kKernelTail = 200;

// The filter's start-up transient, in output frames: until the window is
// filled with real samples the output is a band-limited version of a
// truncated signal, which puts broadband energy in for a fraction of a
// millisecond. Excluded by the RMS-style measurements below, exactly as a
// bench measurement would ignore the first block; the passband/flatness
// probes use a settled window further in anyway.
constexpr size_t kSettle = 2048;

// RMS of the left channel over frames [from, to), in dBFS (a full-scale sine
// reads -3.01 dB). RMS needs no integer-cycle window, which is what makes it
// the right measure for "is there anything there at all" — e.g. an alias.
double RmsDb(const std::vector<float>& d, size_t from, size_t to) {
    if (to > d.size() / 2) to = d.size() / 2;
    if (to <= from) return -300.0;
    double acc = 0.0;
    for (size_t i = from; i < to; i++) {
        const double v = d[i * 2];
        acc += v * v;
    }
    const double rms = std::sqrt(acc / (double)(to - from));
    return 20.0 * std::log10(rms / 0.7071067811865476 + 1e-300);
}

// Amplitude of the component at `freq`, by coherent correlation over `frames`
// frames starting at `from`. Exact when the window holds a whole number of
// cycles, which the callers arrange (frequencies that are multiples of 10 Hz
// over 4800 frames at 48 kHz).
double ToneAmp(const std::vector<float>& d, size_t from, size_t frames,
               double freq, double rate) {
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < frames; i++) {
        const size_t k = from + i;
        if (k * 2 >= d.size()) break;
        const double ph = 2.0 * M_PI * freq * (double)i / rate;
        re += d[k * 2] * std::cos(ph);
        im += d[k * 2] * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (double)frames;
}

} // namespace

int main() {
    // --- Identity: equal rates are bit-exact and produce the same count -----
    {
        SincResampler r(48000, 48000);
        CHECK(r.IsIdentity());
        std::vector<float> in = SineFrames(1000, 997.0, 48000.0, 0.37f);
        std::vector<float> out = Resample(r, in);
        CHECK(out.size() == in.size());
        bool exact = out.size() == in.size();
        for (size_t i = 0; exact && i < in.size(); i++) exact = (out[i] == in[i]);
        CHECK(exact);   // not "close": a passthrough must not round
    }

    // --- Frame counts follow the ratio ------------------------------------
    {
        SincResampler down(96000, 48000);
        CHECK(!down.IsIdentity());
        CHECK(std::fabs(down.Ratio() - 0.5) < 1e-12);
        std::vector<float> out = Resample(down, ConstFrames(1000, 0.5f));
        const long frames = (long)(out.size() / 2);
        CHECK((long)std::abs(frames - 500) <= 1);

        SincResampler up(24000, 48000);
        std::vector<float> out2 = Resample(up, ConstFrames(500, -0.3f));
        const long frames2 = (long)(out2.size() / 2);
        CHECK((long)std::abs(frames2 - 1000) <= 2);
    }

    // --- A constant survives as that constant (unity DC gain per phase) ----
    {
        SincResampler r(96000, 48000);
        std::vector<float> out = Resample(r, ConstFrames(4000, 0.5f));
        // Unity DC gain per PHASE (and the history primed with the stream's
        // own first sample) means this holds from the very first frame; only
        // the unknowable tail is excluded. The tolerance is tight on purpose:
        // the kernel is normalised per phase, so a constant comes back to
        // within float accumulation noise — the raw windowed sinc's own DC
        // gain is only good to ~1e-4, and that would slip past a loose bound.
        bool ok = out.size() / 2 > kKernelTail + 10;
        for (size_t i = 0; ok && i < out.size() / 2 - kKernelTail; i++)
            ok = std::fabs(out[i * 2] - 0.5f) < 2e-6f &&
                 std::fabs(out[i * 2 + 1] - 0.5f) < 2e-6f;
        CHECK(out.size() / 2 > 1000);
        CHECK(ok);
    }

    // --- Aliasing rejection: a tone above the destination Nyquist ----------
    // 30 kHz into a 96k -> 48k conversion folds to 48 - 30 = 18 kHz if nothing
    // stops it. The measurement window skips the filter's own transient at
    // both ends. The linear resampler is measured on the same input for
    // contrast: the assertion is not just "quiet" but "quieter than the thing
    // this replaces by a wide margin".
    double aliasDb = 0.0, linearAliasDb = 0.0;
    {
        const double F = 30000.0;
        std::vector<float> in = SineFrames(96000, F, 96000.0);      // 1 s
        SincResampler r(96000, 48000);
        std::vector<float> out = Resample(r, in);
        CHECK(out.size() / 2 > 47000);
        aliasDb = RmsDb(out, kSettle, out.size() / 2 - kSettle);

        Resampler lin(96000, 48000);
        std::vector<float> lout;
        lin.Process(in.data(), in.size() / 2, lout);
        linearAliasDb = RmsDb(lout, kSettle, lout.size() / 2 - kSettle);

        std::printf("  aliasing  : sinc %.1f dBFS, linear %.1f dBFS "
                    "(30 kHz -> 18 kHz, 96k -> 48k)\n", aliasDb, linearAliasDb);
        CHECK(aliasDb < -70.0);
        CHECK(aliasDb < linearAliasDb - 30.0);
    }

    // --- Passband flatness -------------------------------------------------
    // Everything a master actually contains must come back at its own level;
    // 0.1 dB across the band is inaudible and is far tighter than the linear
    // interpolator manages near the edge.
    {
        SincResampler r(96000, 48000);
        double worst = 0.0, worstF = 0.0;
        for (double F : { 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0,
                          19200.0 }) {
            std::vector<float> in = SineFrames(96000, F, 96000.0);
            std::vector<float> out = Resample(r, in);
            // 4800 output frames = F/10 whole cycles for every F above.
            const double amp = ToneAmp(out, 2048, 4800, F, 48000.0);
            const double db  = 20.0 * std::log10(amp + 1e-30);
            if (std::fabs(db) > std::fabs(worst)) { worst = db; worstF = F; }
        }
        std::printf("  passband  : worst %.3f dB at %.0f Hz (want <= 0.1)\n",
                    worst, worstF);
        CHECK(std::fabs(worst) <= 0.1);
    }

    // --- Upsampling: the source's images must not survive -------------------
    // A 10 kHz tone at 24k has images at 24k - 10k = 14 kHz; that one lands
    // inside the 48k output band and must be filtered away.
    {
        std::vector<float> in = SineFrames(24000, 10000.0, 24000.0);
        SincResampler r(24000, 48000);
        std::vector<float> out = Resample(r, in);
        CHECK(out.size() / 2 > 47000);
        const double pass = ToneAmp(out, 2048, 4800, 10000.0, 48000.0);
        const double image = ToneAmp(out, 2048, 4800, 14000.0, 48000.0);
        const double imageDb = 20.0 * std::log10(image / (pass + 1e-30) + 1e-30);
        std::printf("  image     : 14 kHz image %.1f dB re the 10 kHz tone "
                    "(24k -> 48k)\n", imageDb);
        CHECK(std::fabs(20.0 * std::log10(pass + 1e-30)) <= 0.1);   // passes
        CHECK(imageDb < -70.0);                                     // image gone
    }

    // --- Streaming: chunked == one call, sample for sample -----------------
    {
        std::vector<float> in = SineFrames(4096, 1234.0, 96000.0, 0.8f);
        SincResampler whole(96000, 48000);
        SincResampler split(96000, 48000);
        std::vector<float> a = Resample(whole, in);
        std::vector<float> b;
        split.Process(in.data(), 1024, b);
        split.Process(in.data() + 2048, 1024, b);
        split.Process(in.data() + 4096, 1024, b);
        split.Process(in.data() + 6144, 1024, b);
        split.Flush(b);
        CHECK(a.size() == b.size());
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); i++) same = (a[i] == b[i]);
        CHECK(same);
    }

    // --- Reset drops the carried history ----------------------------------
    // After Reset the next output frame must correspond to the next input
    // frame again, so a re-seek does not carry a tail of the old data.
    {
        SincResampler a(96000, 48000);
        std::vector<float> warm = SineFrames(512, 5000.0, 96000.0);
        Resample(a, warm);
        a.Reset();
        std::vector<float> cold = Resample(a, ConstFrames(512, 0.25f));
        SincResampler b(96000, 48000);
        std::vector<float> fresh = Resample(b, ConstFrames(512, 0.25f));
        CHECK(cold.size() == fresh.size());
        bool same = cold.size() == fresh.size();
        for (size_t i = 0; same && i < cold.size(); i++) same = (cold[i] == fresh[i]);
        CHECK(same);
    }

    // --- Mono: 1 float per frame, no read past an exactly-sized buffer -----
    {
        SincResampler r(24000, 48000, /*channels=*/1);
        CHECK(r.Channels() == 1);
        std::vector<float> in(2000, 0.42f);       // exactly 2000 mono frames
        std::vector<float> out;
        r.Process(in.data(), 2000, out);
        r.Flush(out);
        CHECK((long)std::abs((long)(out.size()) - 4000) <= 2);
        bool ok = out.size() > kKernelTail + 10;
        for (size_t i = 0; ok && i < out.size() - kKernelTail; i++)
            ok = std::fabs(out[i] - 0.42f) < 1e-4f;
        CHECK(ok);
    }

    // --- Degenerate inputs are survivable ---------------------------------
    {
        SincResampler clamp(48000, 48000, 9);
        CHECK(clamp.Channels() == 2);
        SincResampler nan(0.0, -1.0);
        CHECK(nan.Ratio() > 0.0);
        std::vector<float> out;
        nan.Process(nullptr, 100, out);
        CHECK(out.empty());
        nan.Process(out.data(), 0, out);
        CHECK(out.empty());
    }

    std::printf("sinc_resampler_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
