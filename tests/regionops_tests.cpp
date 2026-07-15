// Host tests for RegionOps: peak, in-place reverse, silence-span detection.
//
// Build/run:
//   g++ -std=c++17 -Isrc src/model/RegionOps.cpp tests/regionops_tests.cpp -o /tmp/ro
#include "../src/model/RegionOps.h"

#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// Build an interleaved-stereo buffer from a per-frame mono level (L=R=level).
static std::vector<float> mono(const std::vector<float>& lv) {
    std::vector<float> b;
    b.reserve(lv.size() * 2);
    for (float v : lv) { b.push_back(v); b.push_back(v); }
    return b;
}

int main() {
    // PeakLinear: picks the largest absolute across both channels.
    {
        std::vector<float> b = { 0.1f, -0.2f,  0.3f, 0.05f,  -0.9f, 0.4f };
        CHECK(PeakLinear(b.data(), 3) == 0.9f);
        CHECK(PeakLinear(b.data(), 0) == 0.0f);   // empty -> 0
    }

    // ReverseStereo: frames flip, channels stay paired.
    {
        std::vector<float> b = { 1,10, 2,20, 3,30, 4,40 };   // 4 frames
        ReverseStereo(b.data(), 4);
        CHECK(b[0] == 4 && b[1] == 40);
        CHECK(b[2] == 3 && b[3] == 30);
        CHECK(b[6] == 1 && b[7] == 10);
        std::vector<float> odd = { 1,1, 2,2, 3,3 };           // odd count: middle fixed
        ReverseStereo(odd.data(), 3);
        CHECK(odd[0] == 3 && odd[2] == 2 && odd[4] == 1);
    }

    // NonSilentSpans: clean tone with no gaps -> one full span.
    {
        std::vector<float> lv(1000, 0.5f);
        auto b = mono(lv);
        auto s = NonSilentSpans(b.data(), 1000, 0.01f, 100, 0);
        CHECK(s.size() == 1);
        CHECK(s[0].start == 0 && s[0].end == 1000);
    }

    // A long central silence gap splits into two kept spans.
    {
        std::vector<float> lv(1000, 0.5f);
        for (int i = 400; i < 700; ++i) lv[i] = 0.0f;   // 300-frame gap
        auto b = mono(lv);
        auto s = NonSilentSpans(b.data(), 1000, 0.01f, 100, 0);
        CHECK(s.size() == 2);
        CHECK(s[0].start == 0   && s[0].end == 400);
        CHECK(s[1].start == 700 && s[1].end == 1000);
    }

    // Padding keeps a little air on each side of the gap (shrinks the cut).
    {
        std::vector<float> lv(1000, 0.5f);
        for (int i = 400; i < 700; ++i) lv[i] = 0.0f;
        auto b = mono(lv);
        auto s = NonSilentSpans(b.data(), 1000, 0.01f, 100, 20);
        CHECK(s.size() == 2);
        CHECK(s[0].end == 420);     // gap start 400 + pad 20
        CHECK(s[1].start == 680);   // gap end 700 - pad 20
    }

    // A short gap (< minSilence) is ignored: audio plays through -> one span.
    {
        std::vector<float> lv(1000, 0.5f);
        for (int i = 400; i < 450; ++i) lv[i] = 0.0f;   // 50-frame gap < 100
        auto b = mono(lv);
        auto s = NonSilentSpans(b.data(), 1000, 0.01f, 100, 0);
        CHECK(s.size() == 1);
        CHECK(s[0].start == 0 && s[0].end == 1000);
    }

    // Leading + trailing silence is trimmed.
    {
        std::vector<float> lv(1000, 0.0f);
        for (int i = 300; i < 600; ++i) lv[i] = 0.5f;   // sound only in the middle
        auto b = mono(lv);
        auto s = NonSilentSpans(b.data(), 1000, 0.01f, 100, 0);
        CHECK(s.size() == 1);
        CHECK(s[0].start == 300 && s[0].end == 600);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
