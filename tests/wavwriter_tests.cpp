// Host-buildable test for WavWriter: write a known int16 stereo take, then
// read it back through WavSource and verify the header + samples round-trip.
// Proves the recorded-take file format is exactly what the player reads.

#include "../src/engine/WavWriter.h"
#include "../src/engine/WavSource.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    const char* path = "wavwriter_test_tmp.wav";

    // 4 stereo frames of known int16 samples (L, R interleaved).
    std::vector<int16_t> samples = {
        0, 0,  16384, -16384,  -16384, 16384,  32767, -32768
    };

    {
        WavWriter w;
        CHECK(w.Open(path, 96000, 2));
        CHECK(w.IsOpen());
        // Write in two chunks to exercise incremental appends.
        CHECK(w.WriteInt16(samples.data(), 4));       // 2 frames
        CHECK(w.WriteInt16(samples.data() + 4, 4));   // 2 frames
        CHECK(w.FramesWritten() == 4);
        CHECK(w.Close());
    }

    // Read it back through the player's own reader.
    WavSource s;
    CHECK(s.Open(path));
    CHECK(s.FrameRate() == 96000.0f);
    CHECK(s.SourceChannels() == 2);
    CHECK(s.TotalFrames() == 4);

    std::vector<float> got;
    const float* blk = nullptr; size_t n = 0;
    while (s.ReadChunk(&blk, &n))
        for (size_t i = 0; i < n * 2; i++) got.push_back(blk[i]);

    CHECK(got.size() == 8);
    CHECK(near(got[2], 16384/32768.0f));   // 0.5
    CHECK(near(got[3], -16384/32768.0f));  // -0.5
    CHECK(near(got[6], 32767/32768.0f));   // ~1.0
    CHECK(near(got[7], -1.0f));

    std::remove(path);

    // 4 GB guard: a write that would push the data chunk past the 32-bit RIFF
    // size limit is refused (returns false) without writing, so the file on disk
    // stays a valid WAV. The huge count is checked before the buffer is touched,
    // so a tiny buffer is safe to pass.
    {
        const char* gpath = "wavwriter_4gb_tmp.wav";
        WavWriter g;
        CHECK(g.Open(gpath, 48000, 2));
        int16_t tiny[2] = { 0, 0 };
        CHECK(g.WriteInt16(tiny, 2));                       // small write ok
        CHECK(!g.WriteInt16(tiny, (size_t)3000000000ull));  // would overflow -> refused
        CHECK(g.Close());
        // File is still valid + reads back just the 2 samples actually written.
        WavSource gs;
        CHECK(gs.Open(gpath));
        CHECK(gs.IsValid());
        CHECK(gs.TotalFrames() == 1);                       // 2 samples = 1 stereo frame
        std::remove(gpath);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
