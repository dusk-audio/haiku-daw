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

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>

namespace daw {

class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter();

    // Create the file and write a placeholder header. Returns false on error.
    // The default writes 16-bit PCM (the recorder's take format).
    bool Open(const std::string& path, int sampleRate, int channels);

    // Open for a specific sample format: bitsPerSample 16 or 24 (PCM), or 32
    // with floatFmt=true (IEEE float). Used by the exporter for higher-depth
    // bounces. Returns false on an unsupported combination.
    bool OpenFormat(const std::string& path, int sampleRate, int channels,
                    int bitsPerSample, bool floatFmt);

    bool    IsOpen() const { return fOpen; }
    // Read live from the UI thread while the disk thread writes -> atomic.
    int64_t FramesWritten() const { return fFramesWritten.load(std::memory_order_relaxed); }

    // Append `sampleCount` interleaved int16 samples (sampleCount =
    // frames * channels). Returns false if not open or the write fails.
    bool WriteInt16(const int16_t* interleaved, size_t sampleCount);

    // Append `sampleCount` interleaved float samples, converting to the writer's
    // configured format: 16/24-bit PCM (TPDF-dithered at the LSB when `dither`)
    // or 32-bit float (verbatim). Clamps PCM to [-1,1].
    bool WriteFloat(const float* interleaved, size_t sampleCount, bool dither);

    // Patch the header sizes and close. Safe to call twice / on a closed
    // writer. Returns false if the finalize seek/write failed.
    bool Close();

private:
    std::ofstream fFile;
    bool     fOpen        = false;
    int      fChannels    = 0;
    int      fSampleRate  = 0;
    int      fBits        = 16;      // bits per sample (16/24/32)
    bool     fFloat       = false;   // true = IEEE float (fBits must be 32)
    int64_t  fDataBytes   = 0;
    uint32_t fDitherState = 0x1234567u;  // xorshift PRNG for TPDF dither
    std::atomic<int64_t> fFramesWritten{0};
};

} // namespace daw
