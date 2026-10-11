#include "FlacSink.h"

#include <cmath>
#include <cstdio>

namespace daw {

FlacSink::~FlacSink() {
    Close();
}

bool FlacSink::Open(const std::string& path, const SinkFormat& fmt) {
    if (fOpen) return false;
    if (fmt.channels < 1 || fmt.channels > 8 || fmt.sampleRate < 1)
        return false;
    // FLAC is a fixed-point codec: 16 and 24 are the depths the exporter
    // offers. Anything else (a 32-bit float request) becomes 24 -- the
    // highest FLAC depth -- rather than being refused, so a caller that asks
    // for "best" still gets a file.
    fBits = (fmt.bitDepth == 16) ? 16 : 24;
    fChannels = fmt.channels;
    fDither = fmt.dither && fBits == 16;
    fFailed = false;
    fOpen = false;

    fEnc = FLAC__stream_encoder_new();
    if (!fEnc) return false;

    FLAC__stream_encoder_set_channels(fEnc, (unsigned)fChannels);
    FLAC__stream_encoder_set_bits_per_sample(fEnc, (unsigned)fBits);
    FLAC__stream_encoder_set_sample_rate(fEnc, (unsigned)fmt.sampleRate);
    // A streaming DAW wants a fast encode, not the smallest file: level 5 is
    // libFLAC's own default and roughly the knee of the curve.
    FLAC__stream_encoder_set_compression_level(fEnc, 5);

    if (FLAC__stream_encoder_init_file(fEnc, path.c_str(), nullptr, nullptr)
        != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        std::fprintf(stderr, "FlacSink: cannot create '%s'\n", path.c_str());
        FLAC__stream_encoder_delete(fEnc);
        fEnc = nullptr;
        return false;
    }
    fOpen = true;
    return true;
}

bool FlacSink::WriteFloat(const float* interleaved, size_t sampleCount) {
    if (!fOpen || fFailed || interleaved == nullptr) return false;
    if (sampleCount == 0) return true;

    const double scale = (fBits == 16) ? 32767.0 : 8388607.0;
    const long   lo    = (fBits == 16) ? -32768L : -8388608L;
    const long   hi    = (fBits == 16) ?  32767L :  8388607L;

    if (fConv.size() < sampleCount) fConv.resize(sampleCount);
    for (size_t i = 0; i < sampleCount; i++) {
        // Sanitize before scaling: lround() on a NaN/Inf is undefined, and a
        // sample past full scale would wrap instead of clipping.
        float x = interleaved[i];
        if (!std::isfinite(x)) x = 0.0f;
        else if (x >  1.0f)    x =  1.0f;
        else if (x < -1.0f)    x = -1.0f;
        double v = double(x) * scale;
        if (fDither) v += fRng.Next();
        long q = std::lround(v);
        if (q > hi) q = hi;
        if (q < lo) q = lo;
        fConv[i] = (FLAC__int32)q;
    }

    const size_t frames = sampleCount / (size_t)fChannels;
    if (frames == 0) return true;
    if (!FLAC__stream_encoder_process_interleaved(fEnc, fConv.data(),
                                                  (unsigned)frames)) {
        fFailed = true;
        return false;
    }
    return true;
}

bool FlacSink::Close() {
    if (!fOpen) return !fFailed;   // idempotent; a failed sink stays failed
    fOpen = false;

    bool ok = FLAC__stream_encoder_finish(fEnc);
    FLAC__stream_encoder_delete(fEnc);
    fEnc = nullptr;
    ok = ok && !fFailed;
    fFailed = !ok;
    return ok;
}

} // namespace daw
