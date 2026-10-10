#include "AiffSource.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace daw {

namespace {

// Big-endian readers (IFF is BE; AIFF-C's `sowt` payload is the exception and
// is read with the LE readers below).
uint32_t rd_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
         | (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}
uint16_t rd_be16(const uint8_t* p) {
    return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}
uint32_t rd_le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8)
         | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t rd_le16(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}

// The 80-bit IEEE extended float AIFF stores a sample rate in (sign, 15-bit
// exponent, explicit 64-bit mantissa with no implicit leading 1). Returns 0
// for an unparseable/zero value, which the caller then rejects.
double ReadExtended80(const uint8_t* p) {
    const int      sign     = (p[0] & 0x80) ? -1 : 1;
    const uint16_t exponent = uint16_t(((p[0] & 0x7F) << 8) | p[1]);
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; i++)
        mantissa = (mantissa << 8) | p[2 + i];
    if (exponent == 0 && mantissa == 0)
        return 0.0;                       // explicit zero
    if (exponent == 0x7FFF)
        return 0.0;                       // inf / NaN: not a usable rate
    return sign * std::ldexp(static_cast<double>(mantissa),
                             int(exponent) - 16383 - 63);
}

// `size` bytes, padded to an even boundary ("chunks are word-aligned").
int64_t PaddedSize(uint32_t size) {
    return int64_t(size) + (size & 1);
}

} // namespace

bool AiffSource::Open(const std::string& path) {
    fFile.open(path, std::ios::binary);
    if (!fFile) {
        std::fprintf(stderr, "AiffSource: cannot open '%s'\n", path.c_str());
        return false;
    }

    uint8_t hdr[12];
    fFile.read(reinterpret_cast<char*>(hdr), 12);
    if (fFile.gcount() != 12
        || std::memcmp(hdr, "FORM", 4) != 0
        || (std::memcmp(hdr + 8, "AIFF", 4) != 0
            && std::memcmp(hdr + 8, "AIFC", 4) != 0)) {
        std::fprintf(stderr, "AiffSource: not a FORM/AIFF file\n");
        return false;
    }
    const bool isAifc = std::memcmp(hdr + 8, "AIFC", 4) == 0;

    // The FORM size is advisory: walk to the end of the file so a truncated or
    // over-declared form cannot send the chunk walk astray. Chunks may appear
    // in any order (IFF allows SSND before COMM), so neither one may end the
    // walk early -- SSND is recorded and skipped past, not consumed here.
    bool haveComm = false, haveSsnd = false;
    int64_t ssndOffset = 0, ssndSize = 0, declaredFrames = 0;
    while (fFile && !(haveComm && haveSsnd)) {
        uint8_t ch[8];
        fFile.read(reinterpret_cast<char*>(ch), 8);
        if (fFile.gcount() != 8)
            break;
        const uint32_t size = rd_be32(ch + 4);

        if (std::memcmp(ch, "COMM", 4) == 0) {
            // 18 = the base AIFF COMM; cap guards a corrupt huge size.
            if (size < 18 || size > 4096) break;
            std::vector<uint8_t> c(size);
            fFile.read(reinterpret_cast<char*>(c.data()), size);
            if (fFile.gcount() != (std::streamsize)size) break;

            fChannels       = rd_be16(&c[0]);
            fBitsPerSample  = rd_be16(&c[6]);
            const double sr = ReadExtended80(&c[8]);
            fSampleRate     = float(sr);
            declaredFrames  = int64_t(rd_be32(&c[2]));

            if (isAifc) {
                if (size < 22) break;             // needs a compression fourcc
                std::memcpy(fCompression, &c[18], 4);
                fCompression[4] = '\0';
            } else {
                std::memcpy(fCompression, "NONE", 5);
            }
            // Compression decides byte order and sample type. `twos` and
            // `NONE` are the same big-endian PCM; `sowt` is `twos` with every
            // 16/24/32-bit word byte-reversed; `fl32`/`FL32` and `fl64`/`FL64`
            // are IEEE float (the case difference in the fourcc is real: some
            // writers use one, some the other, for the same format).
            if (std::strcmp(fCompression, "NONE") == 0
                || std::strcmp(fCompression, "twos") == 0) {
                fFloat = false;
                fLittleEndian = false;
            } else if (std::strcmp(fCompression, "sowt") == 0) {
                fFloat = false;
                fLittleEndian = true;
            } else if (std::strcmp(fCompression, "fl32") == 0
                       || std::strcmp(fCompression, "FL32") == 0) {
                fFloat = true;
                fLittleEndian = false;
                fBitsPerSample = 32;   // the fourcc is authoritative
            } else if (std::strcmp(fCompression, "fl64") == 0
                       || std::strcmp(fCompression, "FL64") == 0) {
                fFloat = true;
                fLittleEndian = false;
                fBitsPerSample = 64;
            } else {
                std::fprintf(stderr, "AiffSource: unsupported compression '%s'\n",
                             fCompression);
                return false;
            }
            fBytesPerSample = fBitsPerSample / 8;
            haveComm = true;
            if (size & 1) fFile.seekg(1, std::ios::cur);   // pad byte
        } else if (std::memcmp(ch, "SSND", 4) == 0) {
            // offset:u32 blocksize:u32, then offset bytes to skip, then data.
            uint8_t ss[8];
            if (size < 8) break;
            fFile.read(reinterpret_cast<char*>(ss), 8);
            if (fFile.gcount() != 8) break;
            const int64_t skip  = int64_t(rd_be32(&ss[0]));
            const int64_t where = int64_t(fFile.tellg());
            // The sample data is what the chunk holds past the two fields and
            // the skip; it can never be negative.
            const int64_t avail = int64_t(size) - 8 - skip;
            ssndOffset = where + skip;
            ssndSize   = avail > 0 ? avail : 0;
            haveSsnd   = true;
            // Step over the payload (rather than stopping here) so a COMM
            // chunk written AFTER SSND is still found.
            fFile.seekg(PaddedSize(size) - 8, std::ios::cur);
        } else {
            fFile.seekg(PaddedSize(size), std::ios::cur);
        }
    }

    if (!haveComm || !haveSsnd) {
        std::fprintf(stderr, "AiffSource: missing COMM or SSND chunk\n");
        return false;
    }
    // fChannels is a raw uint16 from the file; size the block buffer from it
    // without a bound and a declared 65535 channels allocates ~2 GB before a
    // byte is read. 64 is well past any real multichannel file (same rule as
    // WavSource).
    if (fChannels < 1 || fChannels > 64 || fBytesPerSample < 1) {
        std::fprintf(stderr, "AiffSource: bad COMM (ch=%d bits=%d)\n",
                     fChannels, fBitsPerSample);
        return false;
    }
    if (!fFloat && fBitsPerSample != 8 && fBitsPerSample != 16
        && fBitsPerSample != 24 && fBitsPerSample != 32) {
        std::fprintf(stderr, "AiffSource: unsupported PCM depth %d\n",
                     fBitsPerSample);
        return false;
    }
    if (fSampleRate <= 0.0f) {
        std::fprintf(stderr, "AiffSource: invalid sample rate %g\n", fSampleRate);
        return false;
    }

    // SSND declares its size, but the file is the authority: clamp the payload
    // to the bytes really present, then to whole frames, so TotalFrames (which
    // callers size buffers from) can never exceed reality.
    fFile.clear();
    fFile.seekg(0, std::ios::end);
    const int64_t fileLen = int64_t(fFile.tellg());
    if (fileLen < 0) {
        std::fprintf(stderr, "AiffSource: cannot determine file length\n");
        return false;
    }
    if (fileLen <= ssndOffset)
        fDataBytes = 0;
    else if (ssndSize > fileLen - ssndOffset)
        fDataBytes = fileLen - ssndOffset;
    else
        fDataBytes = ssndSize;

    fDataStart = ssndOffset;
    const int frameSize = fBytesPerSample * fChannels;
    fTotalFrames = fDataBytes / frameSize;
    // COMM's numSampleFrames is a second, equally untrusted claim. Take it only
    // when it SHORTENS the payload: a file that declares fewer frames than its
    // SSND chunk holds is truncated to what it says (and one that declares more
    // is still bounded by the bytes that are really there). fDataBytes has to
    // come down with it -- TotalFrames bounds the reported length, but the READ
    // cursor is bounded by fDataBytes, so clamping only the former would play
    // the trailing bytes the header disowned.
    if (declaredFrames > 0 && declaredFrames < fTotalFrames) {
        fTotalFrames = declaredFrames;
        fDataBytes   = fTotalFrames * frameSize;
    }
    fBytesRead = 0;

    fFile.seekg(fDataStart, std::ios::beg);
    fValid = true;
    return true;
}

bool AiffSource::Seek(int64_t frame) {
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

float AiffSource::SampleToFloat(const uint8_t* p) const {
    if (fFloat) {
        if (fBitsPerSample == 32) {
            const uint32_t u = rd_be32(p);
            float v;
            std::memcpy(&v, &u, 4);
            return v;
        }
        // 64-bit: read the BE double and narrow. A finite double that does not
        // fit a float becomes an infinity, which the exporter's isfinite sweep
        // and the engine's pre-DAC guard both neutralise.
        uint64_t u = 0;
        for (int i = 0; i < 8; i++)
            u = (u << 8) | p[i];
        double d;
        std::memcpy(&d, &u, 8);
        return float(d);
    }

    switch (fBitsPerSample) {
        case 8:
            // AIFF 8-bit PCM is SIGNED (WAV's is unsigned, midpoint 128).
            return int8_t(p[0]) / 128.0f;
        case 16: {
            const int16_t v = (int16_t)(fLittleEndian ? rd_le16(p) : rd_be16(p));
            return v / 32768.0f;
        }
        case 24: {
            const uint8_t* q = p;
            uint8_t le[3];
            if (fLittleEndian) { le[0] = p[2]; le[1] = p[1]; le[2] = p[0]; q = le; }
            int32_t v = (int32_t(q[0]) << 16) | (int32_t(q[1]) << 8) | int32_t(q[2]);
            if (v & 0x800000) v |= ~0xFFFFFF;   // sign-extend
            return v / 8388608.0f;
        }
        case 32: {
            const uint32_t u = fLittleEndian ? rd_le32(p) : rd_be32(p);
            return int32_t(u) / 2147483648.0f;
        }
    }
    return 0.0f;
}

size_t AiffSource::ReadRawBlock() {
    if (!fValid)
        return 0;

    const int frameSize = fBytesPerSample * fChannels;
    const int64_t bytesLeft = fDataBytes - fBytesRead;
    if (bytesLeft < frameSize)
        return 0;   // end of data

    const size_t kBlockFrames = 8192;
    size_t frames = size_t(bytesLeft / frameSize);
    if (frames > kBlockFrames) frames = kBlockFrames;

    const size_t rawBytes = frames * size_t(frameSize);
    if (fRaw.size() < rawBytes) fRaw.resize(rawBytes);

    fFile.read(reinterpret_cast<char*>(fRaw.data()), rawBytes);
    const std::streamsize got = fFile.gcount();
    if (got <= 0) return 0;
    frames = size_t(got) / size_t(frameSize);   // a short final read
    fBytesRead += int64_t(frames) * frameSize;
    return frames;
}

bool AiffSource::ReadChunk(const float** outStereo, size_t* outFrames) {
    const size_t frames = ReadRawBlock();
    if (frames == 0)
        return false;

    const int frameSize = fBytesPerSample * fChannels;
    if (fStereo.size() < frames * 2) fStereo.resize(frames * 2);

    for (size_t f = 0; f < frames; f++) {
        const uint8_t* base = fRaw.data() + f * size_t(frameSize);
        const float l = SampleToFloat(base);
        const float r = (fChannels == 1)
                        ? l : SampleToFloat(base + fBytesPerSample);
        fStereo[f * 2 + 0] = l;
        fStereo[f * 2 + 1] = r;
    }

    *outStereo = fStereo.data();
    *outFrames = frames;
    return true;
}

} // namespace daw
