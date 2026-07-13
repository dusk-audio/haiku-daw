#include "PeakCache.h"

#include "../engine/WavSource.h"

#include <algorithm>

namespace daw {

bool PeakCache::Build(WavSource& src, int framesPerBucket) {
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

    const float* blk = nullptr;
    size_t frames = 0;
    while (src.ReadChunk(&blk, &frames)) {
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

} // namespace daw
