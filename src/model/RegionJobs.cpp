#include "RegionJobs.h"

#include "RoutingGraph.h"             // kRoutingMaster (the freeze routes there)
#include "../engine/AudioFormats.h"   // OpenAudioSource: any format, one reader
#include "../engine/WavWriter.h"

#include <unistd.h>   // access (the diagnostic below)

#include <cmath>
#include <cstdio>
#include <memory>

namespace daw {

int64_t DecodeClipRegion(const Clip& c, double projectRate,
                         std::vector<float>& out, double& outRate,
                         const RegionJob* job) {
    out.clear();
    outRate = 0.0;
    // Whatever the clip points at -- a WAV take, an imported FLAC, an AIFF.
    std::unique_ptr<IAudioSource> src = OpenAudioSource(c.sourcePath);
    if (!src) {
        fprintf(stderr, "RegionJobs: decode: cannot open '%s' (access=%d)\n",
                c.sourcePath.c_str(), (int)::access(c.sourcePath.c_str(), F_OK));
        return 0;
    }
    outRate = src->FrameRate();
    if (c.sourceOffset > 0) src->Seek(c.sourceOffset);
    // The clip plays lengthFrames project-frames == that many source-frames
    // scaled by the rate ratio, starting at sourceOffset.
    int64_t wantSrc = c.lengthFrames;
    if (outRate > 0 && projectRate > 0)
        wantSrc = (int64_t)llround((double)c.lengthFrames * outRate / projectRate);
    if (wantSrc < 0) wantSrc = 0;
    if (wantSrc > 0) out.reserve((size_t)wantSrc * 2);

    const float* chunk = nullptr;
    size_t frames = 0;
    while ((int64_t)(out.size() / 2) < wantSrc && src->ReadChunk(&chunk, &frames)) {
        if (job != nullptr && JobCancelled(*job)) {
            out.clear();
            return 0;
        }
        int64_t have = (int64_t)(out.size() / 2);
        int64_t take = wantSrc - have;
        if ((int64_t)frames > take) frames = (size_t)take;
        out.insert(out.end(), chunk, chunk + frames * 2);
        // Half the job's time is the decode, the other half is whatever the
        // caller does with the samples; this keeps the bar moving during the
        // part that can be long.
        if (job != nullptr && wantSrc > 0)
            ReportJob(*job, 0.5f * (float)((double)(out.size() / 2) / (double)wantSrc));
    }
    return (int64_t)(out.size() / 2);
}

bool MeasureClipPeak(const Clip& c, double projectRate, const RegionJob& job,
                     float* outGain) {
    std::vector<float> buf;
    double rate = 0.0;
    const int64_t n = DecodeClipRegion(c, projectRate, buf, rate, &job);
    if (n <= 0) return false;
    if (JobCancelled(job)) return false;
    ReportJob(job, 0.75f);
    const float peak = PeakLinear(buf.data(), n);
    if (peak <= 1e-6f) return false;         // silent: nothing to normalize
    float g = 1.0f / peak;
    if (g > 64.0f) g = 64.0f;                // ceiling for near-silent clips
    if (outGain) *outGain = g;
    ReportJob(job, 1.0f);
    return true;
}

bool RenderReversedClip(const Clip& c, double projectRate,
                        const std::string& outPath, const RegionJob& job) {
    std::vector<float> buf;
    double rate = 0.0;
    const int64_t n = DecodeClipRegion(c, projectRate, buf, rate, &job);
    if (n <= 0 || rate <= 0) {
        // Named, because "the job failed" with no reason is the message that
        // costs an afternoon: WHICH step and WHICH file.
        std::fprintf(stderr, "RegionJobs: reverse: cannot read '%s' (%lld "
                             "frames, rate %.0f)\n",
                     c.sourcePath.c_str(), (long long)n, rate);
        return false;
    }
    if (JobCancelled(job)) return false;
    ReverseStereo(buf.data(), n);
    ReportJob(job, 0.75f);

    // Temp + rename: a stopped or failed job must leave nothing where a
    // finished file is expected (the export's own rule).
    const std::string part = outPath + ".part";
    std::remove(part.c_str());
    std::vector<int16_t> pcm((size_t)n * 2);
    for (int64_t i = 0; i < n * 2; ++i) {
        float s = buf[(size_t)i];
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[(size_t)i] = (int16_t)lround(s * 32767.0f);
    }
    WavWriter w;
    // The .part name is ours: no exclusive flag (a leftover from a crashed run
    // is overwritten rather than turned into a refusal).
    if (!w.Open(part, (int)lround(rate), 2, /*exclusive*/ false)
        || !w.WriteInt16(pcm.data(), pcm.size()) || !w.Close()) {
        std::fprintf(stderr, "RegionJobs: reverse: cannot write '%s'\n",
                     part.c_str());
        std::remove(part.c_str());
        return false;
    }
    if (JobCancelled(job)) {
        std::remove(part.c_str());
        return false;
    }
    if (std::rename(part.c_str(), outPath.c_str()) != 0) {
        std::remove(part.c_str());
        return false;
    }
    ReportJob(job, 1.0f);
    return true;
}

bool AnalyzeClipSilence(const Clip& c, double projectRate,
                        const StripSilenceParams& params,
                        std::vector<Span>* spans, double* outSourceRate,
                        const RegionJob& job) {
    std::vector<float> buf;
    double rate = 0.0;
    const int64_t n = DecodeClipRegion(c, projectRate, buf, rate, &job);
    if (n <= 0 || rate <= 0) return false;
    if (outSourceRate) *outSourceRate = rate;
    if (JobCancelled(job)) return false;
    const float   thresh = params.threshold;
    const int64_t minSil = (int64_t)(params.minSilenceSec * rate);
    const int64_t pad    = (int64_t)(params.padSec * rate);
    std::vector<Span> found = NonSilentSpans(buf.data(), n, thresh, minSil, pad);
    ReportJob(job, 0.9f);
    if (JobCancelled(job)) return false;
    if (spans) *spans = std::move(found);
    ReportJob(job, 1.0f);
    return true;
}

bool RenderTrackToWav(const Project& src, TrackId id, const std::string& out,
                      const ExportJob* job) {
    const Track* t = src.FindTrack(id);
    if (!t) return false;
    Project iso;
    iso.sampleRate = src.sampleRate;
    iso.tempoBPM   = src.tempoBPM;
    iso.timeSig    = src.timeSig;
    iso.tempoMap   = src.tempoMap;
    Track copy = *t;
    copy.output = kRoutingMaster;      // straight to the master sum
    copy.sends.clear();
    copy.muted = false;
    copy.soloed = false;
    copy.frozen = false;
    copy.freezeClips.clear();
    copy.freezeMidi.clear();
    copy.freezeFx.clear();
    iso.AddTrack(copy);
    ExportOptions opts;                // the historical bounce format
    opts.job = job;                    // progress + cancel, when the caller has them
    return ExportWav(iso, out, src.sampleRate, opts);
}

} // namespace daw
