// WavSource — decodes an audio file to interleaved stereo float via the
// Media Kit (BMediaFile/BMediaTrack). Any format the installed translators
// understand works (wav, aiff, ...), not just literal .wav.
//
// Output is always 2-channel interleaved float, regardless of the source
// channel count (mono is duplicated, >2 is downmixed to the first two),
// so the ring buffer and mixer downstream stay simple.
//
// NOTE: this does NOT sample-rate convert. If the file's rate differs from
// the engine's output rate, playback pitch/speed shifts. For milestone 2,
// use a file at the engine rate (48 kHz). Resampling is a later milestone.
//
// Haiku-only: depends on the Media Kit. Not built on non-Haiku hosts.
#pragma once

#include <MediaFile.h>
#include <MediaTrack.h>

#include <cstdint>
#include <vector>

namespace daw {

class WavSource {
public:
    WavSource() = default;
    ~WavSource();

    // Opens and prepares decoding. Returns B_OK on success.
    status_t Open(const char* path);

    bool  IsValid() const { return fTrack != nullptr; }
    float FrameRate() const { return fFrameRate; }
    int   SourceChannels() const { return fSrcChannels; }
    int64_t TotalFrames() const { return fTotalFrames; }

    // Decode the next chunk. On success sets *outStereo to an internal
    // buffer of *outFrames interleaved stereo frames (2 * frames floats)
    // and returns true. Returns false at end-of-stream (or error).
    // The returned pointer is valid until the next ReadChunk call.
    bool ReadChunk(const float** outStereo, size_t* outFrames);

private:
    BMediaFile*  fFile  = nullptr;
    BMediaTrack* fTrack = nullptr;

    float   fFrameRate   = 0.0f;
    int     fSrcChannels = 0;
    int64_t fTotalFrames = 0;

    std::vector<uint8_t> fDecodeBuf;   // native-format decode target
    std::vector<float>   fStereo;      // converted interleaved stereo out
    size_t  fDecodeFrameCap = 0;       // frames the decode buffer holds
    bool    fEnded = false;
};

} // namespace daw
