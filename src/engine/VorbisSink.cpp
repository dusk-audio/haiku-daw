#include "VorbisSink.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

namespace daw {

VorbisSink::~VorbisSink() {
    Close();
}

bool VorbisSink::WritePage(const ogg_page& page) {
    if (page.header_len > 0
        && fwrite(page.header, 1, (size_t)page.header_len, fFile)
           != (size_t)page.header_len)
        fFailed = true;
    else if (page.body_len > 0
             && fwrite(page.body, 1, (size_t)page.body_len, fFile)
                != (size_t)page.body_len)
        fFailed = true;
    return !fFailed;
}

bool VorbisSink::Open(const std::string& path, const SinkFormat& fmt) {
    if (fFile) return false;
    if (fmt.channels < 1 || fmt.channels > 8 || fmt.sampleRate < 1)
        return false;
    fChannels = fmt.channels;

    fFile = std::fopen(path.c_str(), "wb");
    if (!fFile) {
        std::fprintf(stderr, "VorbisSink: cannot create '%s'\n", path.c_str());
        return false;
    }

    // VBR quality, libvorbis' own scale (-0.1..1.0). Clamped to [0,1], which
    // is the range the export dialog offers.
    float q = fmt.quality;
    if (!(q >= 0.0f)) q = 0.0f;      // also catches NaN
    if (q > 1.0f) q = 1.0f;

    vorbis_info_init(&fVi);
    fInfoInit = true;
    if (vorbis_encode_init_vbr(&fVi, fChannels, (long)fmt.sampleRate, q) != 0) {
        std::fprintf(stderr, "VorbisSink: encoder setup failed for '%s'\n",
                     path.c_str());
        Close();
        return false;
    }
    vorbis_comment_init(&fVc);
    fCommInit = true;
    vorbis_comment_add_tag(&fVc, (char*)"ENCODER", (char*)"Haiku DAW");

    if (vorbis_analysis_init(&fVd, &fVi) != 0) { Close(); return false; }
    fDspInit = true;
    if (vorbis_block_init(&fVd, &fVb) != 0) { Close(); return false; }
    fBlockInit = true;

    // The stream serial number identifies this logical bitstream inside the Ogg
    // container; anything unique per file works.
    if (ogg_stream_init(&fOs, (int)time(nullptr)) != 0) { Close(); return false; }
    fOggInit = true;

    // The three identification/comment/setup headers, written up front.
    ogg_packet header, headerComm, headerCode;
    if (vorbis_analysis_headerout(&fVd, &fVc, &header, &headerComm,
                                  &headerCode) != 0) {
        Close();
        return false;
    }
    ogg_stream_packetin(&fOs, &header);
    ogg_stream_packetin(&fOs, &headerComm);
    ogg_stream_packetin(&fOs, &headerCode);

    ogg_page page;
    // flush (not pageout): the headers must reach the file before any audio,
    // so a reader can identify the stream without seeking.
    while (ogg_stream_flush(&fOs, &page)) {
        if (!WritePage(page)) { Close(); return false; }
    }
    return true;
}

void VorbisSink::Drain() {
    if (!fFile || !fOggInit) return;
    ogg_page page;
    // blockout returns 1 while a whole block is ready to analyse; each block
    // may yield several packets, and each packet several pages.
    while (vorbis_analysis_blockout(&fVd, &fVb) == 1) {
        vorbis_analysis(&fVb, nullptr);
        vorbis_bitrate_addblock(&fVb);
        ogg_packet op;
        while (vorbis_bitrate_flushpacket(&fVd, &op)) {
            ogg_stream_packetin(&fOs, &op);
            while (ogg_stream_pageout(&fOs, &page)) {
                if (!WritePage(page)) return;
            }
        }
    }
}

bool VorbisSink::WriteFloat(const float* interleaved, size_t sampleCount) {
    if (!fFile || fFailed || interleaved == nullptr) return false;
    const long frames = (long)(sampleCount / (size_t)fChannels);
    if (frames <= 0) return true;

    // libvorbis hands back one buffer per channel; fill it in planar order.
    float** buf = vorbis_analysis_buffer(&fVd, frames);
    if (buf == nullptr) return false;
    for (int ch = 0; ch < fChannels; ch++)
        for (long i = 0; i < frames; i++) {
            const float x = interleaved[size_t(i) * fChannels + (size_t)ch];
            // A NaN would poison the psychoacoustic model's own filters and
            // take the rest of the file with it (parity with the WAV writer's
            // pre-quantisation sanitising).
            buf[ch][i] = std::isfinite(x) ? x : 0.0f;
        }
    vorbis_analysis_wrote(&fVd, frames);
    Drain();
    return !fFailed;
}

bool VorbisSink::Close() {
    // Flush the tail first -- but only for a sink that actually opened; a
    // never-opened one just has to release nothing.
    if (fFile && fOggInit) {
        vorbis_analysis_wrote(&fVd, 0);   // end of stream
        Drain();
        ogg_page page;
        while (ogg_stream_flush(&fOs, &page))
            if (!WritePage(page)) break;
    }

    if (fFile) { std::fclose(fFile); fFile = nullptr; }
    if (fOggInit)   { ogg_stream_clear(&fOs);     fOggInit = false; }
    if (fBlockInit) { vorbis_block_clear(&fVb);   fBlockInit = false; }
    if (fDspInit)   { vorbis_dsp_clear(&fVd);     fDspInit = false; }
    if (fCommInit)  { vorbis_comment_clear(&fVc); fCommInit = false; }
    if (fInfoInit)  { vorbis_info_clear(&fVi);    fInfoInit = false; }
    // The exporter removes its temp when this says false, so a stream that
    // lost a page must not be renamed into place as a finished file.
    const bool ok = !fFailed;
    fFailed = !ok;   // stays true: a failed sink is failed for good
    return ok;
}

} // namespace daw
