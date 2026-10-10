// FlacSource — a FLAC reader built on libFLAC's stream decoder.
//
// Compiled only when DAW_FLAC found the library (CMakeLists.txt defines
// DAW_HAVE_FLAC); with it absent the format simply is not offered. Behind
// IAudioSource it is indistinguishable from WavSource to the engine, the peak
// cache and the exporter, which is the point: those never learn what container
// they are reading.
//
// Seeking goes through FLAC__stream_decoder_seek_absolute, i.e. the codec's
// own sample-accurate seek rather than a byte-offset guess, so a clip whose
// playhead is seeked into the middle of a compressed file lands on the right
// sample. Decoding is pull-based: a block is decoded at a time into a small
// interleaved-float queue and handed out in chunks, so nothing buffers a whole
// file.
//
// Kit-free (std C++ + libFLAC) so it builds and unit-tests on the host.
#pragma once

#include "IAudioSource.h"

#include <FLAC/stream_decoder.h>

#include <string>
#include <vector>

namespace daw {

class FlacSource : public IAudioSource {
public:
    FlacSource() = default;
    ~FlacSource() override;

    FlacSource(const FlacSource&) = delete;
    FlacSource& operator=(const FlacSource&) = delete;

    // Parse the header (STREAMINFO). Returns true on success.
    bool Open(const std::string& path);

    // --- IAudioSource ---
    bool    IsValid() const override { return fValid; }
    float   FrameRate() const override { return fSampleRate; }
    int     SourceChannels() const override { return fChannels; }
    int64_t TotalFrames() const override { return fTotalFrames; }
    bool    Seek(int64_t frame) override;
    bool    ReadChunk(const float** outStereo, size_t* outFrames) override;

    // Bits per sample the stream declares (8/12/16/20/24/32).
    int BitsPerSample() const { return fBits; }

private:
    // libFLAC callbacks (C function pointers: no captures, `client_data` is
    // the FlacSource).
    static FLAC__StreamDecoderWriteStatus WriteCb(
        const FLAC__StreamDecoder* dec, const FLAC__Frame* frame,
        const FLAC__int32* const buffer[], void* client);
    static void MetaCb(const FLAC__StreamDecoder* dec,
                       const FLAC__StreamMetadata* meta, void* client);
    static void ErrorCb(const FLAC__StreamDecoder* dec,
                        FLAC__StreamDecoderErrorStatus status, void* client);

    // Pull decoded frames until `kReadFrames` are queued or the stream ends.
    // Returns the number of frames ready to hand out.
    size_t Fill();

    FLAC__StreamDecoder* fDec = nullptr;
    bool    fValid       = false;
    bool    fEof         = false;    // the decoder reported end of stream
    bool    fPastEnd     = false;    // cursor parked on/past the last frame
    bool    fFailed      = false;    // a hard decoder error: stop reading
    bool    fGotInfo     = false;    // STREAMINFO arrived (see MetaCb)
    int     fChannels    = 0;
    float   fSampleRate  = 0.0f;
    int     fBits        = 0;
    int64_t fTotalFrames = 0;
    int64_t fPosition    = 0;        // source frame the queue's FIRST frame is

    std::vector<float> fQueue;       // decoded interleaved stereo, pending
    size_t  fQueueFrames = 0;        // frames in fQueue from fQueueFrame0 on
    int64_t fQueueFrame0 = 0;        // source frame index of fQueue[0]
    std::vector<float> fStereo;      // the returned block
};

} // namespace daw
