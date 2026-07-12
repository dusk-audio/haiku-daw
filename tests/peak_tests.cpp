// Host-buildable test for PeakCache. Writes a known PCM16 stereo WAV, builds
// an envelope from it via WavSource, and checks bucket count, per-bucket
// min/max, and the Range() aggregation. No Haiku kits needed.

#include "../src/model/PeakCache.h"
#include "../src/engine/WavSource.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

static void put_u32(std::ofstream& f, uint32_t v) {
    uint8_t b[4] = { uint8_t(v), uint8_t(v>>8), uint8_t(v>>16), uint8_t(v>>24) };
    f.write((char*)b, 4);
}
static void put_u16(std::ofstream& f, uint16_t v) {
    uint8_t b[2] = { uint8_t(v), uint8_t(v>>8) };
    f.write((char*)b, 2);
}

static void write_wav(const char* path, uint32_t rate,
                      const std::vector<int16_t>& interleaved) {
    std::ofstream f(path, std::ios::binary);
    const uint16_t channels = 2, bits = 16;
    const uint32_t dataBytes = interleaved.size() * sizeof(int16_t);
    const uint32_t byteRate = rate * channels * (bits/8);
    f.write("RIFF", 4); put_u32(f, 36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); put_u32(f, 16);
    put_u16(f, 1); put_u16(f, channels); put_u32(f, rate);
    put_u32(f, byteRate); put_u16(f, channels * (bits/8)); put_u16(f, bits);
    f.write("data", 4); put_u32(f, dataBytes);
    for (int16_t s : interleaved) put_u16(f, (uint16_t)s);
}

using namespace daw;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
    const char* path = "peak_test_tmp.wav";

    // 8 stereo frames. Mono-fold = 0.5*(L+R). Design the values so buckets of
    // 4 frames have known extremes:
    //   frames 0..3 mono: 0, 0.5, -0.5, 1.0   -> bucket0 min -0.5 max 1.0
    //   frames 4..7 mono: -1.0, 0.25, 0.25, 0 -> bucket1 min -1.0 max 0.25
    const int16_t H = 16384;           // 0.5
    std::vector<int16_t> s = {
        0,0,        H,H,        -H,-H,      32767,32767,   // bucket 0
        -32768,-32768, H/2,H/2, H/2,H/2,   0,0,            // bucket 1
    };
    write_wav(path, 48000, s);

    WavSource w;
    CHECK(w.Open(path));

    PeakCache pc;
    CHECK(pc.Build(w, 4));           // 4 frames per bucket
    CHECK(pc.IsValid());
    CHECK(pc.FramesPerBucket() == 4);
    CHECK(pc.TotalFrames() == 8);
    CHECK(pc.BucketCount() == 2);

    Peak b0 = pc.At(0);
    CHECK(near(b0.min, -0.5f));
    CHECK(near(b0.max, 32767/32768.0f));   // ~1.0

    Peak b1 = pc.At(1);
    CHECK(near(b1.min, -1.0f));
    CHECK(near(b1.max, H/2/32768.0f));     // 0.25

    // Range spanning both buckets = global extremes.
    Peak all = pc.Range(0, 8);
    CHECK(near(all.min, -1.0f));
    CHECK(near(all.max, 32767/32768.0f));

    // Range inside bucket 0 only.
    Peak r0 = pc.Range(0, 4);
    CHECK(near(r0.min, -0.5f));
    CHECK(near(r0.max, 32767/32768.0f));

    // Out-of-range / empty spans -> flat {0,0}.
    Peak empty = pc.Range(100, 200);
    CHECK(near(empty.min, 0.0f) && near(empty.max, 0.0f));
    Peak rev = pc.Range(5, 5);
    CHECK(near(rev.min, 0.0f) && near(rev.max, 0.0f));

    std::remove(path);
    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
