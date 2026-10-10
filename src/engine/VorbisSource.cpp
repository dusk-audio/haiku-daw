#include "VorbisSource.h"

#include <cstdio>
#include <cstring>

namespace daw {

namespace {
// One decoded block per ReadChunk, matching WavSource's 8192 frames.
constexpr int kReadFrames = 8192;
} // namespace

VorbisSource::~VorbisSource() {
    if (fOpen) ov_clear(&fVf);
}

bool VorbisSource::Open(const std::string& path) {
    if (ov_fopen(const_cast<char*>(path.c_str()), &fVf) != 0) {
        std::fprintf(stderr, "VorbisSource: cannot open '%s'\n", path.c_str());
        return false;
    }
    fOpen = true;

    // The last logical bitstream's info: an Ogg file may chain several
    // (Vorbis streams back to back), and the chain is what plays.
    vorbis_info* vi = ov_info(&fVf, -1);
    if (!vi) {
        std::fprintf(stderr, "VorbisSource: '%s' has no Vorbis stream\n",
                     path.c_str());
        return false;
    }
    fChannels   = vi->channels;
    fSampleRate = float(vi->rate);

    // -1 = unknown (an unseekable or chopped stream). We cannot invent a
    // frame count -- callers size clip lengths from it -- so the file is
    // refused rather than advertised with a wrong length. Every encoder that
    // writes a file header to a seekable output records the count.
    const ogg_int64_t total = ov_pcm_total(&fVf, -1);
    if (total < 0) {
        std::fprintf(stderr, "VorbisSource: '%s' does not declare its length\n",
                     path.c_str());
        return false;
    }
    fTotalFrames = int64_t(total);

    if (fChannels < 1 || fChannels > 64 || fSampleRate <= 0.0f
        || fTotalFrames <= 0) {
        std::fprintf(stderr, "VorbisSource: unusable header "
                     "(ch=%d rate=%g frames=%lld)\n", fChannels,
                     double(fSampleRate), (long long)fTotalFrames);
        return false;
    }

    fValid = true;
    return true;
}

bool VorbisSource::Seek(int64_t frame) {
    if (!fValid)
        return false;
    if (frame < 0) frame = 0;
    if (frame > fTotalFrames) frame = fTotalFrames;

    // "One past the last frame" is a position callers legitimately seek to
    // (WavSource accepts it and reads nothing back). Park the cursor rather
    // than asking libvorbisfile to decode from a sample that does not exist;
    // a later seek into the file clears it.
    if (frame >= fTotalFrames) {
        fEof = true;
        fPastEnd = true;
        return true;
    }
    fPastEnd = false;

    // ov_pcm_seek lands on `frame` exactly (it decodes and discards within the
    // target page), which is what the engine's per-clip pre-seek needs.
    if (ov_pcm_seek(&fVf, static_cast<ogg_int64_t>(frame)) != 0)
        return false;
    fEof = false;
    return true;
}

bool VorbisSource::ReadChunk(const float** outStereo, size_t* outFrames) {
    if (!fValid || fEof)
        return false;

    float** pcm = nullptr;
    int bitstream = 0;
    const long got = ov_read_float(&fVf, &pcm, kReadFrames, &bitstream);
    if (got <= 0) {
        // 0 = end of stream, negative = a hole in the data. Either way there
        // is nothing more to hand out; the frames already delivered stand.
        fEof = true;
        return false;
    }

    fStereo.resize(size_t(got) * 2);
    for (long i = 0; i < got; i++) {
        const float l = pcm[0][i];
        // Mono duplicates, >2 channels take the first two: identical widening
        // to WavSource, so the mix sounds the same whatever the container.
        const float r = (fChannels == 1) ? l : pcm[1][i];
        fStereo[size_t(i) * 2 + 0] = l;
        fStereo[size_t(i) * 2 + 1] = r;
    }

    *outStereo = fStereo.data();
    *outFrames = size_t(got);
    return true;
}

} // namespace daw
