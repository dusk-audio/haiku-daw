// IAudioSink — the writer side of the formats work, the mirror of
// IAudioSource.
//
// The Exporter used to end in `WavWriter writer; writer.OpenFormat(...)`.
// It now ends in a sink, so "render the mix, dither it, name a temp file,
// rename on success, report progress, honour cancel" is ONE code path for
// every container: the WAV sink wraps the existing WavWriter (byte-identical
// output), the FLAC sink drives libFLAC's stream encoder, and the Vorbis sink
// drives vorbisenc + libogg.
//
// Kit-free (std C++ only): the interface lives here, the implementations in
// AudioSinkFactory.cpp and the two codec translation units.
#pragma once

#include <cstddef>
#include <string>

namespace daw {

// Everything a sink needs to know about the file it is about to write.
struct SinkFormat {
    int   sampleRate = 44100;
    int   channels   = 2;
    // 16 or 24 = PCM. 32 with floatFmt = IEEE float (WAV only; a FLAC request
    // for 32 is clamped to 24 by the exporter before it gets here).
    int   bitDepth   = 16;
    bool  floatFmt   = false;
    // TPDF dither at the 16-bit LSB (see Dither.h). Honoured by WAV and FLAC
    // at 16 bits; a lossy codec has nothing to dither.
    bool  dither     = true;
    // Ogg Vorbis VBR quality in [0,1] (libvorbis' own scale). Ignored by the
    // WAV and FLAC sinks.
    float quality    = 0.5f;
};

class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    // Create `path` (truncating anything already there -- the exporter hands
    // in its own ".part" temp) and write whatever header the format allows up
    // front. Returns false on any failure, in which case nothing else is
    // called and the caller removes the temp.
    virtual bool Open(const std::string& path, const SinkFormat& fmt) = 0;

    // Append `sampleCount` interleaved float samples (sampleCount = frames *
    // channels), converting to the sink's configured format. Returns false if
    // the sink is closed or the write failed.
    virtual bool WriteFloat(const float* interleaved, size_t sampleCount) = 0;

    // Finalize the file (patch sizes, flush the codec, write the trailing
    // header). Returns false if that failed, in which case the file is not
    // complete and the caller must not rename it into place. Safe to call
    // twice and on a never-opened sink.
    virtual bool Close() = 0;
};

} // namespace daw
