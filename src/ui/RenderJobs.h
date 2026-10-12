// RenderJobs — export, freeze and the region ops, moved out of MainWindow
// (M1.1, slice 4): the snapshot, the worker thread, the progress window's
// messenger and the cancel flag. The dialog itself (ExportWindow and the
// window's remembered choices) stays with the window; this is the work.
#pragma once

#include "../model/Project.h"
#include "../model/RegionJobs.h"   // RegionJob (the worker's cancel + progress)

#include <Messenger.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace daw {

class MainWindow;

class RenderJobs {
public:
    void SetWindow(MainWindow* win) { fWin = win; }

    // The window's render work, body for body (M1.1).
    void StartExport(const char* path, bool stems);
    void FinishExport();                            // pulse: report, close bar
    // Freeze unfreezes synchronously (a pure model restore); freezing itself
    // is a job, below.
    void FreezeTrack(TrackId track, bool freeze);

    // --- region jobs (M4.4) ------------------------------------------------
    // Freeze, Reverse, Normalize and Strip Silence decode off the window
    // thread, through the export's own worker/progress/cancel shape. The
    // snapshot is taken on the looper; the worker only reads it. FinishRegionJob
    // runs on the looper from the pulse and is the ONLY place the model is
    // touched — and only when the job succeeded, so a cancelled or failed job
    // leaves the project exactly as it was.
    enum class RegionOp { Freeze, Normalize, Reverse, StripSilence };
    void StartRegionJob(RegionOp op, TrackId track, ClipId clip = kInvalidClipId);
    void FinishRegionJob();       // pulse: apply the result, close the bar
    bool RegionJobRunning() const { return fRegionRunning.load(); }
    bool RegionJobHandled() const { return fRegionHandled; }

    std::string RenderPath(const std::string& tag) const;

    // Public members on purpose (pure move; the pulse reads the atomics).
    std::unique_ptr<Project>  fExportSnapshot;   // what the worker renders
    std::thread               fExportThread;
    std::atomic<bool>         fExportRunning{false};
    std::atomic<bool>         fExportCancel{false};
    std::atomic<float>        fExportProgress{-1.0f};
    bool                      fExportOk = false;
    int                       fExportWritten = 0;
    bool                      fExportIsStems = false;
    bool                      fExportHandled = true;
    std::string               fExportPath;
    BMessenger                fExportProgMsgr;   // the progress window

    // One region job at a time, the export's shape (M4.4).
    std::thread               fRegionThread;
    std::atomic<bool>         fRegionRunning{false};
    std::atomic<bool>         fRegionCancel{false};
    std::atomic<float>        fRegionProgress{-1.0f};
    bool                      fRegionHandled = true;
    bool                      fRegionOk = false;
    RegionOp                  fRegionOp = RegionOp::Normalize;
    TrackId                   fRegionTrack = kInvalidTrackId;
    ClipId                    fRegionClip  = kInvalidClipId;
    BMessenger                fRegionProgMsgr;    // its progress window
    std::string               fRegionPath;        // what the worker wrote
    std::string               fRegionLabel;       // what the bar says it is
    float                     fRegionGain = 1.0f; // Normalize
    std::vector<daw::Span>    fRegionSpans;       // Strip Silence
    double                    fRegionSourceRate = 0.0;

    // The worker's inputs, snapshotted on the looper: the clip by value (and
    // whether there is one), the project rate, and -- for a freeze -- a copy of
    // the whole project, because that render reads clips, the instrument and
    // the chain.
    std::unique_ptr<Project>  fRegionSnapshot;
    Clip                      fRegionClipCopy;      // the clip the worker reads
    double                    fRegionProjectRate = 48000.0;
    ClipId                    fRegionClipId = kInvalidClipId;   // re-found later

    // The ExportJob the freeze render reports through: the exporter's own
    // progress/cancel wired to this job's atomics.
    ExportJob FreezeExportJob() {
        ExportJob j;
        j.cancel = &fRegionCancel;
        j.progress = [this](float v) {
            fRegionProgress.store(v, std::memory_order_relaxed);
        };
        return j;
    }

private:
    MainWindow* fWin = nullptr;
};

} // namespace daw
