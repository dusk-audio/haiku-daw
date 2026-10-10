// VorbisSink — an Ogg Vorbis writer built on libvorbisenc + libogg.
//
// The lossy sink: quality-selected VBR, so it takes no bit depth and applies no
// dither (there is nothing to dither about a signal that is about to be thrown
// away). Fed float blocks through the same IAudioSink the WAV and FLAC writers
// implement, so the exporter's progress/cancel/temp+rename path is identical.
//
// Compiled only when DAW_VORBIS found the library.
#pragma once

#include "IAudioSink.h"

#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>

#include <cstdio>

namespace daw {

class VorbisSink : public IAudioSink {
public:
    VorbisSink() = default;
    ~VorbisSink() override;

    VorbisSink(const VorbisSink&) = delete;
    VorbisSink& operator=(const VorbisSink&) = delete;

    bool Open(const std::string& path, const SinkFormat& fmt) override;
    bool WriteFloat(const float* interleaved, size_t sampleCount) override;
    bool Close() override;

private:
    // Pull complete pages out of the analyser and write them.
    void Drain();
    bool WritePage(const ogg_page& page);

    FILE*            fFile       = nullptr;
    vorbis_info      fVi         = {};
    vorbis_comment   fVc         = {};
    vorbis_dsp_state fVd         = {};
    vorbis_block     fVb         = {};
    ogg_stream_state fOs         = {};
    // Each of these guards its own teardown: a failed Open must not free
    // state it never initialised (vorbis_info_clear on a zeroed struct).
    bool fInfoInit  = false;
    bool fCommInit  = false;
    bool fDspInit   = false;
    bool fBlockInit = false;
    bool fOggInit   = false;
    // A page that would not write (a full disk, most likely) poisons the
    // stream: every later page is missing from a file that claims to be whole.
    // Close() must report that, or the exporter would rename a truncated file
    // into place and call it a successful bounce.
    bool fFailed    = false;
    int  fChannels  = 2;
};

} // namespace daw
