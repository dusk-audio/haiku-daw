#include "WavSource.h"

#include <Entry.h>          // get_ref_for_path, entry_ref
#include <MediaDefs.h>

#include <cstdio>
#include <cstring>

namespace daw {

WavSource::~WavSource() {
    if (fFile) {
        if (fTrack)
            fFile->ReleaseTrack(fTrack);
        delete fFile;   // BMediaFile owns/closes itself
    }
}

status_t WavSource::Open(const char* path) {
    entry_ref ref;
    status_t err = get_ref_for_path(path, &ref);
    if (err != B_OK) {
        fprintf(stderr, "WavSource: bad path '%s': %s\n", path, strerror(err));
        return err;
    }

    fFile = new BMediaFile(&ref);
    err = fFile->InitCheck();
    if (err != B_OK) {
        fprintf(stderr, "WavSource: BMediaFile init failed: %s\n", strerror(err));
        return err;
    }

    // Find the first audio track in the file.
    int32 count = fFile->CountTracks();
    for (int32 i = 0; i < count; i++) {
        BMediaTrack* t = fFile->TrackAt(i);
        if (!t) continue;
        media_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        if (t->EncodedFormat(&fmt) == B_OK && fmt.IsAudio()) {
            fTrack = t;
            break;
        }
        fFile->ReleaseTrack(t);
    }
    if (!fTrack) {
        fprintf(stderr, "WavSource: no audio track in '%s'\n", path);
        return B_ERROR;
    }

    // Ask the decoder for raw float in the file's native channel/rate.
    media_format dec;
    memset(&dec, 0, sizeof(dec));
    dec.type = B_MEDIA_RAW_AUDIO;
    dec.u.raw_audio = media_raw_audio_format::wildcard;
    dec.u.raw_audio.format     = media_raw_audio_format::B_AUDIO_FLOAT;
    dec.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;

    err = fTrack->DecodedFormat(&dec);
    if (err != B_OK) {
        fprintf(stderr, "WavSource: DecodedFormat failed: %s\n", strerror(err));
        return err;
    }

    fFrameRate   = dec.u.raw_audio.frame_rate;
    fSrcChannels = dec.u.raw_audio.channel_count;
    fTotalFrames = fTrack->CountFrames();

    if (fSrcChannels < 1) {
        fprintf(stderr, "WavSource: bad channel count %d\n", fSrcChannels);
        return B_ERROR;
    }

    // Size the decode buffer from the decoder's preferred buffer_size, with
    // a sane fallback. frameSize = channels * sizeof(float).
    const size_t frameSize = fSrcChannels * sizeof(float);
    size_t bufBytes = dec.u.raw_audio.buffer_size;
    if (bufBytes < frameSize)
        bufBytes = 4096 * frameSize;
    fDecodeFrameCap = bufBytes / frameSize;
    fDecodeBuf.resize(fDecodeFrameCap * frameSize);
    fStereo.resize(fDecodeFrameCap * 2);

    return B_OK;
}

bool WavSource::ReadChunk(const float** outStereo, size_t* outFrames) {
    if (!fTrack || fEnded)
        return false;

    int64 frameCount = fDecodeFrameCap;
    media_header mh;
    status_t err = fTrack->ReadFrames(fDecodeBuf.data(), &frameCount, &mh);

    if (frameCount <= 0) {
        fEnded = true;
        return false;
    }
    // B_LAST_BUFFER_ERROR: this is the final valid chunk; deliver it, end next.
    if (err == B_LAST_BUFFER_ERROR)
        fEnded = true;
    else if (err != B_OK) {
        fEnded = true;
        return false;
    }

    // Convert native-channel float -> interleaved stereo float.
    const float* in = reinterpret_cast<const float*>(fDecodeBuf.data());
    const int ch = fSrcChannels;
    for (int64 f = 0; f < frameCount; f++) {
        float l, r;
        if (ch == 1) {
            l = r = in[f];
        } else {
            l = in[f * ch + 0];
            r = in[f * ch + 1];   // ignore channels beyond the first two
        }
        fStereo[f * 2 + 0] = l;
        fStereo[f * 2 + 1] = r;
    }

    *outStereo = fStereo.data();
    *outFrames = static_cast<size_t>(frameCount);
    return true;
}

} // namespace daw
