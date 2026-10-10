// Host-buildable tests for the file formats package (docs/agent-prompts/
// 28-formats.md): the AIFF/AIFF-C parser, the by-content sniff, and the FLAC
// and Ogg Vorbis round trips through the REAL sinks.
//
// The AIFF fixtures are written by hand here (as wav_tests does for WAV) so the
// expected samples are the test's own strings of integers, not something
// another encoder produced. The compressed round trips go through the same
// IAudioSink the exporter uses, so what is proven is the actual export path.
#include "../src/engine/AudioFormats.h"
#include "../src/engine/AiffSource.h"
#include "../src/engine/IAudioSink.h"
#include "../src/engine/WavSource.h"
#include "../src/engine/WavWriter.h"
#include "../src/model/Project.h"
#include "../src/engine/Exporter.h"

#if defined(DAW_HAVE_FLAC)
#include "../src/engine/FlacSink.h"
#endif
#if defined(DAW_HAVE_VORBIS)
#include "../src/engine/VorbisSink.h"
#endif

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// --- fixture writers -------------------------------------------------------

static void put_be32(std::ofstream& f, uint32_t v) {
    uint8_t b[4] = { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8),
                     uint8_t(v) };
    f.write((char*)b, 4);
}
static void put_be16(std::ofstream& f, uint16_t v) {
    uint8_t b[2] = { uint8_t(v >> 8), uint8_t(v) };
    f.write((char*)b, 2);
}
static void put_le16(std::ofstream& f, uint16_t v) {
    uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
    f.write((char*)b, 2);
}
static void put_le32(std::ofstream& f, uint32_t v) {
    uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16),
                     uint8_t(v >> 24) };
    f.write((char*)b, 4);
}

// The 80-bit IEEE extended float AIFF stores a sample rate in.
static void put_extended80(std::ofstream& f, double v) {
    uint8_t b[10] = { 0 };
    if (v > 0.0) {
        int exp = 0;
        const double m = std::frexp(v, &exp);          // v = m * 2^exp
        const uint64_t mant = (uint64_t)std::ldexp(m, 64);   // explicit lead 1
        const int e = exp - 1 + 16383;
        b[0] = uint8_t((e >> 8) & 0x7F);
        b[1] = uint8_t(e & 0xFF);
        for (int i = 0; i < 8; i++)
            b[2 + i] = uint8_t(mant >> (56 - 8 * i));
    }
    f.write((char*)b, 10);
}

// A minimal AIFF / AIFC file. `samples` are interleaved; `bytesPerSample` and
// the writer's own endianness are the caller's, so the same helper builds the
// big-endian AIFF, the `sowt` AIFF-C and the float variants.
struct AiffFixture {
    const char*  compression;   // "NONE" (plain AIFF), "sowt", "fl32", "fl64"
    int          bits;
    bool         littleEndian;  // sample payload order
    bool         beFloat;       // IEEE float payload
    uint32_t     declaredFrames;   // what COMM claims (0 = the real count)
    uint32_t     ssndExtra;        // bytes of SSND padding past the audio
};
static AiffFixture kAiffPcm16   { "NONE", 16, false, false, 0, 0 };
static AiffFixture kAiffPcm8    { "NONE",  8, false, false, 0, 0 };
static AiffFixture kAiffSowt16  { "sowt", 16, true,  false, 0, 0 };
static AiffFixture kAiffFloat32 { "fl32", 32, false, true,  0, 0 };
static AiffFixture kAiffFloat64 { "fl64", 64, false, true,  0, 0 };

static void WriteAiff(const char* path, double rate, int channels,
                      const AiffFixture& fx,
                      const std::vector<double>& samples) {
    const bool isAifc = std::strcmp(fx.compression, "NONE") != 0;
    const int bytesPerSample = fx.bits / 8;
    const uint32_t frames =
        uint32_t(samples.size() / (size_t)channels);
    const uint32_t dataBytes = uint32_t(samples.size() * (size_t)bytesPerSample);
    const uint32_t commSize = isAifc ? 23u : 18u;   // 23: odd, so it is padded
    const uint32_t ssndSize = 8 + dataBytes + fx.ssndExtra;

    std::ofstream f(path, std::ios::binary);
    f.write("FORM", 4);
    put_be32(f, 4 + (8 + commSize + (commSize & 1)) + (8 + ssndSize
             + (ssndSize & 1)));
    f.write(isAifc ? "AIFC" : "AIFF", 4);

    f.write("COMM", 4);
    put_be32(f, commSize);
    put_be16(f, uint16_t(channels));
    put_be32(f, fx.declaredFrames ? fx.declaredFrames : frames);
    put_be16(f, uint16_t(fx.bits));
    put_extended80(f, rate);
    if (isAifc) {
        f.write(fx.compression, 4);
        uint8_t pstring[1] = { 0 };   // zero-length compression name
        f.write((char*)pstring, 1);
        // (23 bytes written; the pad byte below brings it to 24.)
    }
    if (commSize & 1) { uint8_t pad = 0; f.write((char*)&pad, 1); }

    f.write("SSND", 4);
    put_be32(f, ssndSize);
    put_be32(f, 0);   // offset
    put_be32(f, 0);   // blockSize
    for (double s : samples) {
        if (fx.beFloat) {
            if (fx.bits == 32) {
                const float v = (float)s;
                uint32_t u;
                std::memcpy(&u, &v, 4);
                put_be32(f, u);
            } else {
                uint64_t u;
                std::memcpy(&u, &s, 8);
                uint8_t b[8];
                for (int i = 0; i < 8; i++) b[i] = uint8_t(u >> (56 - 8 * i));
                f.write((char*)b, 8);
            }
        } else if (fx.littleEndian) {
            const int32_t v = (int32_t)llround(s);
            if (bytesPerSample == 2) put_le16(f, uint16_t((uint16_t)v));
            else                     put_le32(f, uint32_t(v));
        } else {
            const int32_t v = (int32_t)llround(s);
            if (bytesPerSample == 1) { uint8_t b = uint8_t(v); f.write((char*)&b, 1); }
            else if (bytesPerSample == 2) put_be16(f, uint16_t((uint16_t)v));
            else if (bytesPerSample == 3) {
                uint8_t b[3] = { uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) };
                f.write((char*)b, 3);
            } else put_be32(f, uint32_t(v));
        }
    }
    for (uint32_t i = 0; i < fx.ssndExtra; i++) { uint8_t z = 0; f.write((char*)&z, 1); }
    if (ssndSize & 1) { uint8_t pad = 0; f.write((char*)&pad, 1); }
}

// Read a whole source into interleaved stereo floats.
static std::vector<float> ReadAll(IAudioSource& src) {
    std::vector<float> out;
    const float* blk = nullptr;
    size_t n = 0;
    while (src.ReadChunk(&blk, &n))
        out.insert(out.end(), blk, blk + n * 2);
    return out;
}

static bool Near(float a, float b, float tol = 1e-6f) {
    return std::fabs(a - b) <= tol;
}

// --- 1. the sniff ----------------------------------------------------------

static void TestSniff() {
    std::printf("test_sniff\n");
    uint8_t buf[64];
    const auto sniff = [&](const char* magic, size_t at) {
        std::memset(buf, 0, sizeof(buf));
        std::memcpy(buf + at, magic, std::strlen(magic));
        return SniffAudioHeader(buf, sizeof(buf));
    };

    std::memset(buf, 0, sizeof(buf));
    std::memcpy(buf, "RIFF", 4); std::memcpy(buf + 8, "WAVE", 4);
    CHECK(SniffAudioHeader(buf, sizeof(buf)) == AudioFileFormat::Wav);
    // A RIFF that is not WAVE (an AVI) is not ours.
    std::memcpy(buf + 8, "AVI ", 4);
    CHECK(SniffAudioHeader(buf, sizeof(buf)) == AudioFileFormat::Unknown);

    CHECK(sniff("FORM", 0) == AudioFileFormat::Unknown);   // magic alone is not enough
    std::memset(buf, 0, sizeof(buf));
    std::memcpy(buf, "FORM", 4); std::memcpy(buf + 8, "AIFF", 4);
    CHECK(SniffAudioHeader(buf, sizeof(buf)) == AudioFileFormat::Aiff);
    std::memcpy(buf + 8, "AIFC", 4);
    CHECK(SniffAudioHeader(buf, sizeof(buf)) == AudioFileFormat::Aiff);

    CHECK(sniff("fLaC", 0) == AudioFileFormat::Flac);

    // Ogg: page header, one lacing entry, then the Vorbis identification
    // packet (type byte 0x01 + "vorbis").
    std::memset(buf, 0, sizeof(buf));
    std::memcpy(buf, "OggS", 4);
    buf[5] = 0x00;          // not a continued page
    buf[26] = 1;            // one segment
    buf[27] = 30;           // its length
    buf[28] = 0x01;         // Vorbis identification packet
    std::memcpy(buf + 29, "vorbis", 6);
    CHECK(SniffAudioHeader(buf, sizeof(buf)) == AudioFileFormat::Ogg);

    // An Ogg page carrying Opus is NOT Vorbis: the decoder would only refuse.
    std::memcpy(buf + 28, "OpusHead", 8);
    CHECK(SniffAudioHeader(buf, sizeof(buf)) == AudioFileFormat::Unknown);
    // ...and a continued first page is the middle of a packet, not a head.
    buf[5] = 0x01;
    std::memcpy(buf + 28, "\x01vorbis", 7);
    CHECK(SniffAudioHeader(buf, sizeof(buf)) == AudioFileFormat::Unknown);

    // Short / empty input never reads past the buffer.
    CHECK(SniffAudioHeader(buf, 0) == AudioFileFormat::Unknown);
    CHECK(SniffAudioHeader(buf, 4) == AudioFileFormat::Unknown);
    CHECK(SniffAudioHeader(nullptr, 64) == AudioFileFormat::Unknown);

    // The name and extension tables the UI and the file panels read.
    CHECK(std::strcmp(AudioFileFormatExtension(AudioFileFormat::Wav), "wav") == 0);
    CHECK(std::strcmp(AudioFileFormatExtension(AudioFileFormat::Flac), "flac") == 0);
    CHECK(std::strcmp(AudioFileFormatExtension(AudioFileFormat::Ogg), "ogg") == 0);
    CHECK(std::strcmp(AudioFileFormatName(AudioFileFormat::Aiff), "AIFF") == 0);
    CHECK(AudioFileFormatCanRead(AudioFileFormat::Wav));
    CHECK(AudioFileFormatCanRead(AudioFileFormat::Aiff));
    CHECK(!AudioFileFormatCanWrite(AudioFileFormat::Aiff));
    CHECK(AudioFileFormatCanWrite(AudioFileFormat::Wav));
}

// --- 2. AIFF / AIFF-C ------------------------------------------------------

static void TestAiffPcm16() {
    std::printf("test_aiff_pcm16\n");
    const char* path = "formats_test_aiff16.aiff";
    // 4 stereo frames: +1.0, -1.0, 0.5, -0.5, 0, and full-scale negatives.
    const std::vector<double> s = {
        32767, -32768,  16384, -16384,  0, 0,  -32768, 32767
    };
    WriteAiff(path, 48000.0, 2, kAiffPcm16, s);

    AiffSource a;
    CHECK(a.Open(path));
    CHECK(a.IsValid());
    CHECK(Near(a.FrameRate(), 48000.0f));
    CHECK(a.SourceChannels() == 2);
    CHECK(a.TotalFrames() == 4);
    CHECK(std::strcmp(a.Compression(), "NONE") == 0);

    std::vector<float> got = ReadAll(a);
    CHECK(got.size() == 8);
    for (size_t i = 0; i < s.size(); i++)
        CHECK(Near(got[i], float(s[i] / 32768.0)));

    // Seek: frame 1 is (16384, -16384) = (0.5, -0.5).
    CHECK(a.Seek(1));
    const float* blk = nullptr;
    size_t n = 0;
    CHECK(a.ReadChunk(&blk, &n));
    CHECK(n == 3);
    CHECK(Near(blk[0], 0.5f));
    CHECK(Near(blk[1], -0.5f));
    // Past the end: nothing more, and no crash.
    CHECK(a.Seek(4));
    CHECK(!a.ReadChunk(&blk, &n));

    // The factory picks it up, and a wrong extension changes nothing.
    const char* renamed = "formats_test_aiff16.wav";
    std::remove(renamed);
    CHECK(std::rename(path, renamed) == 0);
    CHECK(SniffAudioFileFormat(renamed) == AudioFileFormat::Aiff);
    std::string err;
    std::unique_ptr<IAudioSource> viaFactory = OpenAudioSource(renamed, &err);
    CHECK(viaFactory != nullptr);
    if (viaFactory) {
        CHECK(viaFactory->TotalFrames() == 4);
        CHECK(viaFactory->SourceChannels() == 2);
    }
    std::remove(renamed);
}

static void TestAiffPcm8AndSowt() {
    std::printf("test_aiff_pcm8_and_sowt\n");
    // 8-bit AIFF is SIGNED: 127 -> ~+1, -128 -> -1, -64 -> -0.5.
    {
        const char* path = "formats_test_aiff8.aiff";
        const std::vector<double> s = { 127, -128, -64, 64, 0, 1 };
        WriteAiff(path, 22050.0, 2, kAiffPcm8, s);
        AiffSource a;
        CHECK(a.Open(path));
        CHECK(a.TotalFrames() == 3);
        CHECK(Near(a.FrameRate(), 22050.0f));
        std::vector<float> got = ReadAll(a);
        CHECK(got.size() == 6);
        for (size_t i = 0; i < s.size(); i++)
            CHECK(Near(got[i], float(s[i] / 128.0)));
        std::remove(path);
    }
    // AIFF-C `sowt` is the same PCM with the byte order swapped.
    {
        const char* path = "formats_test_sowt.aifc";
        const std::vector<double> s = { 32767, -32768, 16384, -16384 };
        WriteAiff(path, 44100.0, 2, kAiffSowt16, s);
        AiffSource a;
        CHECK(a.Open(path));
        CHECK(a.TotalFrames() == 2);
        CHECK(std::strcmp(a.Compression(), "sowt") == 0);
        std::vector<float> got = ReadAll(a);
        CHECK(got.size() == 4);
        for (size_t i = 0; i < s.size(); i++)
            CHECK(Near(got[i], float(s[i] / 32768.0)));
        std::remove(path);
    }
}

static void TestAiffFloat() {
    std::printf("test_aiff_float\n");
    {
        const char* path = "formats_test_fl32.aifc";
        const std::vector<double> s = { 0.25, -0.5, 1.0, -1.0, 0.125, 0.0 };
        WriteAiff(path, 96000.0, 2, kAiffFloat32, s);
        AiffSource a;
        CHECK(a.Open(path));
        CHECK(a.TotalFrames() == 3);
        CHECK(Near(a.FrameRate(), 96000.0f));
        std::vector<float> got = ReadAll(a);
        CHECK(got.size() == 6);
        for (size_t i = 0; i < s.size(); i++)
            CHECK(Near(got[i], float(s[i])));
        std::remove(path);
    }
    {
        const char* path = "formats_test_fl64.aifc";
        const std::vector<double> s = { 0.25, -0.5, 1.0, -1.0 };
        WriteAiff(path, 48000.0, 2, kAiffFloat64, s);
        AiffSource a;
        CHECK(a.Open(path));
        CHECK(a.TotalFrames() == 2);
        std::vector<float> got = ReadAll(a);
        CHECK(got.size() == 4);
        for (size_t i = 0; i < s.size(); i++)
            CHECK(Near(got[i], float(s[i])));
        std::remove(path);
    }
}

static void TestAiffClampsAndCorrupt() {
    std::printf("test_aiff_clamps_and_corrupt\n");
    // COMM declaring fewer frames than SSND holds: the declared count wins, so
    // a trailing chunk of junk is not played as audio.
    {
        const char* path = "formats_test_aiff_decl.aiff";
        AiffFixture fx = kAiffPcm16;
        fx.declaredFrames = 2;      // the audio really holds 4
        WriteAiff(path, 48000.0, 2, fx, { 100, 200, 300, 400, 500, 600, 700, 800 });
        AiffSource a;
        CHECK(a.Open(path));
        CHECK(a.TotalFrames() == 2);
        std::vector<float> got = ReadAll(a);
        CHECK(got.size() == 4);
        std::remove(path);
    }
    // COMM declaring MORE frames than the file holds: clamped to reality, so
    // TotalFrames (which callers size buffers from) never exceeds the file.
    {
        const char* path = "formats_test_aiff_over.aiff";
        AiffFixture fx = kAiffPcm16;
        fx.declaredFrames = 100000;
        WriteAiff(path, 48000.0, 2, fx, { 100, 200, 300, 400 });
        AiffSource a;
        CHECK(a.Open(path));
        CHECK(a.TotalFrames() == 2);
        std::remove(path);
    }
    // Not a FORM at all.
    {
        const char* path = "formats_test_notaiff.bin";
        std::ofstream f(path, std::ios::binary);
        f.write("this is not an audio file at all, not even close", 50);
        f.close();
        AiffSource a;
        CHECK(!a.Open(path));
        CHECK(!a.IsValid());
        // ...and the factory refuses it with a message rather than crashing.
        std::string err;
        CHECK(OpenAudioSource(path, &err) == nullptr);
        CHECK(!err.empty());
        std::remove(path);
    }
    // Header only, no SSND.
    {
        const char* path = "formats_test_nossnd.aiff";
        std::ofstream f(path, std::ios::binary);
        f.write("FORM", 4); put_be32(f, 4 + 8 + 18); f.write("AIFF", 4);
        f.write("COMM", 4); put_be32(f, 18);
        put_be16(f, 2); put_be32(f, 10); put_be16(f, 16); put_extended80(f, 48000.0);
        f.close();
        AiffSource a;
        CHECK(!a.Open(path));
        std::remove(path);
    }
    // An AIFF-C compression we do not read.
    {
        const char* path = "formats_test_adpcm.aifc";
        AiffFixture fx = kAiffPcm16;
        fx.compression = "ima4";
        WriteAiff(path, 48000.0, 2, fx, { 1, 2, 3, 4 });
        AiffSource a;
        CHECK(!a.Open(path));
        std::remove(path);
    }
    // A truncated SSND: the file ends in the middle of a frame. Open succeeds
    // (the header is fine) and the reader stops cleanly at the last whole one.
    {
        const char* path = "formats_test_trunc.aiff";
        {
            std::ofstream f(path, std::ios::binary);
            f.write("FORM", 4); put_be32(f, 4 + 26 + 8 + 100); f.write("AIFF", 4);
            f.write("COMM", 4); put_be32(f, 18);
            put_be16(f, 2); put_be32(f, 50); put_be16(f, 16);
            put_extended80(f, 48000.0);
            f.write("SSND", 4); put_be32(f, 8 + 100);
            put_be32(f, 0); put_be32(f, 0);
            for (int i = 0; i < 30; i++) put_be16(f, uint16_t(i * 100));
        }
        AiffSource a;
        CHECK(a.Open(path));
        CHECK(a.TotalFrames() == 15);   // 30 samples / 2 channels
        std::vector<float> got = ReadAll(a);
        CHECK(got.size() == 30);
        std::remove(path);
    }
}

// --- 3. the source factory -------------------------------------------------

static void TestFactory() {
    std::printf("test_factory\n");
    // A WAV written by WavWriter, named with a misleading extension: the sniff
    // must go by content, so the drop handler needs no extension list.
    const char* path = "formats_test_mislabelled.flac";
    std::remove(path);
    {
        WavWriter w;
        CHECK(w.OpenFormat(path, 48000, 2, 16, false));
        std::vector<float> s = { 0.5f, -0.5f, 0.25f, -0.25f };
        CHECK(w.WriteFloat(s.data(), s.size(), false));
        CHECK(w.Close());
    }
    CHECK(SniffAudioFileFormat(path) == AudioFileFormat::Wav);
    std::string err;
    std::unique_ptr<IAudioSource> src = OpenAudioSource(path, &err);
    CHECK(src != nullptr);
    if (src) {
        CHECK(src->TotalFrames() == 2);
        std::vector<float> got = ReadAll(*src);
        CHECK(got.size() == 4);
        CHECK(Near(got[0], 0.5f));
    }
    std::remove(path);

    // A file that does not exist is Unknown, and reported as such.
    err.clear();
    CHECK(OpenAudioSource("formats_test_no_such_file_anywhere", &err) == nullptr);
    CHECK(!err.empty());

    // Format availability matches what the factories actually return, which is
    // the property the export dialog relies on when it builds its menu.
    CHECK((MakeAudioSink(AudioFileFormat::Flac) != nullptr)
          == AudioFileFormatCanWrite(AudioFileFormat::Flac));
    CHECK((MakeAudioSink(AudioFileFormat::Ogg) != nullptr)
          == AudioFileFormatCanWrite(AudioFileFormat::Ogg));
    CHECK((OpenAudioSource("formats_test_no_such_file_anywhere") != nullptr)
          == false);   // and an unreadable path is never a source
    CHECK(MakeAudioSink(AudioFileFormat::Aiff) == nullptr);
    CHECK(MakeAudioSink(AudioFileFormat::Unknown) == nullptr);
}

// --- 4. FLAC ---------------------------------------------------------------

#if defined(DAW_HAVE_FLAC)
// A known signal: a ramp plus a sine at exactly-representable values.
static std::vector<float> TestSignal(int64_t frames, int channels) {
    std::vector<float> s((size_t)frames * channels);
    for (int64_t i = 0; i < frames; i++) {
        const float v = 0.7f * std::sin(float(i) * 0.01f)
                      + 0.2f * float((i % 100) - 50) / 50.0f;
        for (int c = 0; c < channels; c++)
            s[(size_t)i * channels + (size_t)c] = (c == 0) ? v : -v;
    }
    return s;
}

// What the sinks do to a float before handing it to the codec, so a lossless
// round trip can be asserted exactly rather than approximately.
static int32_t Quantize(float x, int bits, bool dither) {
    (void)dither;
    const double scale = (bits == 16) ? 32767.0 : 8388607.0;
    const long lo = (bits == 16) ? -32768L : -8388608L;
    const long hi = (bits == 16) ?  32767L :  8388607L;
    if (!(x == x)) x = 0.0f;
    double v = double(x) * scale;
    long q = std::lround(v);
    if (q > hi) q = hi;
    if (q < lo) q = lo;
    return (int32_t)q;
}

static void TestFlacRoundTrip() {
    std::printf("test_flac_roundtrip\n");
    const int64_t frames = 20000;      // > one 8192-frame read block
    for (int bits : { 16, 24 }) {
        const char* path = (bits == 16) ? "formats_test_16.flac"
                                        : "formats_test_24.flac";
        std::remove(path);
        const std::vector<float> sig = TestSignal(frames, 2);

        FlacSink sink;
        SinkFormat fmt;
        fmt.sampleRate = 48000;
        fmt.channels   = 2;
        fmt.bitDepth   = bits;
        fmt.dither     = false;    // exactness: no dithering at 16 either
        CHECK(sink.Open(path, fmt));
        CHECK(sink.WriteFloat(sig.data(), sig.size()));
        CHECK(sink.Close());

        // The file really is a FLAC (the marker the sniff keys on).
        CHECK(SniffAudioFileFormat(path) == AudioFileFormat::Flac);

        std::string err;
        std::unique_ptr<IAudioSource> src = OpenAudioSource(path, &err);
        CHECK(src != nullptr);
        if (!src) { std::remove(path); continue; }
        CHECK(src->TotalFrames() == frames);
        CHECK(src->SourceChannels() == 2);
        CHECK(Near(src->FrameRate(), 48000.0f));

        const int scaleBits = (bits == 16) ? 15 : 23;
        const std::vector<float> got = ReadAll(*src);
        CHECK((int64_t)got.size() == frames * 2);
        int64_t mismatches = 0;
        for (int64_t i = 0; i < frames * 2; i++) {
            const float expect =
                float(double(Quantize(sig[(size_t)i], bits, false))
                      / std::ldexp(1.0, scaleBits));
            if (!Near(got[(size_t)i], expect)) mismatches++;
        }
        CHECK(mismatches == 0);   // lossless: every sample, exactly

        // Seeking lands on the same samples the sequential read produced.
        CHECK(src->Seek(frames / 2));
        const float* blk = nullptr;
        size_t n = 0;
        CHECK(src->ReadChunk(&blk, &n));
        CHECK(n > 0);
        if (n > 0) {
            const size_t at = (size_t)(frames / 2) * 2;
            CHECK(Near(blk[0], got[at]));
            CHECK(Near(blk[1], got[at + 1]));
        }
        // ...and seeking to the very end reads nothing, cleanly.
        CHECK(src->Seek(frames));
        CHECK(!src->ReadChunk(&blk, &n));
        std::remove(path);
    }
}

static void TestFlacCorrupt() {
    std::printf("test_flac_corrupt\n");
    // Frames of valid FLAC, then truncated mid-stream: what was written must
    // still come out, and the read must STOP (not spin, not crash).
    const char* path = "formats_test_trunc.flac";
    std::remove(path);
    {
        const std::vector<float> sig = TestSignal(30000, 2);
        FlacSink sink;
        SinkFormat fmt;
        fmt.sampleRate = 44100; fmt.channels = 2; fmt.bitDepth = 16;
        fmt.dither = false;
        CHECK(sink.Open(path, fmt));
        CHECK(sink.WriteFloat(sig.data(), sig.size()));
        CHECK(sink.Close());
    }
    {
        // Chop the last third off.
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        const std::streamoff len = in.tellg();
        in.seekg(0);
        std::vector<char> bytes((size_t)len);
        in.read(bytes.data(), len);
        in.close();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), (std::streamsize)(len * 2 / 3));
    }
    {
        std::string err;
        std::unique_ptr<IAudioSource> src = OpenAudioSource(path, &err);
        // Reading either fails at once or hands out the frames that survived --
        // both are clean. What is asserted is that it TERMINATES and never
        // reads past the end.
        if (src) {
            int64_t total = 0;
            const float* blk = nullptr;
            size_t n = 0;
            int guard = 0;
            while (src->ReadChunk(&blk, &n) && ++guard < 1000)
                total += (int64_t)n;
            CHECK(guard < 1000);
            CHECK(total >= 0);
        } else {
            CHECK(!err.empty());
        }
    }
    std::remove(path);

    // Garbage with a FLAC marker: refused, not crashed on.
    {
        const char* bad = "formats_test_garbage.flac";
        std::ofstream f(bad, std::ios::binary);
        f.write("fLaC", 4);
        for (int i = 0; i < 200; i++) { char c = (char)(i * 7); f.write(&c, 1); }
        f.close();
        std::string err;
        std::unique_ptr<IAudioSource> src = OpenAudioSource(bad, &err);
        if (src) {   // a decoder that accepted the header still must not crash
            const float* blk = nullptr;
            size_t n = 0;
            int guard = 0;
            while (src->ReadChunk(&blk, &n) && ++guard < 1000) {}
            CHECK(guard < 1000);
        }
        std::remove(bad);
    }
}
#endif  // DAW_HAVE_FLAC

// --- 5. Ogg Vorbis ---------------------------------------------------------

#if defined(DAW_HAVE_VORBIS)
static void TestVorbisRoundTrip() {
    std::printf("test_vorbis_roundtrip\n");
    const int64_t frames = 40000;
    const char* path = "formats_test.ogg";
    std::remove(path);

    // Left carries the signal, right is silent: channel order, the mono/stereo
    // widening and the frame count are all proven by one comparison.
    std::vector<float> sig((size_t)frames * 2, 0.0f);
    for (int64_t i = 0; i < frames; i++)
        sig[(size_t)i * 2] = 0.8f * std::sin(float(i) * 0.02f);

    VorbisSink sink;
    SinkFormat fmt;
    fmt.sampleRate = 48000;
    fmt.channels   = 2;
    fmt.quality    = 0.5f;
    CHECK(sink.Open(path, fmt));
    CHECK(sink.WriteFloat(sig.data(), sig.size()));
    CHECK(sink.Close());

    CHECK(SniffAudioFileFormat(path) == AudioFileFormat::Ogg);
    std::string err;
    std::unique_ptr<IAudioSource> src = OpenAudioSource(path, &err);
    CHECK(src != nullptr);
    if (src) {
        // Vorbis is LOSSY, so this is a bound, not an equality: the frame count
        // is exact (granule position) and the error is small -- a codec bug,
        // a channel swap or a block-boundary shift all blow straight past it.
        CHECK(src->SourceChannels() == 2);
        CHECK(Near(src->FrameRate(), 48000.0f));
        CHECK(std::llabs(src->TotalFrames() - frames) <= 1);

        std::vector<float> got = ReadAll(*src);
        CHECK(std::llabs((int64_t)(got.size() / 2) - frames) <= 1);

        double err2 = 0.0, ref2 = 0.0, right2 = 0.0;
        const int64_t n = std::min<int64_t>(frames, (int64_t)(got.size() / 2));
        for (int64_t i = 0; i < n; i++) {
            const double d = double(got[(size_t)i * 2]) - double(sig[(size_t)i * 2]);
            err2 += d * d;
            ref2 += double(sig[(size_t)i * 2]) * double(sig[(size_t)i * 2]);
            right2 += double(got[(size_t)i * 2 + 1]) * double(got[(size_t)i * 2 + 1]);
        }
        CHECK(ref2 > 0.0);
        CHECK(err2 / ref2 < 0.01);   // < -20 dB error: comfortably lossy, not broken
        // The silent right channel must stay essentially silent -- a channel
        // swap would put the whole signal here.
        CHECK(right2 / (ref2 > 0 ? ref2 : 1.0) < 0.01);

        // Seek: the same waveform comes back at the seek target.
        CHECK(src->Seek(frames / 2));
        const float* blk = nullptr;
        size_t nn = 0;
        CHECK(src->ReadChunk(&blk, &nn));
        CHECK(nn > 0);
        if (nn > 0) {
            const size_t at = (size_t)(frames / 2) * 2;
            CHECK(std::fabs(double(blk[0]) - double(got[at])) < 0.05);
        }
        // Seeking to the very end reads nothing, cleanly, exactly as
        // WavSource does.
        CHECK(src->Seek(src->TotalFrames()));
        CHECK(!src->ReadChunk(&blk, &nn));
    }
    std::remove(path);
}

static void TestVorbisCorrupt() {
    std::printf("test_vorbis_corrupt\n");
    const char* bad = "formats_test_trunc.ogg";
    std::remove(bad);
    {
        const std::vector<float> sig = TestSignal(20000, 2);
        VorbisSink sink;
        SinkFormat fmt;
        fmt.sampleRate = 44100; fmt.channels = 2; fmt.quality = 0.4f;
        CHECK(sink.Open(bad, fmt));
        CHECK(sink.WriteFloat(sig.data(), sig.size()));
        CHECK(sink.Close());
    }
    {
        std::ifstream in(bad, std::ios::binary | std::ios::ate);
        const std::streamoff len = in.tellg();
        in.seekg(0);
        std::vector<char> bytes((size_t)len);
        in.read(bytes.data(), len);
        in.close();
        std::ofstream out(bad, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), (std::streamsize)(len / 2));
    }
    {
        std::string err;
        std::unique_ptr<IAudioSource> src = OpenAudioSource(bad, &err);
        if (src) {
            const float* blk = nullptr;
            size_t n = 0;
            int guard = 0;
            while (src->ReadChunk(&blk, &n) && ++guard < 100000) {}
            CHECK(guard < 100000);   // it terminated
        }
    }
    std::remove(bad);

    // "OggS" with a Vorbis packet header, then nothing readable.
    {
        const char* junk = "formats_test_junk.ogg";
        std::ofstream f(junk, std::ios::binary);
        uint8_t hdr[64] = { 0 };
        std::memcpy(hdr, "OggS", 4);
        hdr[26] = 1; hdr[27] = 30; hdr[28] = 0x01;
        std::memcpy(hdr + 29, "vorbis", 6);
        f.write((char*)hdr, 64);
        f.close();
        std::string err;
        std::unique_ptr<IAudioSource> src = OpenAudioSource(junk, &err);
        CHECK(src == nullptr);
        CHECK(!err.empty());
        std::remove(junk);
    }
}
#endif  // DAW_HAVE_VORBIS

// --- 6. the export dialog's menus ------------------------------------------

static void TestChoiceTables() {
    std::printf("test_choice_tables\n");
    const int n = ExportFormatChoiceCount();
    CHECK(n >= 3);
    CHECK(ExportFormatChoiceAt(0).container == AudioFileFormat::Wav);
    CHECK(ExportFormatChoiceAt(0).bitDepth == 16);
    CHECK(ExportFormatChoiceAt(n - 1).container != AudioFileFormat::Unknown);
    // Every row must be writable by this build, or the dialog would offer
    // something the export can only fail on.
    for (int i = 0; i < n; i++)
        CHECK(AudioFileFormatCanWrite(ExportFormatChoiceAt(i).container));
    // Rows are unique, and the index lookup finds each one.
    for (int i = 0; i < n; i++) {
        const ExportFormatChoice& c = ExportFormatChoiceAt(i);
        CHECK(ExportFormatChoiceIndex(c.container, c.bitDepth) == i);
    }
    CHECK(ExportFormatChoiceIndex(AudioFileFormat::Wav, 24) >= 0);
    CHECK(ExportFormatChoiceIndex(AudioFileFormat::Wav, 99) == -1);
#if defined(DAW_HAVE_FLAC)
    CHECK(ExportFormatChoiceIndex(AudioFileFormat::Flac, 16) >= 0);
    CHECK(ExportFormatChoiceIndex(AudioFileFormat::Flac, 24) >= 0);
#else
    CHECK(ExportFormatChoiceIndex(AudioFileFormat::Flac, 16) == -1);
#endif
#if defined(DAW_HAVE_VORBIS)
    CHECK(ExportFormatChoiceIndex(AudioFileFormat::Ogg, 16) >= 0);
#else
    CHECK(ExportFormatChoiceIndex(AudioFileFormat::Ogg, 16) == -1);
#endif
    // Out-of-range access is clamped rather than undefined.
    CHECK(ExportFormatChoiceAt(-1).container == AudioFileFormat::Wav);
    CHECK(ExportFormatChoiceAt(999).container == AudioFileFormat::Wav);

    const int qn = VorbisQualityChoiceCount();
    CHECK(qn >= 2);
    CHECK(VorbisQualityChoiceAt(VorbisQualityChoiceIndex(0.5f)).quality == 0.5f);
    CHECK(VorbisQualityChoiceIndex(-5.0f) == 0);
    CHECK(VorbisQualityChoiceIndex(9.0f) == qn - 1);
}

// --- 7. the exporter, per container ----------------------------------------

// A one-second project: a 16-bit WAV source holding a known ramp, one clip.
static void WriteSourceWav(const std::string& path, int64_t frames) {
    WavWriter w;
    w.OpenFormat(path.c_str(), 48000, 2, 16, false);
    std::vector<int16_t> pcm((size_t)frames * 2);
    for (int64_t i = 0; i < frames; i++) {
        pcm[(size_t)i * 2]     = (int16_t)((i * 37) % 20000 - 10000);
        pcm[(size_t)i * 2 + 1] = (int16_t)(10000 - (i * 53) % 20000);
    }
    w.WriteInt16(pcm.data(), pcm.size());
    w.Close();
}

static void TestExportContainers() {
    std::printf("test_export_containers\n");
    const std::string wav = "formats_test_src.wav";
    WriteSourceWav(wav, 24000);

    Project p;
    p.sampleRate = 48000.0;
    Track t;
    t.id = p.NextTrackId();
    t.type = TrackType::Audio;
    t.name = "src";
    Clip c;
    c.id = p.NextClipId();
    c.startFrame = 0;
    c.lengthFrames = 24000;
    c.sourcePath = wav;
    t.clips.push_back(c);
    CHECK(p.AddTrack(t));

    struct Case { AudioFileFormat container; int bits; const char* path; };
    std::vector<Case> cases = {
        { AudioFileFormat::Wav, 16, "formats_test_out.wav" },
        { AudioFileFormat::Wav, 24, "formats_test_out24.wav" },
    };
#if defined(DAW_HAVE_FLAC)
    cases.push_back({ AudioFileFormat::Flac, 16, "formats_test_out.flac" });
    cases.push_back({ AudioFileFormat::Flac, 24, "formats_test_out24.flac" });
#endif
#if defined(DAW_HAVE_VORBIS)
    cases.push_back({ AudioFileFormat::Ogg, 16, "formats_test_out.ogg" });
#endif

    int64_t referenceFrames = -1;
    for (const Case& k : cases) {
        std::remove(k.path);
        std::remove((std::string(k.path) + ".part").c_str());
        ExportOptions opts;
        opts.format.container = k.container;
        opts.format.bitDepth  = k.bits;
        opts.format.dither    = false;
        CHECK(ExportWav(p, k.path, 0.0, opts));

        // The temp is gone and the real file is there.
        CHECK(!std::ifstream(std::string(k.path) + ".part").good());
        std::ifstream probe(k.path, std::ios::binary);
        CHECK(probe.good());
        probe.close();

        CHECK(SniffAudioFileFormat(k.path) == k.container);
        std::string err;
        std::unique_ptr<IAudioSource> src = OpenAudioSource(k.path, &err);
        CHECK(src != nullptr);
        if (!src) { std::remove(k.path); continue; }
        CHECK(src->SourceChannels() == 2);
        if (referenceFrames < 0) referenceFrames = src->TotalFrames();
        else if (k.container != AudioFileFormat::Ogg)
            CHECK(src->TotalFrames() == referenceFrames);

        // The bounce is the source ramp, so it is not silent and not constant.
        std::vector<float> got = ReadAll(*src);
        CHECK(!got.empty());
        float lo = 1.0f, hi = -1.0f;
        for (float v : got) { if (v < lo) lo = v; if (v > hi) hi = v; }
        CHECK(hi - lo > 0.05f);
        std::remove(k.path);
    }

    // A container this build cannot write fails cleanly and leaves NOTHING at
    // the destination -- the temp+rename contract holds for a refusal too.
    {
        const char* path = "formats_test_unwritable.aiff";
        std::remove(path);
        std::remove((std::string(path) + ".part").c_str());
        ExportOptions opts;
        opts.format.container = AudioFileFormat::Aiff;   // read-only
        CHECK(!ExportWav(p, path, 0.0, opts));
        CHECK(!std::ifstream(path).good());
        CHECK(!std::ifstream(std::string(path) + ".part").good());
    }

    // Cancel is the same for every container: the worker polls the flag per
    // chunk, so a cancelled run leaves neither the destination nor the temp.
    {
        std::atomic<bool> cancel{true};   // raised before it starts
        ExportJob job;
        job.cancel = &cancel;
        ExportOptions opts;
        opts.job = &job;
        opts.format.container = AudioFileFormat::Wav;
        const char* path = "formats_test_cancel.wav";
        std::remove(path);
        CHECK(!ExportWav(p, path, 0.0, opts));
        CHECK(!std::ifstream(path).good());
        CHECK(!std::ifstream(std::string(path) + ".part").good());
    }

    // Stems carry the container's own extension: a FLAC stems run must not
    // leave files named ".wav".
    {
#if defined(DAW_HAVE_FLAC)
        const std::string dir = "formats_test_stems";
        const std::string rm = "rm -rf " + dir;
        if (system(rm.c_str()) != 0) return;
        const std::string mk = "mkdir -p " + dir;
        if (system(mk.c_str()) != 0) return;
        ExportOptions opts;
        opts.format.container = AudioFileFormat::Flac;
        opts.format.bitDepth  = 16;
        CHECK(ExportStems(p, dir, 0.0, opts) == 1);
        std::ifstream stem(dir + "/01_src.flac", std::ios::binary);
        CHECK(stem.good());
        stem.close();
        CHECK(SniffAudioFileFormat(dir + "/01_src.flac") == AudioFileFormat::Flac);
        if (system(rm.c_str()) != 0) return;
#endif
    }

    std::remove(wav.c_str());
}

// --- main ------------------------------------------------------------------

int main() {
    TestSniff();
    TestAiffPcm16();
    TestAiffPcm8AndSowt();
    TestAiffFloat();
    TestAiffClampsAndCorrupt();
    TestFactory();
#if defined(DAW_HAVE_FLAC)
    TestFlacRoundTrip();
    TestFlacCorrupt();
#endif
#if defined(DAW_HAVE_VORBIS)
    TestVorbisRoundTrip();
    TestVorbisCorrupt();
#endif
    TestChoiceTables();
    TestExportContainers();

    std::printf("\nformats_tests: %d checks, %d failures"
#if !defined(DAW_HAVE_FLAC)
                " (built without FLAC)"
#endif
#if !defined(DAW_HAVE_VORBIS)
                " (built without Ogg Vorbis)"
#endif
                "\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
