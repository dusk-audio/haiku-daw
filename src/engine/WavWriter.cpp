#include "WavWriter.h"

#include <cstdio>

namespace daw {

namespace {
// Little-endian writers (WAV is LE).
void wr_u32(std::ofstream& f, uint32_t v) {
    uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
    f.write(reinterpret_cast<char*>(b), 4);
}
void wr_u16(std::ofstream& f, uint16_t v) {
    uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
    f.write(reinterpret_cast<char*>(b), 2);
}
} // namespace

WavWriter::~WavWriter() {
    Close();
}

bool WavWriter::Open(const std::string& path, int sampleRate, int channels) {
    if (channels < 1 || sampleRate < 1)
        return false;

    fFile.open(path, std::ios::binary | std::ios::trunc);
    if (!fFile) {
        std::fprintf(stderr, "WavWriter: cannot create '%s'\n", path.c_str());
        return false;
    }

    fChannels      = channels;
    fSampleRate    = sampleRate;
    fDataBytes     = 0;
    fFramesWritten = 0;

    const uint16_t bits      = 16;
    const uint16_t blockAlign = channels * (bits / 8);
    const uint32_t byteRate   = uint32_t(sampleRate) * blockAlign;

    // Placeholder header: RIFF + data sizes are patched in Close().
    fFile.write("RIFF", 4); wr_u32(fFile, 0);   fFile.write("WAVE", 4);
    fFile.write("fmt ", 4); wr_u32(fFile, 16);
    wr_u16(fFile, 1);                       // PCM
    wr_u16(fFile, uint16_t(channels));
    wr_u32(fFile, uint32_t(sampleRate));
    wr_u32(fFile, byteRate);
    wr_u16(fFile, blockAlign);
    wr_u16(fFile, bits);
    fFile.write("data", 4); wr_u32(fFile, 0);   // data size patched later

    fOpen = fFile.good();
    return fOpen;
}

bool WavWriter::WriteInt16(const int16_t* interleaved, size_t sampleCount) {
    if (!fOpen)
        return false;
    fFile.write(reinterpret_cast<const char*>(interleaved),
                sampleCount * sizeof(int16_t));
    if (!fFile.good())
        return false;
    fDataBytes     += int64_t(sampleCount) * sizeof(int16_t);
    fFramesWritten  = fChannels ? fDataBytes / (2 * fChannels) : 0;
    return true;
}

bool WavWriter::Close() {
    if (!fOpen)
        return true;   // idempotent
    fOpen = false;

    // Patch RIFF size (offset 4) and data size (offset 40).
    fFile.seekp(4, std::ios::beg);
    wr_u32(fFile, uint32_t(36 + fDataBytes));
    fFile.seekp(40, std::ios::beg);
    wr_u32(fFile, uint32_t(fDataBytes));

    const bool ok = fFile.good();
    fFile.close();
    return ok;
}

} // namespace daw
