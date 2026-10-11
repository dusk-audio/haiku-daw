#include "AudioFormats.h"

#include "IAudioSink.h"
#include "WavSource.h"
#include "AiffSource.h"

#if defined(DAW_HAVE_FLAC)
#include "FlacSource.h"
#endif
#if defined(DAW_HAVE_VORBIS)
#include "VorbisSource.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace daw {

namespace {

// The four-byte magic each container starts with. Ogg is shared by Vorbis,
// Opus, FLAC-in-Ogg and others; the codec check below tells them apart.
bool Has(const uint8_t* p, size_t n, size_t at, const char* magic) {
    const size_t len = std::strlen(magic);
    if (at + len > n) return false;
    return std::memcmp(p + at, magic, len) == 0;
}

} // namespace

const char* AudioFileFormatName(AudioFileFormat f) {
    switch (f) {
        case AudioFileFormat::Wav:  return "WAV";
        case AudioFileFormat::Aiff: return "AIFF";
        case AudioFileFormat::Flac: return "FLAC";
        case AudioFileFormat::Ogg:  return "Ogg Vorbis";
        default:                    return "unknown";
    }
}

const char* AudioFileFormatExtension(AudioFileFormat f) {
    switch (f) {
        case AudioFileFormat::Wav:  return "wav";
        case AudioFileFormat::Aiff: return "aiff";
        case AudioFileFormat::Flac: return "flac";
        case AudioFileFormat::Ogg:  return "ogg";
        default:                    return "";
    }
}

bool AudioFileFormatCanRead(AudioFileFormat f) {
    switch (f) {
        case AudioFileFormat::Wav:  return true;
        case AudioFileFormat::Aiff: return true;
        case AudioFileFormat::Flac:
#if defined(DAW_HAVE_FLAC)
            return true;
#else
            return false;
#endif
        case AudioFileFormat::Ogg:
#if defined(DAW_HAVE_VORBIS)
            return true;
#else
            return false;
#endif
        default: return false;
    }
}

bool AudioFileFormatCanWrite(AudioFileFormat f) {
    switch (f) {
        case AudioFileFormat::Wav:  return true;
        case AudioFileFormat::Flac:
            // FLAC's decoder and encoder are the same library, so read and
            // write availability cannot diverge; keeping both through the one
            // define means a build can never offer a write it cannot perform.
            return AudioFileFormatCanRead(AudioFileFormat::Flac);
        case AudioFileFormat::Ogg:
            return AudioFileFormatCanRead(AudioFileFormat::Ogg);
        default: return false;   // AIFF is read-only, Unknown is neither
    }
}

AudioFileFormat SniffAudioHeader(const uint8_t* bytes, size_t size) {
    if (bytes == nullptr || size < 12)
        return AudioFileFormat::Unknown;

    // RIFF....WAVE. A bare RIFF that is not WAVE (AVI, for one) is not ours.
    if (Has(bytes, size, 0, "RIFF") && Has(bytes, size, 8, "WAVE"))
        return AudioFileFormat::Wav;

    // FORM....AIFF / AIFC. (AIFF is an IFF form, so the size field sits
    // between the two fourccs exactly as RIFF's does.)
    if (Has(bytes, size, 0, "FORM")
        && (Has(bytes, size, 8, "AIFF") || Has(bytes, size, 8, "AIFC")))
        return AudioFileFormat::Aiff;

    // Native FLAC streams start with the "fLaC" marker. (FLAC-in-Ogg starts
    // with OggS and is handled below.)
    if (Has(bytes, size, 0, "fLaC"))
        return AudioFileFormat::Flac;

    // Ogg: the container's own magic only says "Ogg" -- it carries Vorbis,
    // Opus, FLAC and more. The first page's payload starts with the codec's
    // identification packet: type byte 0x01 then "vorbis" for Vorbis. Opus
    // pages start with "OpusHead" and Ogg-FLAC with 0x7F "FLAC"; both fall
    // through to Unknown rather than being handed to a decoder that would
    // refuse them.
    //
    // Where that packet starts depends on the page's lacing table (one byte
    // per 255-byte segment), so it is walked rather than assumed. A first page
    // flagged "continued" (0x01) is the middle of a packet, not a stream head.
    if (Has(bytes, size, 0, "OggS")) {
        if (size < 28) return AudioFileFormat::Unknown;
        if (bytes[5] & 0x01) return AudioFileFormat::Unknown;   // continued
        const size_t packetAt = 27 + size_t(bytes[26]);
        if (packetAt < size && bytes[packetAt] == 0x01
            && Has(bytes, size, packetAt + 1, "vorbis"))
            return AudioFileFormat::Ogg;
        return AudioFileFormat::Unknown;
    }

    return AudioFileFormat::Unknown;
}

AudioFileFormat SniffAudioFileFormat(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return AudioFileFormat::Unknown;
    // Enough for an Ogg page header plus the start of its first packet.
    uint8_t buf[64] = { 0 };
    f.read(reinterpret_cast<char*>(buf), sizeof(buf));
    const std::streamsize got = f.gcount();
    if (got <= 0)
        return AudioFileFormat::Unknown;
    return SniffAudioHeader(buf, size_t(got));
}

std::unique_ptr<IAudioSource> OpenAudioSource(const std::string& path,
                                              std::string* error) {
    const AudioFileFormat fmt = SniffAudioFileFormat(path);
    const auto fail = [&](const char* why) -> std::unique_ptr<IAudioSource> {
        if (error) *error = why;
        return nullptr;
    };

    switch (fmt) {
        case AudioFileFormat::Wav: {
            auto src = std::make_unique<WavSource>();
            if (!src->Open(path))
                return fail("the WAV file is corrupt or unsupported");
            return src;
        }
        case AudioFileFormat::Aiff: {
            auto src = std::make_unique<AiffSource>();
            if (!src->Open(path))
                return fail("the AIFF file is corrupt or unsupported");
            return src;
        }
        case AudioFileFormat::Flac:
#if defined(DAW_HAVE_FLAC)
        {
            auto src = std::make_unique<FlacSource>();
            if (!src->Open(path))
                return fail("the FLAC file is corrupt or unreadable");
            return src;
        }
#else
            return fail("this build has no FLAC support");
#endif
        case AudioFileFormat::Ogg:
#if defined(DAW_HAVE_VORBIS)
        {
            auto src = std::make_unique<VorbisSource>();
            if (!src->Open(path))
                return fail("the Ogg Vorbis file is corrupt or unreadable");
            return src;
        }
#else
            return fail("this build has no Ogg Vorbis support");
#endif
        default:
            return fail("the file is not WAV, AIFF, FLAC or Ogg Vorbis");
    }
}

// --- the export dialog's menus --------------------------------------------

namespace {

// Built at first use from what this build can write, so an unavailable format
// is never offered. WAV first (the historical default), then the compressed
// formats in the order the plan lists them.
constexpr int kWavChoices = 3;   // 16-bit PCM, 24-bit PCM, 32-bit float

int BuildFormatChoices(ExportFormatChoice* out) {
    int n = 0;
    out[n++] = { AudioFileFormat::Wav, 16, "WAV 16-bit PCM" };
    out[n++] = { AudioFileFormat::Wav, 24, "WAV 24-bit PCM" };
    out[n++] = { AudioFileFormat::Wav, 32, "WAV 32-bit float" };
    if (AudioFileFormatCanWrite(AudioFileFormat::Flac)) {
        out[n++] = { AudioFileFormat::Flac, 16, "FLAC 16-bit" };
        out[n++] = { AudioFileFormat::Flac, 24, "FLAC 24-bit" };
    }
    if (AudioFileFormatCanWrite(AudioFileFormat::Ogg)) {
        // 16 is a placeholder depth for a lossy codec; the exporter ignores it.
        out[n++] = { AudioFileFormat::Ogg, 16, "Ogg Vorbis" };
    }
    return n;
}

int FormatChoiceCount() {
    int n = kWavChoices;
    if (AudioFileFormatCanWrite(AudioFileFormat::Flac)) n += 2;
    if (AudioFileFormatCanWrite(AudioFileFormat::Ogg))  n += 1;
    return n;
}

const VorbisQualityChoice kVorbisQualities[] = {
    { "Low",    0.2f },
    { "Medium", 0.5f },
    { "High",   0.8f },
    { "Maximum", 1.0f },
};

} // namespace

int ExportFormatChoiceCount() {
    return FormatChoiceCount();
}

const ExportFormatChoice& ExportFormatChoiceAt(int index) {
    static ExportFormatChoice table[kWavChoices + 3];   // + FLAC pair + Ogg
    static const int n = BuildFormatChoices(table);
    (void)n;
    if (index < 0 || index >= FormatChoiceCount()) index = 0;
    return table[index];
}

int ExportFormatChoiceIndex(AudioFileFormat container, int bitDepth) {
    const int n = ExportFormatChoiceCount();
    for (int i = 0; i < n; i++) {
        const ExportFormatChoice& c = ExportFormatChoiceAt(i);
        if (c.container != container) continue;
        // An Ogg row carries a placeholder depth: any depth selects it, so a
        // remembered value (or a message from an older build) still finds it.
        if (container == AudioFileFormat::Ogg || c.bitDepth == bitDepth)
            return i;
    }
    return -1;
}

int VorbisQualityChoiceCount() {
    return int(sizeof(kVorbisQualities) / sizeof(kVorbisQualities[0]));
}

const VorbisQualityChoice& VorbisQualityChoiceAt(int index) {
    const int n = VorbisQualityChoiceCount();
    if (index < 0 || index >= n) index = 1;   // Medium
    return kVorbisQualities[index];
}

int VorbisQualityChoiceIndex(float q) {
    const int n = VorbisQualityChoiceCount();
    int best = 0;
    float bestDist = -1.0f;
    for (int i = 0; i < n; i++) {
        const float d = std::fabs(kVorbisQualities[i].quality - q);
        if (bestDist < 0.0f || d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

} // namespace daw
