// Host-buildable tests for the Widener stereo-width / utility effect.
// Covers: width=1 is an exact no-op; width=0 collapses to mono; width=2
// widens (the L-R difference grows); pan shifts level between channels.

#include "../src/dsp/Widener.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// A deterministic, asymmetric interleaved-stereo test signal.
static std::vector<float> signal(int frames) {
    std::vector<float> buf(frames * 2);
    for (int i = 0; i < frames; i++) {
        buf[i * 2]     = 0.5f  * std::sin(0.10f * i);          // L
        buf[i * 2 + 1] = 0.25f * std::sin(0.13f * i + 0.7f);   // R (different)
    }
    return buf;
}

int main() {
    const double SR = 48000.0;
    const int N = 256;

    // 1. width=1, pan=0, gain=1 is an exact no-op.
    {
        Widener w(1.0, 0.0, 1.0);
        w.Prepare(SR);
        auto in  = signal(N);
        auto out = in;
        w.Process(out.data(), N);
        bool identical = true;
        for (int i = 0; i < N * 2; i++)
            if (out[i] != in[i]) identical = false;
        CHECK(identical);
    }

    // 2. width=0 collapses to mono: L==R==(L+R)/2 of the input, per frame.
    {
        Widener w(0.0, 0.0, 1.0);
        w.Prepare(SR);
        auto in  = signal(N);
        auto out = in;
        w.Process(out.data(), N);
        bool ok = true;
        for (int i = 0; i < N; i++) {
            float mid = (in[i * 2] + in[i * 2 + 1]) * 0.5f;
            if (std::fabs(out[i * 2]     - mid) > 1e-6f) ok = false;
            if (std::fabs(out[i * 2 + 1] - mid) > 1e-6f) ok = false;
            if (std::fabs(out[i * 2] - out[i * 2 + 1]) > 1e-6f) ok = false;
        }
        CHECK(ok);
    }

    // 3. width=2 doubles the side component: the L-R difference grows exactly 2x.
    {
        Widener w(2.0, 0.0, 1.0);
        w.Prepare(SR);
        auto in  = signal(N);
        auto out = in;
        w.Process(out.data(), N);
        bool ok = true;
        for (int i = 0; i < N; i++) {
            float dIn  = in[i * 2]  - in[i * 2 + 1];
            float dOut = out[i * 2] - out[i * 2 + 1];
            if (std::fabs(dOut - 2.0f * dIn) > 1e-5f) ok = false;
        }
        CHECK(ok);
        // The mid (sum/2) is preserved by width scaling.
        bool midOk = true;
        for (int i = 0; i < N; i++) {
            float sIn  = (in[i * 2]  + in[i * 2 + 1])  * 0.5f;
            float sOut = (out[i * 2] + out[i * 2 + 1]) * 0.5f;
            if (std::fabs(sOut - sIn) > 1e-5f) midOk = false;
        }
        CHECK(midOk);
    }

    // 4. width=2 makes a hard-panned (L-only) input wider than width=1.
    {
        auto measureDiff = [&](double width) {
            Widener w(width, 0.0, 1.0);
            w.Prepare(SR);
            std::vector<float> buf(4 * 2, 0.0f);
            buf[0] = 1.0f; buf[1] = 0.0f;   // hard-left input
            w.Process(buf.data(), 1);
            return std::fabs(buf[0] - buf[1]);
        };
        CHECK(measureDiff(2.0) > measureDiff(1.0));
    }

    // 5. Pan shifts level between channels. Equal L==R input, pan right ->
    //    right louder than left; pan left -> left louder. Center is balanced.
    {
        // Center: balanced.
        {
            Widener w(1.0, 0.0, 1.0);
            w.Prepare(SR);
            std::vector<float> buf = {1.0f, 1.0f};
            w.Process(buf.data(), 1);
            CHECK(std::fabs(buf[0] - buf[1]) < 1e-6f);
            CHECK(std::fabs(buf[0] - 1.0f) < 1e-6f);   // unity at center
        }
        // Pan right.
        {
            Widener w(1.0, 0.5, 1.0);
            w.Prepare(SR);
            std::vector<float> buf = {1.0f, 1.0f};
            w.Process(buf.data(), 1);
            CHECK(buf[1] > buf[0]);
        }
        // Pan left.
        {
            Widener w(1.0, -0.5, 1.0);
            w.Prepare(SR);
            std::vector<float> buf = {1.0f, 1.0f};
            w.Process(buf.data(), 1);
            CHECK(buf[0] > buf[1]);
        }
        // Hard pan right silences the left channel.
        {
            Widener w(1.0, 1.0, 1.0);
            w.Prepare(SR);
            std::vector<float> buf = {1.0f, 1.0f};
            w.Process(buf.data(), 1);
            CHECK(std::fabs(buf[0]) < 1e-6f);
        }
    }

    // 6. gain scales output linearly (width=1, pan=0).
    {
        Widener w(1.0, 0.0, 0.5);
        w.Prepare(SR);
        auto in  = signal(N);
        auto out = in;
        w.Process(out.data(), N);
        bool ok = true;
        for (int i = 0; i < N * 2; i++)
            if (std::fabs(out[i] - 0.5f * in[i]) > 1e-6f) ok = false;
        CHECK(ok);
    }

    // 7. SetParams re-derives coefficients; Reset() is a harmless no-op.
    {
        Widener w;
        w.Prepare(SR);
        w.SetParams(0.0, 0.0, 1.0);   // now mono
        w.Reset();
        std::vector<float> buf = {1.0f, -1.0f};
        w.Process(buf.data(), 1);
        CHECK(std::fabs(buf[0]) < 1e-6f);   // (1 + -1)/2 = 0
        CHECK(std::fabs(buf[1]) < 1e-6f);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
