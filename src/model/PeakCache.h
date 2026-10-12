// PeakCache — a precomputed min/max waveform envelope.
//
// A DAW must never scan a whole audio file to repaint a waveform. Instead we
// build this cache once (on import) by scanning the source top to bottom and
// recording, per fixed-size bucket of frames, the minimum and maximum sample
// amplitude in that bucket. Painting a waveform then reads a handful of
// buckets per pixel column — O(pixels), not O(samples).
//
// Kit-free (STL only): builds and unit-tests on any host. The UI layer maps
// pixel columns to frame ranges and calls Range() to get the min/max to draw.
//
// The envelope is mono: each bucket summarises the average of the source's
// stereo channels, which is what a single waveform lane shows. Amplitudes are
// in [-1, 1] float, matching IAudioSource output.
#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace daw {

class IAudioSource;

// One bucket's vertical extent. An empty/never-written bucket reads back as
// {0, 0} (a flat line), which is the correct thing to draw for silence.
struct Peak {
    float min = 0.0f;
    float max = 0.0f;
};

class PeakCache {
public:
    PeakCache() = default;

    // A scan that can be stopped. The loader's projects change under it (a load
    // while an older pass is still reading), and a pass must give up promptly —
    // the window waits for it in order to start the next one.
    class Cancel {
    public:
        virtual ~Cancel() = default;
        virtual bool Cancelled() = 0;   // asked once per decoded chunk
    };

    // Scan `src` (from wherever its read cursor is — normally the start) to
    // end of data, filling the envelope at the given bucket size. Returns
    // false if src is invalid, framesPerBucket < 1, or `cancel` asked to stop —
    // in which case NOTHING is kept: a half-scanned envelope that still looked
    // valid would draw a waveform that stops in the middle of the clip.
    // Consumes the source's forward read cursor, so build once, before
    // streaming for playback.
    bool Build(IAudioSource& src, int framesPerBucket = 256,
               Cancel* cancel = nullptr);

    bool   IsValid() const { return fFramesPerBucket > 0; }
    int    FramesPerBucket() const { return fFramesPerBucket; }
    size_t BucketCount() const { return fPeaks.size(); }
    Frame  TotalFrames() const { return fTotalFrames; }
    // Source sample rate the envelope was built from (0 if unknown). The UI
    // needs it to map timeline frames (output rate) to source frames.
    float  SampleRate() const { return fSampleRate; }

    // The bucket at index i (clamped-empty {0,0} if out of range).
    Peak At(size_t i) const {
        return i < fPeaks.size() ? fPeaks[i] : Peak{};
    }

    // Take an envelope built elsewhere (the loader, reading a cache file): the
    // other way into this class, beside Build(). The caller has already checked
    // that it belongs to the source it is about to be used for.
    void Adopt(int framesPerBucket, Frame totalFrames, float sampleRate,
               std::vector<Peak> peaks) {
        fFramesPerBucket = framesPerBucket;
        fTotalFrames     = totalFrames;
        fSampleRate      = sampleRate;
        fPeaks           = std::move(peaks);
    }

    // Aggregate min/max over the frame span [startFrame, endFrame). This is
    // the paint-time entry point: pass the frame range that a single pixel
    // column covers and get the extremes to draw. Spans covering many buckets
    // (zoomed out) combine them; a sub-bucket span returns its bucket. Out-of
    // range or empty spans return {0, 0}.
    Peak Range(Frame startFrame, Frame endFrame) const;

private:
    int                fFramesPerBucket = 0;
    Frame              fTotalFrames     = 0;
    float              fSampleRate      = 0.0f;
    std::vector<Peak>  fPeaks;
};

// ---------------------------------------------------------------------------
// The on-disk envelope (M4.4).
//
// A project's waveforms should not be re-scanned every time it is opened, and a
// cache that draws the WRONG waveform is worse than no cache at all — so what a
// cached envelope is valid for is recorded with it and checked before it is
// used. Paths get reused (a take is re-recorded, a file is replaced, another
// editor rewrites it in place); size and modification time are what tell those
// apart.
//
// Kit-free: the bytes are a vector, the caller does the file I/O. Everything
// here is host-tested, because every failure mode is silent — a stale envelope
// draws confidently, and a truncated one draws garbage.
// ---------------------------------------------------------------------------
// What a cached envelope is valid for: WHICH FILE it describes, and in what
// bucket size. That is all a caller can know without opening the audio (a stat
// gives the first two), so that is all that is compared — a hit costs one stat,
// not a file open. The envelope's own frame count and sample rate travel with it
// as data.
struct PeakCacheId {
    int64_t mtime           = 0;   // source file's modification time (0 = unknown)
    int64_t size            = 0;   // its size in bytes
    int     framesPerBucket = 0;

    bool operator==(const PeakCacheId& o) const {
        return mtime == o.mtime && size == o.size
               && framesPerBucket == o.framesPerBucket;
    }
    bool operator!=(const PeakCacheId& o) const { return !(*this == o); }
};

// The bytes to write for this envelope and id.
std::vector<uint8_t> SerializePeakCache(const PeakCache& c, const PeakCacheId& id);

// Read them back, but only if they hold a COMPLETE envelope for exactly `want`.
// False leaves `out` untouched, and every caller treats false as "build it
// again": a short buffer, a wrong magic, a newer version, another file's
// envelope and a source that changed all land here.
bool DeserializePeakCache(const uint8_t* data, size_t n,
                          const PeakCacheId& want, PeakCache& out);

// The cache file's name for a source path: a hash of the path (hex), so a path
// can never escape the cache directory and two paths cannot share one file.
std::string PeakCacheFileName(const std::string& sourcePath);

} // namespace daw
