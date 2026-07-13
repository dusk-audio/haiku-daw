// WavWriter — a self-contained RIFF/WAVE writer for recorded takes.
//
// Streams interleaved PCM to disk incrementally: Open() writes a placeholder
// header, WriteInt16() appends sample blocks as they arrive, Close() seeks
// back and patches the RIFF/data chunk sizes. The counterpart to WavSource;
// like it, kit-free (std C++ only) so it builds and unit-tests on any host.
//
// v1 writes 16-bit PCM, which is what the Haiku HD Audio input hands us
// (negotiated int16 stereo). Little-endian on disk (x86 host + Haiku target).
#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace daw {

class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter();

    // Create the file and write a placeholder header. Returns false on error.
    bool Open(const std::string& path, int sampleRate, int channels);

    bool    IsOpen() const { return fOpen; }
    int64_t FramesWritten() const { return fFramesWritten; }

    // Append `sampleCount` interleaved int16 samples (sampleCount =
    // frames * channels). Returns false if not open or the write fails.
    bool WriteInt16(const int16_t* interleaved, size_t sampleCount);

    // Patch the header sizes and close. Safe to call twice / on a closed
    // writer. Returns false if the finalize seek/write failed.
    bool Close();

private:
    std::ofstream fFile;
    bool     fOpen        = false;
    int      fChannels    = 0;
    int      fSampleRate  = 0;
    int64_t  fDataBytes   = 0;
    int64_t  fFramesWritten = 0;
};

} // namespace daw
