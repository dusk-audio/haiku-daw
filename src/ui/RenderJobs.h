// RenderJobs — export, freeze and the region ops, moved out of MainWindow
// (M1.1, slice 4): the snapshot, the worker thread, the progress window's
// messenger and the cancel flag. The dialog itself (ExportWindow and the
// window's remembered choices) stays with the window; this is the work.
#pragma once

#include "../model/Project.h"

#include <Messenger.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace daw {

class MainWindow;

// The frozen-track renderer (defined in MainWindow.cpp; it uses the exporter).
bool RenderTrackToWav(const Project& src, TrackId id, const std::string& out);

class RenderJobs {
public:
    void SetWindow(MainWindow* win) { fWin = win; }

    // The window's render work, body for body (M1.1).
    void StartExport(const char* path, bool stems);
    void FinishExport();                            // pulse: report, close bar
    void FreezeTrack(TrackId track, bool freeze);
    void RegionNormalize(TrackId track, ClipId clip);
    void RegionReverse(TrackId track, ClipId clip);
    void RegionStripSilence(TrackId track, ClipId clip);
    int64_t DecodeClipRegion(const Clip& c, std::vector<float>& out,
                             double& outRate);
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

private:
    MainWindow* fWin = nullptr;
};

} // namespace daw
