#include "WavWriter.h"

#include <cmath>
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
// RIFF/data chunk sizes are 32-bit. The RIFF size is 36 + dataBytes, so data is
// bounded by UINT32_MAX - 36; past it the size fields truncate and readers see a
// corrupt/short file. Refuse writes beyond this rather than emit a broken WAV.
constexpr int64_t kMaxWavDataBytes = int64_t(0xFFFFFFFFu) - 36;
} // namespace

WavWriter::~WavWriter() {
    Close();
}

bool WavWriter::Open(const std::string& path, int sampleRate, int channels) {
    return OpenFormat(path, sampleRate, channels, 16, false);
}

bool WavWriter::OpenFormat(const std::string& path, int sampleRate,
                           int channels, int bitsPerSample, bool floatFmt) {
    if (channels < 1 || sampleRate < 1)
        return false;
    if (floatFmt) { if (bitsPerSample != 32) return false; }
    else if (bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32)
        return false;

    fFile.open(path, std::ios::binary | std::ios::trunc);
    if (!fFile) {
        std::fprintf(stderr, "WavWriter: cannot create '%s'\n", path.c_str());
        return false;
    }

    fChannels      = channels;
    fSampleRate    = sampleRate;
    fBits          = bitsPerSample;
    fFloat         = floatFmt;
    fDataBytes     = 0;
    fFramesWritten = 0;

    const uint16_t bits       = uint16_t(bitsPerSample);
    const uint16_t blockAlign = uint16_t(channels * (bitsPerSample / 8));
    const uint32_t byteRate    = uint32_t(sampleRate) * blockAlign;

    // Placeholder header: RIFF + data sizes are patched in Close().
    fFile.write("RIFF", 4); wr_u32(fFile, 0);   fFile.write("WAVE", 4);
    fFile.write("fmt ", 4); wr_u32(fFile, 16);
    wr_u16(fFile, floatFmt ? 3 : 1);        // 1 = PCM, 3 = IEEE float
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
    // Compare by division so a huge sampleCount can't overflow when multiplied
    // by the byte width before the check (fDataBytes never exceeds the cap).
    if (sampleCount > (size_t)((kMaxWavDataBytes - fDataBytes) / 2)) {
        std::fprintf(stderr, "WavWriter: refusing write past the 4 GB WAV limit\n");
        return false;
    }
    fFile.write(reinterpret_cast<const char*>(interleaved),
                sampleCount * sizeof(int16_t));
    if (!fFile.good())
        return false;
    fDataBytes     += int64_t(sampleCount) * sizeof(int16_t);
    fFramesWritten  = fChannels ? fDataBytes / (2 * fChannels) : 0;
    return true;
}

bool WavWriter::WriteFloat(const float* interleaved, size_t sampleCount,
                           bool dither) {
    if (!fOpen)
        return false;
    const int bytesPer = fFloat ? 4 : fBits / 8;
    // Divide rather than multiply sampleCount up front (overflow-safe).
    if (sampleCount > (size_t)((kMaxWavDataBytes - fDataBytes) / bytesPer)) {
        std::fprintf(stderr, "WavWriter: refusing write past the 4 GB WAV limit\n");
        return false;
    }

    // xorshift32 -> two uniform [0,1) -> triangular PDF dither in (-1,1) LSB.
    auto tpdf = [&]() -> double {
        auto next = [&]() {
            fDitherState ^= fDitherState << 13;
            fDitherState ^= fDitherState >> 17;
            fDitherState ^= fDitherState << 5;
            return fDitherState;
        };
        const double r1 = (next() >> 8) * (1.0 / 16777216.0);
        const double r2 = (next() >> 8) * (1.0 / 16777216.0);
        return r1 - r2;
    };

    if (fFloat) {                                   // 32-bit IEEE float verbatim
        fFile.write(reinterpret_cast<const char*>(interleaved),
                    sampleCount * sizeof(float));
        if (!fFile.good()) return false;
        fDataBytes += int64_t(sampleCount) * 4;
    } else if (fBits == 16) {
        for (size_t i = 0; i < sampleCount; i++) {
            // Sanitize before scaling: a NaN/Inf sample would make lround()
            // undefined; a >1 sample would overflow. Normalize to [-1, 1] first.
            float x = interleaved[i];
            if (!std::isfinite(x)) x = 0.0f;
            else if (x >  1.0f)    x =  1.0f;
            else if (x < -1.0f)    x = -1.0f;
            double v = double(x) * 32767.0;
            if (dither) v += tpdf();
            long q = std::lround(v);
            if (q >  32767) q =  32767;
            if (q < -32768) q = -32768;
            wr_u16(fFile, uint16_t(int16_t(q)));
        }
        fDataBytes += int64_t(sampleCount) * 2;
    } else if (fBits == 24) {
        for (size_t i = 0; i < sampleCount; i++) {
            float x = interleaved[i];
            if (!std::isfinite(x)) x = 0.0f;
            else if (x >  1.0f)    x =  1.0f;
            else if (x < -1.0f)    x = -1.0f;
            double v = double(x) * 8388607.0;
            if (dither) v += tpdf();
            long q = std::lround(v);
            if (q >  8388607) q =  8388607;
            if (q < -8388608) q = -8388608;
            const uint32_t u = uint32_t(int32_t(q));
            uint8_t b[3] = { uint8_t(u), uint8_t(u >> 8), uint8_t(u >> 16) };
            fFile.write(reinterpret_cast<char*>(b), 3);
        }
        fDataBytes += int64_t(sampleCount) * 3;
    } else {                                        // 32-bit PCM
        for (size_t i = 0; i < sampleCount; i++) {
            float x = interleaved[i];
            if (!std::isfinite(x)) x = 0.0f;   // llround(NaN) is undefined
            double v = double(x) * 2147483647.0;
            if (v >  2147483647.0) v =  2147483647.0;
            if (v < -2147483648.0) v = -2147483648.0;
            const uint32_t u = uint32_t(int32_t(std::llround(v)));
            wr_u32(fFile, u);
        }
        fDataBytes += int64_t(sampleCount) * 4;
    }

    if (!fFile.good()) return false;
    fFramesWritten = fChannels ? fDataBytes / (bytesPer * fChannels) : 0;
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
