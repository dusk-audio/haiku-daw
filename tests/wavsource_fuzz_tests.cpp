// Adversarial fuzz corpus for the native WAV reader. A malformed, truncated,
// oversized, or garbage RIFF/WAVE must never crash, read out of bounds, hang,
// or drive an unbounded allocation — it must fail cleanly (Open returns false)
// or read to a bounded end. Deterministic PRNG so runs are reproducible; build
// with -DDAW_SANITIZE=ON to catch silent OOB reads under ASan/UBSan.

#include "../src/engine/WavSource.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// A well-formed 16-bit PCM stereo WAV as a byte vector we then corrupt.
static std::vector<uint8_t> baseWav() {
    auto u32 = [](std::vector<uint8_t>& v, uint32_t x) {
        v.push_back(x); v.push_back(x >> 8); v.push_back(x >> 16); v.push_back(x >> 24);
    };
    auto u16 = [](std::vector<uint8_t>& v, uint16_t x) {
        v.push_back(x); v.push_back(x >> 8);
    };
    auto tag = [](std::vector<uint8_t>& v, const char* s) {
        for (int i = 0; i < 4; i++) v.push_back((uint8_t)s[i]);
    };
    std::vector<uint8_t> v;
    const uint32_t frames = 256;
    const uint32_t dataBytes = frames * 2 * 2;   // stereo, 16-bit
    tag(v, "RIFF"); u32(v, 36 + dataBytes); tag(v, "WAVE");
    tag(v, "fmt "); u32(v, 16);
    u16(v, 1); u16(v, 2); u32(v, 48000); u32(v, 48000 * 4); u16(v, 4); u16(v, 16);
    tag(v, "data"); u32(v, dataBytes);
    for (uint32_t i = 0; i < dataBytes; i++) v.push_back((uint8_t)(i & 0xFF));
    return v;
}

// Open a byte buffer as a WAV and, if it opens, drain it fully. The whole point
// is that neither step crashes / OOBs / hangs; the return value is don't-care.
static void probe(const std::vector<uint8_t>& bytes) {
    const char* path = "wavfuzz_tmp.wav";
    { std::ofstream f(path, std::ios::binary);
      if (!bytes.empty()) f.write((const char*)bytes.data(), bytes.size()); }
    WavSource w;
    if (w.Open(path)) {
        const float* c = nullptr; size_t f = 0;
        // Hard iteration cap: a correct reader terminates at EOF; the cap turns
        // any accidental non-advancing loop into a failure instead of a hang.
        int guard = 0;
        while (w.ReadChunk(&c, &f)) {
            if (++guard > 200000) { CHECK(false && "ReadChunk did not terminate"); break; }
        }
        (void)w.Seek(0);
        (void)w.Seek(-100);
        (void)w.Seek(1LL << 40);        // absurd seek must clamp, not crash
        w.ReadChunk(&c, &f);
    }
    std::remove(path);
}

// Write bytes, return whether WavSource accepts them (for targeted assertions).
static bool opens(const std::vector<uint8_t>& bytes) {
    const char* path = "wavfuzz_open_tmp.wav";
    { std::ofstream f(path, std::ios::binary);
      if (!bytes.empty()) f.write((const char*)bytes.data(), bytes.size()); }
    WavSource w; const bool ok = w.Open(path);
    std::remove(path);
    return ok;
}

int main() {
    const std::vector<uint8_t> base = baseWav();

    // 1. The clean base opens and drains fine (sanity), and malformed variants
    //    that hit specific guards are REJECTED (not just non-crashing).
    CHECK(opens(base));
    probe(base);
    { std::vector<uint8_t> v = base; v[20] = 3;   // fmt tag -> float, bits still 16
      CHECK(!opens(v)); }                          // float!=32-bit rejected (OOB guard)
    { std::vector<uint8_t> v = base;               // absurd fmt size -> cap rejects
      v[16]=0xFF; v[17]=0xFF; v[18]=0xFF; v[19]=0xFF; CHECK(!opens(v)); }
    { std::vector<uint8_t> v = base; v[22]=0; v[23]=0; CHECK(!opens(v)); } // 0 channels
    { std::vector<uint8_t> v(8, 0); CHECK(!opens(v)); }                    // too short

    // 2. Every truncation length: cut the file at each byte boundary.
    for (size_t n = 0; n <= base.size(); n++)
        probe(std::vector<uint8_t>(base.begin(), base.begin() + n));

    // 3. Targeted malformations that hit specific guards.
    auto withU32 = [&](size_t off, uint32_t val) {
        std::vector<uint8_t> v = base;
        v[off] = val; v[off+1] = val>>8; v[off+2] = val>>16; v[off+3] = val>>24;
        return v;
    };
    auto withU16 = [&](size_t off, uint16_t val) {
        std::vector<uint8_t> v = base;
        v[off] = val; v[off+1] = val>>8; return v;
    };
    // fmt chunk starts at byte 12: [id4][size4][fmt2][ch2][rate4][byterate4][align2][bits2]
    const size_t fmtSize = 16, fmtTag = 20, fmtCh = 22, fmtBits = 34;
    probe(withU32(fmtSize, 0xFFFFFFFFu));   // absurd fmt size -> cap rejects
    probe(withU32(fmtSize, 8));             // too-small fmt size
    probe(withU16(fmtTag, 3));              // float tag but bits=16 -> reject (OOB guard)
    probe(withU16(fmtTag, 0xBEEF));         // unknown format tag
    probe(withU16(fmtCh, 0));               // zero channels
    probe(withU16(fmtCh, 0xFFFF));          // absurd channel count
    probe(withU16(fmtBits, 0));             // zero bits
    probe(withU16(fmtBits, 7));             // non-multiple-of-8 depth
    probe(withU16(fmtBits, 12));            // unsupported PCM depth
    // data chunk size field sits right after the "data" tag. Find it.
    {
        // locate "data" tag
        size_t d = 0;
        for (size_t i = 0; i + 4 <= base.size(); i++)
            if (base[i]=='d'&&base[i+1]=='a'&&base[i+2]=='t'&&base[i+3]=='a') { d = i; break; }
        probe(withU32(d + 4, 0xFFFFFFFFu));   // data far bigger than the file
        probe(withU32(d + 4, 0));             // empty data
    }
    // Corrupt magic.
    { std::vector<uint8_t> v = base; v[0] = 'X'; probe(v); }
    { std::vector<uint8_t> v = base; v[8] = 'X'; probe(v); }   // WAVE -> XAVE

    // 4. Pure garbage of assorted sizes.
    for (size_t len : { (size_t)0, (size_t)1, (size_t)4, (size_t)11, (size_t)12,
                        (size_t)44, (size_t)1000 }) {
        std::vector<uint8_t> v(len, 0x00); probe(v);
        std::vector<uint8_t> w(len, 0xFF); probe(w);
    }

    // 5. Randomized bit-flip fuzzing of the header region (deterministic seed).
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<size_t> pos(0, base.size() ? base.size() - 1 : 0);
    std::uniform_int_distribution<int> byte(0, 255);
    for (int iter = 0; iter < 3000; iter++) {
        std::vector<uint8_t> v = base;
        const int flips = 1 + (iter % 6);
        for (int k = 0; k < flips; k++) v[pos(rng)] = (uint8_t)byte(rng);
        probe(v);
    }

    std::printf("\n%d checks, %d failures (no crash/hang/OOB == pass)\n",
                g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
