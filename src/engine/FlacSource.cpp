#include "FlacSource.h"

#include <cmath>
#include <cstdio>

namespace daw {

namespace {
// How many frames a single ReadChunk hands out, and therefore how much is kept
// queued: 8192 matches WavSource's block so downstream buffering is unchanged.
constexpr size_t kReadFrames = 8192;

// One decoded FLAC sample (signed, `bits` wide) as a float in [-1, 1). The
// FLAC format is fixed-point, so this is the same normalisation WavSource
// applies to a PCM WAV of the same depth.
inline float ToFloat(FLAC__int32 v, int bits) {
    if (bits <= 0 || bits > 32) return 0.0f;
    return float(double(v) / std::ldexp(1.0, bits - 1));
}
} // namespace

FlacSource::~FlacSource() {
    if (fDec) FLAC__stream_decoder_delete(fDec);
}

FLAC__StreamDecoderWriteStatus FlacSource::WriteCb(
    const FLAC__StreamDecoder*, const FLAC__Frame* frame,
    const FLAC__int32* const buffer[], void* client) {
    FlacSource* self = static_cast<FlacSource*>(client);
    const unsigned frames = frame->header.blocksize;
    const unsigned ch     = frame->header.channels;
    if (ch == 0 || buffer == nullptr)
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;

    self->fQueue.reserve(self->fQueue.size() + frames * 2);
    for (unsigned i = 0; i < frames; i++) {
        const float l = ToFloat(buffer[0][i], self->fBits);
        // Mono is duplicated, >2 channels take the first two -- the same
        // widening WavSource does, so a stereo clip sounds identical whichever
        // container it came from.
        const float r = (ch == 1) ? l : ToFloat(buffer[1][i], self->fBits);
        self->fQueue.push_back(l);
        self->fQueue.push_back(r);
    }
    if (self->fQueueFrames == 0)
        self->fQueueFrame0 = self->fPosition;
    self->fQueueFrames += frames;
    self->fPosition    += frames;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void FlacSource::MetaCb(const FLAC__StreamDecoder*,
                        const FLAC__StreamMetadata* meta, void* client) {
    if (meta == nullptr || meta->type != FLAC__METADATA_TYPE_STREAMINFO)
        return;
    // STREAMINFO is the authority here. The decoder's own
    // get_channels/get_sample_rate/get_bits_per_sample accessors report the
    // most recently DECODED FRAME's header (libFLAC documents them as "only
    // valid after decoding has started"), so reading them straight after the
    // metadata pass returns zeros and every file looks broken.
    FlacSource* self = static_cast<FlacSource*>(client);
    self->fChannels    = int(meta->data.stream_info.channels);
    self->fSampleRate  = float(meta->data.stream_info.sample_rate);
    self->fBits        = int(meta->data.stream_info.bits_per_sample);
    self->fTotalFrames = int64_t(meta->data.stream_info.total_samples);
    self->fGotInfo     = true;
}

void FlacSource::ErrorCb(const FLAC__StreamDecoder*,
                         FLAC__StreamDecoderErrorStatus, void*) {
    // Non-fatal by libFLAC's definition (a lost sync frame, a bad CRC on one
    // frame). A stream that is really broken makes process_single/…_stream
    // return false, which is where we stop; a single dropped frame should not
    // take the file down.
}

bool FlacSource::Open(const std::string& path) {
    fDec = FLAC__stream_decoder_new();
    if (!fDec) {
        std::fprintf(stderr, "FlacSource: cannot create a decoder\n");
        return false;
    }
    // The md5 in STREAMINFO is a nice integrity check for a whole-file decode
    // and a wasted pass for streaming playback, where we may stop early and
    // never finish the stream at all.
    FLAC__stream_decoder_set_md5_checking(fDec, false);

    const FLAC__StreamDecoderInitStatus st = FLAC__stream_decoder_init_file(
        fDec, path.c_str(), &FlacSource::WriteCb, &FlacSource::MetaCb,
        &FlacSource::ErrorCb, this);
    if (st != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        std::fprintf(stderr, "FlacSource: cannot open '%s' (%s)\n", path.c_str(),
                     FLAC__StreamDecoderInitStatusString[st]);
        FLAC__stream_decoder_delete(fDec);
        fDec = nullptr;
        return false;
    }

    // libFLAC reads STREAMINFO lazily, and delivers it through MetaCb -- which
    // is where the fields below come from. Without this pass every accessor
    // reads back zero and every file would be rejected.
    if (!FLAC__stream_decoder_process_until_end_of_metadata(fDec) || !fGotInfo) {
        std::fprintf(stderr, "FlacSource: '%s' has no readable STREAMINFO\n",
                     path.c_str());
        return false;
    }

    if (fChannels < 1 || fChannels > 64 || fBits < 1 || fBits > 32
        || fSampleRate <= 0.0f) {
        std::fprintf(stderr, "FlacSource: unusable STREAMINFO "
                     "(ch=%d bits=%d rate=%g)\n", fChannels, fBits,
                     double(fSampleRate));
        return false;
    }

    // A stream whose total-sample count is unknown (STREAMINFO says 0: only a
    // live encode does that) still has to report a truthful TotalFrames,
    // because callers size clip lengths from it. Decode it once to count, then
    // rewind. Rare enough that the extra pass costs nothing in practice.
    if (fTotalFrames <= 0) {
        if (!FLAC__stream_decoder_process_until_end_of_stream(fDec)) {
            std::fprintf(stderr, "FlacSource: '%s' will not decode\n", path.c_str());
            return false;
        }
        fTotalFrames = fPosition;
        fQueue.clear();
        fQueueFrames = 0;
        fQueueFrame0 = 0;
        fPosition = 0;
        fEof = false;
        if (fTotalFrames <= 0 || !FLAC__stream_decoder_seek_absolute(fDec, 0)) {
            std::fprintf(stderr, "FlacSource: '%s' has no audio\n", path.c_str());
            return false;
        }
    } else {
        // Drop whatever the metadata pass decoded (it should be nothing) and
        // start the queue at the beginning of the stream.
        fQueue.clear();
        fQueueFrames = 0;
        fQueueFrame0 = 0;
        fPosition = 0;
    }

    fValid = true;
    return true;
}

bool FlacSource::Seek(int64_t frame) {
    if (!fValid)
        return false;
    if (frame < 0) frame = 0;
    if (fTotalFrames > 0 && frame > fTotalFrames) frame = fTotalFrames;

    // Anything queued is about to be wrong, whichever way the seek lands.
    fQueue.clear();
    fQueueFrames = 0;
    fQueueFrame0 = 0;

    // "One past the last frame" is a position a caller legitimately seeks to
    // (WavSource accepts it and simply reads nothing), but it is not a sample
    // libFLAC can decode FROM -- seek_absolute refuses the end of the stream.
    // Park the cursor instead: every read from here reports end of data, and a
    // later seek back into the file clears it.
    if (fTotalFrames > 0 && frame >= fTotalFrames) {
        fPastEnd = true;
        fPosition = fTotalFrames;
        fEof = true;
        return true;
    }
    fPastEnd = false;

    // The codec's own seek: it resolves the target's frame offset from the
    // seek table and decodes from exactly `frame`.
    if (!FLAC__stream_decoder_seek_absolute(
            fDec, static_cast<FLAC__uint64>(frame))) {
        fFailed = true;
        return false;
    }
    fPosition = frame;
    fEof = false;
    return true;
}

size_t FlacSource::Fill() {
    while (fQueueFrames < kReadFrames && !fEof && !fFailed) {
        if (!FLAC__stream_decoder_process_single(fDec)) {
            fFailed = true;
            break;
        }
        // process_single returns true at end of stream too, having delivered
        // nothing -- that is the only way to tell "done" from "more coming".
        if (FLAC__stream_decoder_get_state(fDec)
            == FLAC__STREAM_DECODER_END_OF_STREAM)
            fEof = true;
    }
    return fQueueFrames;
}

bool FlacSource::ReadChunk(const float** outStereo, size_t* outFrames) {
    if (!fValid || fPastEnd)
        return false;

    size_t have = Fill();
    if (have == 0)
        return false;

    if (have > kReadFrames) have = kReadFrames;
    fStereo.resize(have * 2);
    // fQueue holds exactly one queued run starting at fQueueFrame0; the read
    // cursor is its front.
    for (size_t i = 0; i < have * 2; i++)
        fStereo[i] = fQueue[i];

    // Drop what was handed out (erase, not a cursor: the queue is at most
    // kReadFrames + one decoded block, so this is a cheap memmove).
    const size_t droppedFloats = have * 2;
    if (droppedFloats >= fQueue.size()) {
        fQueue.clear();
    } else {
        fQueue.erase(fQueue.begin(), fQueue.begin() + droppedFloats);
    }
    fQueueFrame0 += int64_t(have);
    fQueueFrames -= have;

    *outStereo = fStereo.data();
    *outFrames = have;
    return true;
}

} // namespace daw
