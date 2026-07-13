// Host-buildable test for the native WAV reader. Writes a known PCM16
// stereo file, reads it back through WavSource, and checks the header +
// that samples round-trip to the expected floats. No Haiku kits needed.

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

// Write a minimal PCM16 stereo WAV with the given interleaved samples.
static void write_wav(const char* path, uint32_t rate,
                      const std::vector<int16_t>& interleaved) {
    std::ofstream f(path, std::ios::binary);
    const uint16_t channels = 2, bits = 16;
    const uint32_t dataBytes = interleaved.size() * sizeof(int16_t);
    const uint32_t byteRate = rate * channels * (bits/8);
    f.write("RIFF", 4); put_u32(f, 36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); put_u32(f, 16);
    put_u16(f, 1);            // PCM
    put_u16(f, channels);
    put_u32(f, rate);
    put_u32(f, byteRate);
    put_u16(f, channels * (bits/8));   // block align
    put_u16(f, bits);
    f.write("data", 4); put_u32(f, dataBytes);
    for (int16_t s : interleaved) put_u16(f, (uint16_t)s);
}

using namespace daw;

int main() {
    const char* path = "wav_test_tmp.wav";
    // 4 stereo frames: L then R interleaved.
    std::vector<int16_t> samples = {
        0, 0,  16384, -16384,  -16384, 16384,  32767, -32768
    };
    write_wav(path, 48000, samples);

    WavSource w;
    CHECK(w.Open(path));
    CHECK(w.IsValid());
    CHECK(w.FrameRate() == 48000.0f);
    CHECK(w.SourceChannels() == 2);
    CHECK(w.TotalFrames() == 4);

    // Collect all frames from ReadChunk.
    std::vector<float> got;
    const float* blk = nullptr; size_t n = 0;
    while (w.ReadChunk(&blk, &n))
        for (size_t i = 0; i < n * 2; i++) got.push_back(blk[i]);

    CHECK(got.size() == 8);   // 4 frames * 2 ch
    // Expected floats = sample / 32768.
    auto near = [](float a, float b){ return std::fabs(a - b) < 1e-4f; };
    CHECK(near(got[0], 0.0f));
    CHECK(near(got[2], 16384/32768.0f));   // 0.5
    CHECK(near(got[3], -16384/32768.0f));  // -0.5
    CHECK(near(got[6], 32767/32768.0f));   // ~1.0
    CHECK(near(got[7], -1.0f));            // -32768/32768

    // Seek back to frame 1 and re-read: first frame out should be frame 1
    // (L=0.5, R=-0.5), proving the cursor repositioned.
    CHECK(w.Seek(1));
    const float* sblk = nullptr; size_t sn = 0;
    CHECK(w.ReadChunk(&sblk, &sn));
    CHECK(sn == 3);                        // frames 1..3 remain
    CHECK(near(sblk[0], 16384/32768.0f));  // frame 1 L = 0.5
    CHECK(near(sblk[1], -16384/32768.0f)); // frame 1 R = -0.5
    // Seek past end -> next read returns false (no data).
    CHECK(w.Seek(4));
    const float* eblk = nullptr; size_t en = 0;
    CHECK(!w.ReadChunk(&eblk, &en));

    std::remove(path);
    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
