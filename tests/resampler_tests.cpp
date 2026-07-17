// Host-buildable tests for the streaming linear Resampler. Checks identity
// pass-through, up/down ratios (output count ~= inFrames * ratio), constant-
// signal preservation, and continuity across chunk boundaries.

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

// Build N interleaved-stereo frames with a constant value.
static std::vector<float> constFrames(size_t n, float v) {
    return std::vector<float>(n * 2, v);
}

int main() {
    // 1. Identity (equal rates): same frame count, same values.
    {
        Resampler r(48000, 48000);
        CHECK(r.IsIdentity());
        std::vector<float> out;
        auto in = constFrames(100, 0.25f);
        r.Process(in.data(), 100, out);
        CHECK(out.size() == 200);            // 100 stereo frames
        bool allEq = true;
        for (float s : out) if (std::fabs(s - 0.25f) > 1e-6f) allEq = false;
        CHECK(allEq);
    }

    // 2. Downsample 96k -> 48k: ~half the frames.
    {
        Resampler r(96000, 48000);
        std::vector<float> out;
        auto in = constFrames(1000, 0.5f);
        r.Process(in.data(), 1000, out);
        const size_t frames = out.size() / 2;
        CHECK(std::llabs((long long)frames - 500) <= 1);
        // Constant in -> constant out.
        bool ok = true;
        for (float s : out) if (std::fabs(s - 0.5f) > 1e-4f) ok = false;
        CHECK(ok);
    }

    // 3. Upsample 24k -> 48k: ~double the frames.
    {
        Resampler r(24000, 48000);
        std::vector<float> out;
        auto in = constFrames(500, -0.3f);
        r.Process(in.data(), 500, out);
        const size_t frames = out.size() / 2;
        CHECK(std::llabs((long long)frames - 1000) <= 2);
        bool ok = true;
        for (float s : out) if (std::fabs(s - (-0.3f)) > 1e-4f) ok = false;
        CHECK(ok);
    }

    // 4. Continuity: feeding one chunk of 1000 == two chunks of 500.
    {
        auto in = constFrames(1000, 0.1f);
        std::vector<float> whole, split;
        Resampler r1(44100, 48000);
        r1.Process(in.data(), 1000, whole);
        Resampler r2(44100, 48000);
        r2.Process(in.data(), 500, split);
        r2.Process(in.data() + 1000, 500, split);
        // Same total frame count (+/- 1) and no discontinuity spike.
        CHECK(std::llabs((long long)whole.size() - (long long)split.size()) <= 2);
        bool ok = true;
        for (float s : split) if (std::fabs(s - 0.1f) > 1e-4f) ok = false;
        CHECK(ok);
    }

    // 5. Mono (1 channel): a mono buffer must resample as 1 float/frame with no
    //    read past each frame (stereo indexing would OOB / misinterpret).
    {
        Resampler r(24000, 48000, /*channels=*/1);
        CHECK(r.Channels() == 1);
        // Exactly-sized mono buffer: 100 frames == 100 floats. A stereo-indexed
        // reader would touch in[199] here (OOB) — this must stay in bounds.
        std::vector<float> in(100, 0.42f);
        std::vector<float> out;
        r.Process(in.data(), 100, out);
        const size_t frames = out.size();          // 1 float per frame (mono)
        CHECK(std::llabs((long long)frames - 200) <= 2);   // 2x upsample
        bool ok = true;
        for (float s : out) if (std::fabs(s - 0.42f) > 1e-4f) ok = false;
        CHECK(ok);
    }

    // 6. Channel count clamps to [1, kMaxChannels] (bad input never over-indexes).
    {
        Resampler lo(48000, 48000, 0);   CHECK(lo.Channels() == 1);
        Resampler hi(48000, 48000, 9);   CHECK(hi.Channels() == 2);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
