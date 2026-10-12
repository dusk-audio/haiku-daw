// Host-buildable test for PeakCache. Writes a known PCM16 stereo WAV, builds
// an envelope from it via WavSource, and checks bucket count, per-bucket
// min/max, and the Range() aggregation. No Haiku kits needed.

#include "../src/model/PeakCache.h"
#include "../src/engine/AudioFormats.h"
#include "../src/engine/WavSource.h"
#include "../src/engine/WavWriter.h"

#include <cmath>
#include <memory>
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

// A scan that is cancelled must give up EARLY (the window waits for the loader
// between passes) and must keep NOTHING: an envelope that stopped halfway is
// still IsValid(), and would draw a waveform that ends in the middle of a clip.
static void TestCancellableScan() {
    std::printf("cancellable scan:\n");
    const char* path = "/tmp/haiku_daw_peaks_cancel.wav";
    {
        WavWriter w;
        if (!w.Open(path, 48000, 2)) { CHECK(false); return; }
        std::vector<int16_t> f(48000 * 2, 4000);   // 1 s
        CHECK(w.WriteInt16(f.data(), f.size()));
        CHECK(w.Close());
    }
    struct CountCancel : public PeakCache::Cancel {
        int limit = 0, calls = 0;
        bool Cancelled() override { return ++calls > limit; }
    };

    {
        std::unique_ptr<IAudioSource> src = OpenAudioSource(path);
        CHECK(src != nullptr);
        PeakCache c;
        CountCancel cancel;
        cancel.limit = 1;                    // trip on the second chunk
        CHECK(!c.Build(*src, 256, &cancel));
        CHECK(cancel.calls >= 2);
        CHECK(!c.IsValid());                 // nothing kept
        CHECK(c.BucketCount() == 0);
        CHECK(c.Range(0, 1000).min == 0.0f);
    }
    {
        // The same scan without a cancel hook (or one that never fires) still
        // produces the whole envelope.
        std::unique_ptr<IAudioSource> src = OpenAudioSource(path);
        PeakCache c;
        CountCancel never;
        never.limit = 1000000;
        CHECK(c.Build(*src, 256, &never));
        CHECK(c.IsValid());
        CHECK(c.BucketCount() > 0);
    }
    std::remove(path);
}

// --- the on-disk envelope (M4.4) --------------------------------------------
//
// Every failure mode here is silent in the app: a stale envelope draws a
// confident waveform for audio that is no longer there, and a truncated one
// draws rubbish. So each mismatch must be a MISS (false, out untouched), and a
// real round trip must come back bit-exact.

static void TestCacheBytes() {
    std::printf("peak cache bytes:\n");

    PeakCache c;
    std::vector<Peak> buckets = {{-0.5f, 0.25f}, {0.0f, 0.0f}, {-1.0f, 1.0f},
                                 {0.125f, 0.125f}};
    c.Adopt(256, 1000, 48000.0f, buckets);

    PeakCacheId key{1700000000, 4096, 256};
    const std::vector<uint8_t> bytes = SerializePeakCache(c, key);
    CHECK(bytes.size() == 4 + 4 + 8 + 8 + 4 + 8 + 4 + 4 + 4 * 8);

    {
        PeakCache back;
        CHECK(DeserializePeakCache(bytes.data(), bytes.size(), key, back));
        CHECK(back.FramesPerBucket() == 256);
        CHECK(back.TotalFrames() == 1000);
        CHECK(back.SampleRate() == 48000.0f);
        CHECK(back.BucketCount() == buckets.size());
        for (size_t i = 0; i < buckets.size(); i++) {
            CHECK(back.At(i).min == buckets[i].min);   // bit-exact, not "near"
            CHECK(back.At(i).max == buckets[i].max);
        }
        CHECK(back.Range(0, 1000).min == -1.0f);
    }

    // AN ID THAT MOVED IS A MISS. Each field of it, one at a time: the source
    // was rewritten (mtime), replaced by another file (size), or the envelope
    // was built at another resolution.
    {
        PeakCacheId other = key;
        other.mtime++;
        PeakCache back;
        back.Adopt(1, 1, 1.0f, {{1.0f, 1.0f}});          // sentinel: must not change
        CHECK(!DeserializePeakCache(bytes.data(), bytes.size(), other, back));
        CHECK(back.TotalFrames() == 1);                   // untouched on a miss
    }
    { PeakCacheId o = key; o.size++;           PeakCache b; CHECK(!DeserializePeakCache(bytes.data(), bytes.size(), o, b)); }
    { PeakCacheId o = key; o.framesPerBucket = 512;
                                               PeakCache b; CHECK(!DeserializePeakCache(bytes.data(), bytes.size(), o, b)); }

    // A TRUNCATED OR FOREIGN FILE IS A MISS: nothing to read, not our magic, a
    // version we do not know, or a bucket count that the bytes cannot hold.
    {
        PeakCache b;
        CHECK(!DeserializePeakCache(bytes.data(), 3, key, b));
        CHECK(!DeserializePeakCache(nullptr, 0, key, b));
        std::vector<uint8_t> bad = bytes;
        bad[0] = 'X';
        CHECK(!DeserializePeakCache(bad.data(), bad.size(), key, b));
        std::vector<uint8_t> newer = bytes;
        newer[4] = 99;                                    // version 99
        CHECK(!DeserializePeakCache(newer.data(), newer.size(), key, b));
        std::vector<uint8_t> cut = bytes;
        cut.resize(bytes.size() - 4);                     // one bucket short
        CHECK(!DeserializePeakCache(cut.data(), cut.size(), key, b));
        // A count field claiming four billion buckets: without the guard this
        // reserves 32 GB before it reads a thing (and the read would have failed
        // anyway, so only the guard makes this a clean miss).
        std::vector<uint8_t> lying = bytes;
        lying[40] = lying[41] = lying[42] = lying[43] = 0xff;
        CHECK(!DeserializePeakCache(lying.data(), lying.size(), key, b));
    }

    // Names: stable for a path, different for two paths, and never a path.
    {
        const std::string a = PeakCacheFileName("/boot/home/take-1.wav");
        CHECK(a == PeakCacheFileName("/boot/home/take-1.wav"));
        CHECK(a != PeakCacheFileName("/boot/home/take-2.wav"));
        CHECK(a.find('/') == std::string::npos);
        CHECK(a.find("..") == std::string::npos);
        CHECK(!a.empty() && a.size() < 64);
        // A path with a trailing difference must not collide.
        CHECK(PeakCacheFileName("a") != PeakCacheFileName("a "));
    }

    // An envelope with no buckets (an empty source) round-trips too.
    {
        PeakCache empty;
        empty.Adopt(256, 0, 0.0f, {});
        PeakCacheId k2{1, 2, 256};
        const std::vector<uint8_t> b2 = SerializePeakCache(empty, k2);
        PeakCache back;
        CHECK(DeserializePeakCache(b2.data(), b2.size(), k2, back));
        CHECK(back.BucketCount() == 0 && back.TotalFrames() == 0);
    }
}

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

    TestCancellableScan();
    std::remove(path);
    TestCacheBytes();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
