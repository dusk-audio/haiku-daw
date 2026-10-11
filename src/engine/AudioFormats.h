// AudioFormats — what a file is, and which reader or writer to use for it.
//
// One name for the formats this app understands, the sniffing that decides
// which one a file is, and the two factories. Nothing here looks at a file's
// NAME: a WAV called `take.flac` is a WAV, and an AIFF someone renamed `.wav`
// still opens. That is what makes the drop handler and the import panel accept
// every format with no extension list to keep in sync.
//
// Kit-free (std C++ only). FLAC and Ogg Vorbis are optional at build time
// (DAW_FLAC / DAW_VORBIS, see CMakeLists.txt): with a library absent its
// sources are not compiled, `AudioFileFormatCanRead/CanWrite` report false and
// the factories refuse that format with an explicit message -- the format is
// never offered rather than offered and then broken.
#pragma once

#include "IAudioSource.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace daw {

class IAudioSink;

enum class AudioFileFormat {
    Unknown = 0,   // not a format we know (or the file is unreadable)
    Wav,
    Aiff,          // AIFF and AIFF-C (uncompressed family)
    Flac,
    Ogg,           // Ogg Vorbis
};

// "WAV" / "AIFF" / "FLAC" / "Ogg Vorbis" / "unknown": for messages.
const char* AudioFileFormatName(AudioFileFormat f);

// The bare extension without a dot: "wav", "aiff", "flac", "ogg", and "" for
// Unknown. Used to name a bounced file and to prefill the save panel.
const char* AudioFileFormatExtension(AudioFileFormat f);

// Can THIS BUILD read / write that format? False for a format whose library
// was missing at configure time, and for Aiff writing (we read AIFF, we do not
// write it) and Unknown.
bool AudioFileFormatCanRead(AudioFileFormat f);
bool AudioFileFormatCanWrite(AudioFileFormat f);

// Identify a file from its leading bytes alone. Returns Unknown for anything
// unrecognised, including a file that cannot be read at all.
//
// The header-only form exists so the sniffing rules themselves are unit
// testable without touching a filesystem; `SniffAudioFileFormat` is the file
// form the drop handler and the import path use.
AudioFileFormat SniffAudioHeader(const uint8_t* bytes, size_t size);
AudioFileFormat SniffAudioFileFormat(const std::string& path);

// Open `path` with the reader its header calls for. Returns nullptr when the
// file is not a readable format of ours, when it is corrupt, or when its
// format is one this build has no library for -- in which case `error`, if
// given, says which of those it was.
std::unique_ptr<IAudioSource> OpenAudioSource(const std::string& path,
                                              std::string* error = nullptr);

// A sink for `container`, or nullptr when this build cannot write it. The
// caller opens it; the returned sink owns nothing outside itself.
std::unique_ptr<IAudioSink> MakeAudioSink(AudioFileFormat container);

// --- the export dialog's menus --------------------------------------------
// The dialog has to offer exactly what this build can write, and the tests
// have to drive it without a window. Both read these tables.

// One row of the Format menu: a container at one depth (32 = IEEE float, WAV
// only). The array is built from what is compiler-available, so a build
// without FLAC never shows a FLAC row.
struct ExportFormatChoice {
    AudioFileFormat container;
    int             bitDepth;
    const char*     label;
};
int ExportFormatChoiceCount();
const ExportFormatChoice& ExportFormatChoiceAt(int index);
// The row matching (container, bitDepth), or -1; for marking the menu from
// remembered settings, including a depth the container no longer offers.
int ExportFormatChoiceIndex(AudioFileFormat container, int bitDepth);

// The Vorbis quality presets (libvorbis VBR quality, 0..1).
struct VorbisQualityChoice {
    const char* label;
    float       quality;
};
int VorbisQualityChoiceCount();
const VorbisQualityChoice& VorbisQualityChoiceAt(int index);
// The preset nearest `q`, so a remembered value always lands on a marked row.
int VorbisQualityChoiceIndex(float q);

} // namespace daw
