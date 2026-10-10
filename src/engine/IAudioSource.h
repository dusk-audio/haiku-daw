// IAudioSource — the one reader interface every audio file format implements.
//
// WavSource was the only reader for a long time, so its shape IS the contract:
// open (done by the factory, not here), report the source's rate/channels/frame
// count, reposition the cursor to a source frame, and stream interleaved stereo
// float blocks. AIFF/AIFC, FLAC and Ogg Vorbis implement the same surface, so
// the engine, the peak cache, the exporter and the waveform drawing never learn
// what container they are reading.
//
// Kit-free (std C++ only). Nothing here may touch the Media Kit: the target
// image ships no reader/decoder plugins, so `BMediaFile` cannot open a WAV, let
// alone a FLAC (docs/HANDOFF.md). Formats are our own parsers or bundled
// libraries, reached through src/engine/AudioFormats.h.
#pragma once

#include <cstddef>
#include <cstdint>

namespace daw {

class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    // True once the header parsed and the source is streamable. A source that
    // failed to open is never handed out by the factory, but the flag is what
    // every consumer checks first anyway (the sampler, the peak cache).
    virtual bool    IsValid() const = 0;

    // The file's own sample rate and channel count (BEFORE any stereo
    // widening/downmix ReadChunk applies). 0 if unknown.
    virtual float   FrameRate() const = 0;
    virtual int     SourceChannels() const = 0;

    // Total decodable frames, clamped to what the file really holds. Callers
    // size buffers and clip lengths from this, so it must never exceed reality.
    virtual int64_t TotalFrames() const = 0;

    // Reposition the read cursor to source frame `frame` (clamped to
    // [0, TotalFrames]). The next ReadChunk decodes from there. Returns false
    // if the source is invalid. Used to align a clip with a seeked playhead —
    // the engine calls it on every Load.
    virtual bool Seek(int64_t frame) = 0;

    // Decode the next block. On success sets *outStereo to an internal buffer
    // of *outFrames interleaved stereo frames (2 * frames floats) and returns
    // true. Returns false at end of data. The returned pointer is valid until
    // the next ReadChunk call.
    virtual bool ReadChunk(const float** outStereo, size_t* outFrames) = 0;
};

} // namespace daw
