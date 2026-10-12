// RegionJobs — the work behind the clip region operations (Normalize,
// Reverse, Strip Silence) and Freeze, as functions that can run on a WORKER
// thread (M4.4). Kit-free, like the rest of src/model: the readers, the writers
// and the exporter are all in this library already, so the whole job is
// host-testable and the UI's worker has nothing to do but call it.
//
// Each entry point takes a RegionJob: the cancellation flag it polls and the
// progress callback it reports through (the ExportJob shape, deliberately — one
// kind of long job in this app). Neither part may touch a Haiku object: the
// callback runs on the worker thread and does nothing but store a float for the
// window's own looper to read.
//
// The rule the export established holds here too: a cancelled or failed job
// leaves NOTHING behind. RenderReversedClip writes to <path>.part and renames
// on success, so a stopped job cannot leave a file a project could later
// reference.
#pragma once

#include "Project.h"          // Project, Track, Clip, Frame
#include "RegionOps.h"        // Span (the silence analysis' output)
#include "../engine/Exporter.h"   // ExportJob (the freeze render IS an export)

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace daw {

// Progress + cancellation for a long region job. Both parts are optional.
struct RegionJob {
    std::function<void(float)> progress;   // [0,1], worker thread, cheap
    const std::atomic<bool>*   cancel = nullptr;
};

inline bool JobCancelled(const RegionJob& job) {
    return job.cancel != nullptr
        && job.cancel->load(std::memory_order_relaxed);
}

inline void ReportJob(const RegionJob& job, float v) {
    if (job.progress) job.progress(v);
}

// What Strip Silence cuts with. The window's own numbers, named so the caller
// and the tests cannot disagree about them.
struct StripSilenceParams {
    float  threshold     = 0.00316f;   // ~ -50 dBFS
    double minSilenceSec = 0.25;       // a gap must be this long to be a cut
    double padSec        = 0.02;       // air kept either side of a cut
};

// Decode a clip's played region to interleaved-stereo float at the SOURCE rate.
// Returns frames decoded (0 on failure, or when the job was cancelled); sets
// outRate to the file's rate. `projectRate` converts the clip's project-frame
// length into source frames.
int64_t DecodeClipRegion(const Clip& c, double projectRate,
                         std::vector<float>& out, double& outRate,
                         const RegionJob* job = nullptr);

// Normalize: decode the clip and report the gain that brings its peak to
// unity (ceilinged at 64x for near-silent material). False when the clip
// cannot be read, is silent, or the job was cancelled.
bool MeasureClipPeak(const Clip& c, double projectRate, const RegionJob& job,
                     float* outGain);

// Reverse: decode, reverse, and write a new WAV at the source rate. The file
// lands at `outPath` only if the whole job succeeded.
bool RenderReversedClip(const Clip& c, double projectRate,
                        const std::string& outPath, const RegionJob& job);

// Strip Silence: decode and report the source-frame spans worth keeping.
// `outSourceRate` is the decoded file's rate — the spans are in SOURCE frames,
// so the caller needs it to place the resulting clips on the timeline.
bool AnalyzeClipSilence(const Clip& c, double projectRate,
                        const StripSilenceParams& params,
                        std::vector<Span>* spans, double* outSourceRate,
                        const RegionJob& job);

// Freeze: render one track (its clips or its instrument, through its own fx
// chain and fader) to a WAV, straight to the master. `job` carries the
// exporter's own progress and cancellation (it IS an export of one track).
bool RenderTrackToWav(const Project& src, TrackId id, const std::string& out,
                      const ExportJob* job = nullptr);

} // namespace daw
