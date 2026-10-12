#include "PeakCache.h"

#include "../engine/IAudioSource.h"

#include <algorithm>
#include <cstring>

namespace daw {

bool PeakCache::Build(IAudioSource& src, int framesPerBucket, Cancel* cancel) {
    if (!src.IsValid() || framesPerBucket < 1)
        return false;

    fFramesPerBucket = framesPerBucket;
    fTotalFrames     = 0;
    fSampleRate      = src.FrameRate();
    fPeaks.clear();

    // Accumulator for the bucket currently being filled.
    int   inBucket = 0;
    Peak  cur;
    bool  curOpen = false;

    auto flush = [&]() {
        if (curOpen) { fPeaks.push_back(cur); curOpen = false; }
    };

    auto giveUp = [&]() {
        fFramesPerBucket = 0;      // IsValid() false: never used as an envelope
        fTotalFrames     = 0;
        fPeaks.clear();
        return false;
    };

    const float* blk = nullptr;
    size_t frames = 0;
    while (src.ReadChunk(&blk, &frames)) {
        if (cancel && cancel->Cancelled()) return giveUp();
        for (size_t f = 0; f < frames; f++) {
            // Mono-fold the stereo pair; this is what one waveform lane shows.
            const float mono = 0.5f * (blk[f * 2 + 0] + blk[f * 2 + 1]);

            if (!curOpen) { cur.min = cur.max = mono; curOpen = true; inBucket = 0; }
            cur.min = std::min(cur.min, mono);
            cur.max = std::max(cur.max, mono);

            if (++inBucket == fFramesPerBucket)
                flush();
        }
        fTotalFrames += static_cast<Frame>(frames);
    }
    flush();   // partial trailing bucket

    return true;
}

Peak PeakCache::Range(Frame startFrame, Frame endFrame) const {
    if (fFramesPerBucket <= 0 || endFrame <= startFrame)
        return Peak{};

    // Map the frame span to the buckets it touches, then combine their
    // extremes. Clamp to the buckets we actually have.
    Frame b0 = startFrame / fFramesPerBucket;
    Frame b1 = (endFrame - 1) / fFramesPerBucket;
    if (b0 < 0) b0 = 0;
    if (b1 >= static_cast<Frame>(fPeaks.size()))
        b1 = static_cast<Frame>(fPeaks.size()) - 1;
    if (b0 > b1)
        return Peak{};

    Peak out = fPeaks[b0];
    for (Frame b = b0 + 1; b <= b1; b++) {
        out.min = std::min(out.min, fPeaks[b].min);
        out.max = std::max(out.max, fPeaks[b].max);
    }
    return out;
}

// --- the on-disk envelope (M4.4) --------------------------------------------

namespace {

// The file's header: a magic, a version, the key, and the bucket count. Little
// explicit padding, written field by field, so this cannot depend on the
// compiler's struct layout: a cache written by a different build of the app is
// still read correctly (or rejected on the version).
constexpr char     kPeakMagic[4] = {'D', 'A', 'W', 'P'};
constexpr uint32_t kPeakVersion  = 1;

void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)(v & 0xffu));
    b.push_back((uint8_t)((v >> 8) & 0xffu));
    b.push_back((uint8_t)((v >> 16) & 0xffu));
    b.push_back((uint8_t)((v >> 24) & 0xffu));
}
void PutU64(std::vector<uint8_t>& b, uint64_t v) {
    PutU32(b, (uint32_t)(v & 0xffffffffu));
    PutU32(b, (uint32_t)(v >> 32));
}
void PutF32(std::vector<uint8_t>& b, float f) {
    uint32_t v = 0;
    static_assert(sizeof(float) == 4, "float is 4 bytes here");
    std::memcpy(&v, &f, 4);
    PutU32(b, v);
}
bool GetU32(const uint8_t* d, size_t n, size_t& at, uint32_t& out) {
    if (at + 4 > n) return false;
    out = (uint32_t)d[at] | ((uint32_t)d[at + 1] << 8)
        | ((uint32_t)d[at + 2] << 16) | ((uint32_t)d[at + 3] << 24);
    at += 4;
    return true;
}
bool GetU64(const uint8_t* d, size_t n, size_t& at, uint64_t& out) {
    uint32_t lo = 0, hi = 0;
    if (!GetU32(d, n, at, lo) || !GetU32(d, n, at, hi)) return false;
    out = (uint64_t)lo | ((uint64_t)hi << 32);
    return true;
}
bool GetF32(const uint8_t* d, size_t n, size_t& at, float& out) {
    uint32_t v = 0;
    if (!GetU32(d, n, at, v)) return false;
    std::memcpy(&out, &v, 4);
    return true;
}

} // namespace

std::vector<uint8_t> SerializePeakCache(const PeakCache& c,
                                        const PeakCacheId& id) {
    std::vector<uint8_t> b;
    b.insert(b.end(), kPeakMagic, kPeakMagic + 4);
    PutU32(b, kPeakVersion);
    PutU64(b, (uint64_t)id.mtime);
    PutU64(b, (uint64_t)id.size);
    PutU32(b, (uint32_t)id.framesPerBucket);
    PutU64(b, (uint64_t)c.TotalFrames());
    PutF32(b, c.SampleRate());
    PutU32(b, (uint32_t)c.BucketCount());
    for (size_t i = 0; i < c.BucketCount(); i++) {
        const Peak p = c.At(i);
        PutF32(b, p.min);
        PutF32(b, p.max);
    }
    return b;
}

bool DeserializePeakCache(const uint8_t* data, size_t n,
                          const PeakCacheId& want, PeakCache& out) {
    if (!data || n < 4) return false;
    for (int i = 0; i < 4; i++)
        if (data[i] != (uint8_t)kPeakMagic[i]) return false;
    size_t at = 4;
    uint32_t version = 0;
    if (!GetU32(data, n, at, version) || version != kPeakVersion) return false;
    uint64_t mtime = 0, size = 0, total = 0;
    uint32_t bucket = 0, count = 0;
    float rate = 0.0f;
    if (!GetU64(data, n, at, mtime) || !GetU64(data, n, at, size)
        || !GetU32(data, n, at, bucket) || !GetU64(data, n, at, total)
        || !GetF32(data, n, at, rate) || !GetU32(data, n, at, count))
        return false;
    PeakCacheId got{(int64_t)mtime, (int64_t)size, (int)bucket};
    if (got != want) return false;          // another file, or it changed
    if (count > (n - at) / 8) return false;  // truncated: not that many buckets
    std::vector<Peak> peaks;
    peaks.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        float mn = 0.0f, mx = 0.0f;
        if (!GetF32(data, n, at, mn) || !GetF32(data, n, at, mx)) return false;
        peaks.push_back(Peak{mn, mx});
    }
    out.Adopt((int)bucket, (Frame)total, rate, std::move(peaks));
    return true;
}

std::string PeakCacheFileName(const std::string& sourcePath) {
    // FNV-1a over the path, its length included so two paths that differ only in
    // trailing characters cannot land on one name.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : sourcePath) {
        h ^= (uint64_t)ch;
        h *= 1099511628211ull;
    }
    static const char* kHex = "0123456789abcdef";
    std::string name = "peak-";
    for (int i = 15; i >= 0; i--)
        name.push_back(kHex[(h >> (i * 4)) & 0xf]);
    name += ".bin";
    return name;
}

} // namespace daw
