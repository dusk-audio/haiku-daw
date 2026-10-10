// AiffSource — a self-contained AIFF / AIFF-C reader.
//
// The second reader behind IAudioSource (see WavSource for the house style it
// copies: clamp a declared size against the real file, reject absurd header
// fields, never size a buffer from an untrusted integer). AIFF is the
// big-endian cousin of WAV — same FORM/IFF chunking, same PCM-ish payload —
// with three things that catch people out and are handled here:
//
//   - 8-bit samples are SIGNED (WAV's are unsigned, offset by 128);
//   - a sample rate is an 80-bit IEEE extended float, not an integer;
//   - AIFF-C adds a compression fourcc. We read the uncompressed family
//     ("NONE", "twos" big-endian, "sowt" little-endian — the byte-swapped
//     "twos" Apple's own tools write) and the IEEE float family
//     ("fl32"/"FL32", "fl64"/"FL64"). Everything else is refused loudly.
//
// Kit-free (std C++ only) so it builds and unit-tests on any host. Output is
// always 2-channel interleaved float: mono is duplicated, >2 channels are
// downmixed to the first two, exactly as WavSource does.
#pragma once

#include "IAudioSource.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace daw {

class AiffSource : public IAudioSource {
public:
    AiffSource() = default;

    // Parse the header. Returns true on success.
    bool Open(const std::string& path);

    // --- IAudioSource ---
    bool    IsValid() const override { return fValid; }
    float   FrameRate() const override { return fSampleRate; }
    int     SourceChannels() const override { return fChannels; }
    int64_t TotalFrames() const override { return fTotalFrames; }
    bool    Seek(int64_t frame) override;
    bool    ReadChunk(const float** outStereo, size_t* outFrames) override;

    // The AIFF-C compression fourcc ("NONE" for a plain AIFF), for tests and
    // for anything that wants to say what it just read.
    const char* Compression() const { return fCompression; }

private:
    std::ifstream fFile;
    bool          fValid = false;

    // Parsed COMM chunk.
    char     fCompression[5]  = "NONE";   // AIFF-C fourcc; AIFF itself = NONE
    bool     fFloat           = false;    // IEEE float samples (fl32/fl64)
    bool     fLittleEndian    = false;    // sowt only
    int      fChannels        = 0;
    float    fSampleRate      = 0.0f;
    int      fBitsPerSample   = 0;
    int      fBytesPerSample  = 0;

    // SSND data bounds.
    int64_t  fDataStart   = 0;   // file offset of the first sample byte
    int64_t  fDataBytes   = 0;   // bytes of sample data actually present
    int64_t  fBytesRead   = 0;   // consumed so far
    int64_t  fTotalFrames = 0;

    std::vector<uint8_t> fRaw;     // one block of raw file bytes
    std::vector<float>   fStereo;  // converted interleaved stereo out

    size_t ReadRawBlock();
    float  SampleToFloat(const uint8_t* p) const;
};

} // namespace daw
