#include "WavSource.h"

#include <cstdio>
#include <cstring>

namespace daw {

namespace {
// Little-endian readers (WAV is LE; x86 host + Haiku target).
uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8)
         | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t rd_u16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
} // namespace

bool WavSource::Open(const std::string& path) {
    fFile.open(path, std::ios::binary);
    if (!fFile) {
        std::fprintf(stderr, "WavSource: cannot open '%s'\n", path.c_str());
        return false;
    }

    uint8_t hdr[12];
    fFile.read(reinterpret_cast<char*>(hdr), 12);
    if (fFile.gcount() != 12
        || std::memcmp(hdr, "RIFF", 4) != 0
        || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        std::fprintf(stderr, "WavSource: not a RIFF/WAVE file\n");
        return false;
    }

    bool haveFmt = false, haveData = false;
    // Walk chunks until we have both fmt and data.
    while (fFile && !(haveFmt && haveData)) {
        uint8_t ch[8];
        fFile.read(reinterpret_cast<char*>(ch), 8);
        if (fFile.gcount() != 8)
            break;
        const uint32_t id   = rd_u32(ch);         // fourcc as bytes
        const uint32_t size = rd_u32(ch + 4);
        (void)id;

        if (std::memcmp(ch, "fmt ", 4) == 0) {
            std::vector<uint8_t> f(size);
            fFile.read(reinterpret_cast<char*>(f.data()), size);
            if (fFile.gcount() != (std::streamsize)size) break;
            fAudioFormat    = rd_u16(&f[0]);
            fChannels       = rd_u16(&f[2]);
            fSampleRate     = (float)rd_u32(&f[4]);
            fBitsPerSample  = rd_u16(&f[14]);
            fBytesPerSample = fBitsPerSample / 8;
            // WAVE_FORMAT_EXTENSIBLE: real format tag is in the subformat.
            if (fAudioFormat == 0xFFFE && size >= 26)
                fAudioFormat = rd_u16(&f[24]);
            haveFmt = true;
            if (size & 1) fFile.seekg(1, std::ios::cur);   // pad byte
        } else if (std::memcmp(ch, "data", 4) == 0) {
            fDataStart = fFile.tellg();
            fDataBytes = size;
            haveData = true;
            // Don't consume data now; ReadChunk streams it.
            break;
        } else {
            // Skip unknown chunk (+ pad to even boundary).
            fFile.seekg(size + (size & 1), std::ios::cur);
        }
    }

    if (!haveFmt || !haveData) {
        std::fprintf(stderr, "WavSource: missing fmt or data chunk\n");
        return false;
    }
    if (fChannels < 1 || fBytesPerSample < 1) {
        std::fprintf(stderr, "WavSource: bad fmt (ch=%d bits=%d)\n",
                     fChannels, fBitsPerSample);
        return false;
    }
    if (fAudioFormat != 1 && fAudioFormat != 3) {
        std::fprintf(stderr, "WavSource: unsupported format tag %u "
                     "(only PCM and float)\n", fAudioFormat);
        return false;
    }

    const int frameSize = fBytesPerSample * fChannels;
    fTotalFrames = fDataBytes / frameSize;
    fBytesRead = 0;

    fFile.seekg(fDataStart, std::ios::beg);
    fValid = true;
    return true;
}

bool WavSource::Seek(int64_t frame) {
    if (!fValid)
        return false;
    if (frame < 0) frame = 0;
    if (frame > fTotalFrames) frame = fTotalFrames;

    const int frameSize = fBytesPerSample * fChannels;
    const int64_t byteOffset = frame * frameSize;
    fFile.clear();   // drop any prior EOF/fail state before repositioning
    fFile.seekg(fDataStart + byteOffset, std::ios::beg);
    fBytesRead = byteOffset;
    return true;
}

// Convert one source sample at p to float in [-1, 1].
float WavSource::SampleToFloat(const uint8_t* p) const {
    if (fAudioFormat == 3) {                 // 32-bit IEEE float
        float v;
        std::memcpy(&v, p, 4);
        return v;
    }
    switch (fBitsPerSample) {
        case 8:   // WAV 8-bit is unsigned, midpoint 128
            return (int(p[0]) - 128) / 128.0f;
        case 16: {
            int16_t v = (int16_t)rd_u16(p);
            return v / 32768.0f;
        }
        case 24: {
            int32_t v = (int32_t(p[0]) | (int32_t(p[1]) << 8)
                        | (int32_t(p[2]) << 16));
            if (v & 0x800000) v |= ~0xFFFFFF;   // sign-extend
            return v / 8388608.0f;
        }
        case 32: {
            int32_t v = (int32_t)rd_u32(p);
            return v / 2147483648.0f;
        }
    }
    return 0.0f;
}

bool WavSource::ReadChunk(const float** outStereo, size_t* outFrames) {
    if (!fValid)
        return false;

    const int frameSize = fBytesPerSample * fChannels;
    const int64_t bytesLeft = fDataBytes - fBytesRead;
    if (bytesLeft < frameSize)
        return false;   // end of data

    const size_t kBlockFrames = 8192;
    size_t frames = bytesLeft / frameSize;
    if (frames > kBlockFrames) frames = kBlockFrames;

    const size_t rawBytes = frames * frameSize;
    if (fRaw.size() < rawBytes) fRaw.resize(rawBytes);
    if (fStereo.size() < frames * 2) fStereo.resize(frames * 2);

    fFile.read(reinterpret_cast<char*>(fRaw.data()), rawBytes);
    const std::streamsize got = fFile.gcount();
    if (got <= 0) return false;
    frames = (size_t)got / frameSize;   // in case of a short final read
    fBytesRead += frames * frameSize;

    // Convert each frame's first two channels to stereo float.
    for (size_t f = 0; f < frames; f++) {
        const uint8_t* base = fRaw.data() + f * frameSize;
        float l = SampleToFloat(base);
        float r = (fChannels == 1)
                  ? l
                  : SampleToFloat(base + fBytesPerSample);
        fStereo[f * 2 + 0] = l;
        fStereo[f * 2 + 1] = r;
    }

    *outStereo = fStereo.data();
    *outFrames = frames;
    return true;
}

} // namespace daw
