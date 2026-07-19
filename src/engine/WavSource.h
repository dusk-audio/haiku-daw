// WavSource — a self-contained RIFF/WAVE reader.
//
// Parses uncompressed WAV files directly (PCM 8/16/24/32-bit and 32-bit
// IEEE float) and streams them out as interleaved stereo float. It does
// NOT use the Haiku Media Kit, on purpose: some Haiku images ship without
// the media reader/decoder plugins, and a DAW wants to own its core audio
// file I/O regardless. Compressed formats (mp3/flac/...) are a later job.
//
// Kit-free (std C++ only) so it builds and unit-tests on any host.
//
// Output is always 2-channel interleaved float: mono is duplicated,
// >2 channels are downmixed to the first two. WAV data is little-endian;
// this reads it as such (fine on x86; both host and Haiku target here).
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace daw {

class WavSource {
public:
    WavSource() = default;

    // Parse the header. Returns true on success.
    bool Open(const std::string& path);

    bool    IsValid() const { return fValid; }
    float   FrameRate() const { return fSampleRate; }
    int     SourceChannels() const { return fChannels; }
    int64_t TotalFrames() const { return fTotalFrames; }

    // Reposition the read cursor to source frame `frame` (clamped to
    // [0, TotalFrames]). The next ReadChunk decodes from there. Returns false
    // if the source is invalid. Used to align a clip to a seeked playhead.
    bool Seek(int64_t frame);

    // Decode the next block. On success sets *outStereo to an internal
    // buffer of *outFrames interleaved stereo frames (2 * frames floats)
    // and returns true. Returns false at end of the data chunk.
    // The returned pointer is valid until the next ReadChunk call.
    bool ReadChunk(const float** outStereo, size_t* outFrames);

    // Decode the next block WITHOUT the stereo widening ReadChunk applies: the
    // buffer holds *outFrames * SourceChannels() interleaved floats, exactly
    // the channel layout the file stores. The sampler uses this so a mono drum
    // hit stays mono (half the memory, and the true mono signal is preserved).
    // Same streaming contract as ReadChunk otherwise; don't mix the two on one
    // WavSource, since both advance the same read cursor.
    bool ReadChunkNative(const float** outNative, size_t* outFrames);

private:
    std::ifstream fFile;
    bool     fValid       = false;

    // Parsed fmt chunk.
    uint16_t fAudioFormat = 0;   // 1=PCM, 3=float, 0xFFFE=extensible
    int      fChannels    = 0;
    float    fSampleRate  = 0.0f;
    int      fBitsPerSample = 0;
    int      fBytesPerSample = 0;

    // data chunk bounds.
    int64_t  fDataStart   = 0;   // file offset of first sample byte
    int64_t  fDataBytes   = 0;   // total bytes in the data chunk
    int64_t  fBytesRead   = 0;   // consumed so far
    int64_t  fTotalFrames = 0;

    std::vector<uint8_t> fRaw;     // one block of raw file bytes
    std::vector<float>   fStereo;  // converted interleaved stereo out
    std::vector<float>   fNative;  // converted, source channel count preserved

    // Read the next block of raw bytes into fRaw. Returns the frame count
    // decoded (0 at end of data) and advances fBytesRead. Shared by
    // ReadChunk and ReadChunkNative, which differ only in how they convert.
    size_t ReadRawBlock();

    float SampleToFloat(const uint8_t* p) const;   // one sample -> [-1,1]
};

} // namespace daw
