// FlacSink — a FLAC writer built on libFLAC's stream encoder.
//
// The exporter's second sink (see IAudioSink.h). 16- and 24-bit, lossless, so
// what comes out decodes back to exactly the integers that went in -- which is
// what lets the round-trip test compare samples rather than approximate them.
// At 16 bits it dithers through the same TpdfDither the WAV writer uses, so a
// 16-bit FLAC and a 16-bit WAV of the same mix carry the same dithered signal.
//
// Compiled only when DAW_FLAC found the library.
#pragma once

#include "IAudioSink.h"
#include "Dither.h"

#include <FLAC/stream_encoder.h>

#include <vector>

namespace daw {

class FlacSink : public IAudioSink {
public:
    FlacSink() = default;
    ~FlacSink() override;

    FlacSink(const FlacSink&) = delete;
    FlacSink& operator=(const FlacSink&) = delete;

    bool Open(const std::string& path, const SinkFormat& fmt) override;
    bool WriteFloat(const float* interleaved, size_t sampleCount) override;
    bool Close() override;

private:
    FLAC__StreamEncoder* fEnc = nullptr;
    int   fBits    = 16;      // 16 or 24
    int   fChannels = 2;
    bool  fDither  = true;
    bool  fOpen    = false;
    bool  fFailed  = false;
    TpdfDither fRng;
    std::vector<FLAC__int32> fConv;   // interleaved int32 staging buffer
};

} // namespace daw
