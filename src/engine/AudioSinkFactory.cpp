// The write side of AudioFormats.h: MakeAudioSink, and the WAV sink that wraps
// the existing WavWriter.
//
// (No AudioSinkFactory.h: MakeAudioSink is declared in AudioFormats.h beside
// the reader factory, so a caller has one header for both directions.)
//
// The WAV sink is deliberately thin. WavWriter already does exactly what a sink
// must do -- placeholder header, streamed sample blocks, sizes patched on
// Close -- and every existing bounce goes through it, so wrapping it rather
// than rewriting it is what keeps a WAV export byte-for-byte what it was.
#include "AudioFormats.h"

#include "IAudioSink.h"
#include "WavWriter.h"

#if defined(DAW_HAVE_FLAC)
#include "FlacSink.h"
#endif
#if defined(DAW_HAVE_VORBIS)
#include "VorbisSink.h"
#endif

namespace daw {

namespace {

class WavSink : public IAudioSink {
public:
    bool Open(const std::string& path, const SinkFormat& fmt) override {
        fDither = fmt.dither;
        // truncate (not exclusive): the exporter hands in its own ".part" temp,
        // which must be replaced if a previous run left one behind.
        return fWriter.OpenFormat(path, fmt.sampleRate, fmt.channels,
                                  fmt.bitDepth, fmt.floatFmt);
    }
    bool WriteFloat(const float* interleaved, size_t sampleCount) override {
        return fWriter.WriteFloat(interleaved, sampleCount, fDither);
    }
    bool Close() override { return fWriter.Close(); }

private:
    WavWriter fWriter;
    bool      fDither = true;
};

} // namespace

std::unique_ptr<IAudioSink> MakeAudioSink(AudioFileFormat container) {
    switch (container) {
        case AudioFileFormat::Wav:
            return std::make_unique<WavSink>();
        case AudioFileFormat::Flac:
#if defined(DAW_HAVE_FLAC)
            return std::make_unique<FlacSink>();
#else
            return nullptr;
#endif
        case AudioFileFormat::Ogg:
#if defined(DAW_HAVE_VORBIS)
            return std::make_unique<VorbisSink>();
#else
            return nullptr;
#endif
        default:
            return nullptr;   // AIFF writing is not offered
    }
}

} // namespace daw
