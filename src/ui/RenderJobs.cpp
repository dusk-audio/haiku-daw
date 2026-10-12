// RenderJobs — see the header. The bodies are the window's, body for body.
#include "RenderJobs.h"

#include "MainWindow.h"
#include "EffectsWindow.h"
#include "ExportProgressWindow.h"

#include <Alert.h>

#include "TimelineView.h"
#include "../model/Commands.h"
#include "../model/RegionOps.h"
#include "../model/TakeNames.h"
#include "../engine/Exporter.h"
#include "../engine/WavSource.h"
#include "../engine/AudioFormats.h"   // OpenAudioSource: any format, one reader
#include "../engine/WavWriter.h"
#include "../engine/Resampler.h"

#include <cstdio>
#include <cmath>

namespace daw {

void RenderJobs::StartExport(const char* path, bool stems) {

    if (!path || !path[0]) return;
    if (fExportThread.joinable()) return;   // one at a time (a bar is up)

    fWin->fTransportCtl.StopPlayback();
    fWin->FlushFxEditors();

    fExportSnapshot = std::make_unique<Project>(*fWin->fProject);
    const std::string outPath(path);
    fExportPath    = outPath;
    fExportIsStems = stems;
    fExportWritten = 0;
    fExportOk      = false;
    fExportHandled = false;
    fExportProgress.store(-1.0f, std::memory_order_relaxed);
    fExportCancel.store(false, std::memory_order_relaxed);

    const ExportChoices c = fWin->fExportChoices;
    const double rate = c.sampleRate > 0 ? (double)c.sampleRate : 0.0;
    ExportOptions opts;
    opts.format    = ExportFormat{ c.bitDepth, c.dither, c.container,
                                   c.vorbisQuality };
    opts.normalize = ExportNormalize{ c.normalize, c.targetLufs, c.truePeak,
                                      c.limiter };
    opts.range     = ExportRange{};   // whole project unless the loop is asked
    if (c.range == 1 && fWin->fProject->transport.loopEnabled
        && fWin->fProject->transport.loopEnd > fWin->fProject->transport.loopStart)
        opts.range = ExportRange{ fWin->fProject->transport.loopStart,
                                  fWin->fProject->transport.loopEnd };
    Project* snap = fExportSnapshot.get();

    // The bar goes up BEFORE the thread starts, so a very short export cannot
    // finish and be handled before there is anything to close. (It is created
    // only now: the flush above can block the looper for a moment, and a
    // visible "exporting" must never precede a settled model.)
    BRect wr(240, 240, 240 + 320, 240 + 96);
    ExportProgressWindow* w = new ExportProgressWindow(
        wr, BMessenger(fWin), stems ? "Rendering stems" : "Rendering mix");
    fExportProgMsgr = BMessenger(w);
    w->Show();

    fExportRunning.store(true, std::memory_order_release);
    fExportThread = std::thread([this, snap, stems, rate, opts, outPath]() {
        ExportOptions o = opts;
        ExportJob job;
        job.cancel = &fExportCancel;
        job.progress = [this](float v) {
            fExportProgress.store(v, std::memory_order_relaxed);
        };
        o.job = &job;
        if (stems) {
            fExportWritten = ExportStems(*snap, outPath, rate, o);
            fExportOk = fExportWritten > 0;
        } else {
            fExportOk = ExportWav(*snap, outPath, rate, o);
        }
        fExportRunning.store(false, std::memory_order_release);
    });
    fWin->UpdatePulse();
}

void RenderJobs::FinishExport() {

    fExportHandled = true;
    if (fExportProgMsgr.IsValid()) {
        BMessage q(B_QUIT_REQUESTED);
        fExportProgMsgr.SendMessage(&q);
        fExportProgMsgr = BMessenger();
    }
    if (fExportThread.joinable()) fExportThread.join();
    fExportSnapshot.reset();

    // Cancellation is checked FIRST: a stems run that was stopped after some
    // stems were written still reports a count, and that is a cancel, not a
    // finished export.
    if (fExportCancel.load()) {
        std::fprintf(stderr, "MainWindow: export cancelled: %s\n",
                     fExportPath.c_str());
    } else if (fExportOk) {
        if (fExportIsStems)
            std::fprintf(stderr, "MainWindow: exported %d stem(s) to %s\n",
                         fExportWritten, fExportPath.c_str());
        else
            std::fprintf(stderr, "MainWindow: exported %s\n",
                         fExportPath.c_str());
    } else {
        std::fprintf(stderr, "MainWindow: export failed: %s\n",
                     fExportPath.c_str());
        // Only a real failure is worth an alert: a cancel is the user's own
        // doing, and it already said so on stderr.
        BAlert* a = new ThemedAlertWindow("Export", "The export failed. Nothing was "
                               "written to that name.", "OK", nullptr, nullptr,
                               B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        a->Go(nullptr);   // async, like the other warnings
    }
    fWin->UpdatePulse();
}

// Freeze/Unfreeze. Unfreezing is a pure model restore and stays synchronous;
// freezing renders the track and is a JOB (M4.4), because that render is the
// long part — an embed of a 3-minute track froze the window for seconds.
void RenderJobs::FreezeTrack(TrackId track, bool freeze) {
    Track* t = fWin->fProject->FindTrack(track);
    if (!t) return;
    if (!freeze) {                     // unfreeze: pure model restore
        if (!t->frozen) return;
        fWin->fStack->Execute(std::make_unique<FreezeTrackCommand>(track, false),
                        *fWin->fProject);
        fWin->RebuildPeaks();
        fWin->fTimeline->Invalidate();
        return;
    }
    StartRegionJob(RegionOp::Freeze, track);
}

// --- region jobs (M4.4) ----------------------------------------------------

void RenderJobs::StartRegionJob(RegionOp op, TrackId track, ClipId clip) {
    // One job at a time, whichever kind: a second request while one runs is
    // refused (the caller says so in the status line), and the export is the
    // same job family.
    if (fRegionThread.joinable() || fExportThread.joinable()) {
        fWin->SetTransportStatus("A render is already running");
        return;
    }

    const Track* t = fWin->fProject->FindTrack(track);
    if (!t) return;
    const Clip* c = (clip != kInvalidClipId) ? t->FindClip(clip) : nullptr;
    if (op != RegionOp::Freeze && c == nullptr) return;
    if (op == RegionOp::Freeze && t->frozen) return;

    // A freeze renders the track from the model, so an editor gesture still
    // inside its debounce has to land first -- otherwise the frozen audio is
    // missing the change the user just heard.
    if (op == RegionOp::Freeze) fWin->FlushFxEditors();

    // The snapshot the worker reads. Freeze needs the whole project (its
    // clips, instrument and chain); the region ops need the clip and the rate.
    fRegionSnapshot.reset();
    if (op == RegionOp::Freeze)
        fRegionSnapshot = std::make_unique<Project>(*fWin->fProject);
    fRegionClipCopy = c ? *c : Clip{};
    fRegionHasClip = (c != nullptr);
    fRegionProjectRate = fWin->fProject->sampleRate;
    fRegionTrack = track;
    fRegionClipId = clip;
    fRegionOp = op;
    fRegionOk = false;
    fRegionHandled = false;
    fRegionGain = 1.0f;
    fRegionSpans.clear();
    fRegionSourceRate = 0.0;
    fRegionPath.clear();
    fRegionProgress.store(-1.0f, std::memory_order_relaxed);
    fRegionCancel.store(false, std::memory_order_relaxed);

    const char* tag = op == RegionOp::Freeze   ? "frozen"
                    : op == RegionOp::Reverse  ? "reversed" : "region";
    if (op == RegionOp::Reverse || op == RegionOp::Freeze)
        fRegionPath = RenderPath(tag);
    fRegionLabel = op == RegionOp::Freeze       ? "Freezing track"
                 : op == RegionOp::Normalize    ? "Normalizing region"
                 : op == RegionOp::Reverse      ? "Reversing region"
                                                : "Stripping silence";

    // The bar goes up BEFORE the thread starts, so a very short job cannot
    // finish and be handled before there is anything to close.
    BRect wr(240, 240, 240 + 320, 240 + 96);
    ExportProgressWindow* w = new ExportProgressWindow(wr, BMessenger(fWin),
                                                       fRegionLabel.c_str());
    fRegionProgMsgr = BMessenger(w);
    w->Show();

    fRegionRunning.store(true, std::memory_order_release);
    fRegionThread = std::thread([this, op]() {
        RegionJob job;
        job.cancel = &fRegionCancel;
        job.progress = [this](float v) {
            fRegionProgress.store(v, std::memory_order_relaxed);
        };
        switch (op) {
            case RegionOp::Freeze: {
                // The exporter's own progress/cancel, wired to this job's
                // atomics; the bar shows the bounce's real fraction.
                const ExportJob fj = FreezeExportJob();
                fRegionOk = RenderTrackToWav(*fRegionSnapshot, fRegionTrack,
                                             fRegionPath, &fj);
                break;
            }
            case RegionOp::Normalize: {
                float gain = 1.0f;
                fRegionOk = MeasureClipPeak(fRegionClipCopy, fRegionProjectRate, job,
                                            &gain);
                fRegionGain = gain;
                break;
            }
            case RegionOp::Reverse:
                fRegionOk = RenderReversedClip(fRegionClipCopy, fRegionProjectRate,
                                               fRegionPath, job);
                break;
            case RegionOp::StripSilence: {
                StripSilenceParams params;
                fRegionOk = AnalyzeClipSilence(fRegionClipCopy, fRegionProjectRate,
                                               params, &fRegionSpans,
                                               &fRegionSourceRate, job);
                break;
            }
        }
        fRegionRunning.store(false, std::memory_order_release);
    });
    fWin->UpdatePulse();
}

void RenderJobs::FinishRegionJob() {
    fRegionHandled = true;
    if (fRegionProgMsgr.IsValid()) {
        BMessage q(B_QUIT_REQUESTED);
        fRegionProgMsgr.SendMessage(&q);
        fRegionProgMsgr = BMessenger();
    }
    if (fRegionThread.joinable()) fRegionThread.join();
    fRegionSnapshot.reset();

    const bool cancelled = fRegionCancel.load();
    if (cancelled || !fRegionOk) {
        // The project is untouched: nothing below this line ran.
        std::fprintf(stderr, "Region job: %s (%s)\n", cancelled ? "cancelled"
                     : "failed", fRegionLabel.c_str());
        fWin->SetTransportStatus(cancelled ? "Render cancelled"
                                           : "Render failed");
        fWin->UpdatePulse();
        return;
    }

    // Everything past here runs on the LOOPER, against the CURRENT model: the
    // track and the clip are re-found by id, so an edit made while the worker
    // ran cannot be applied to something that moved (or no longer exists).
    Track* t = fWin->fProject->FindTrack(fRegionTrack);
    if (t == nullptr) {
        fWin->SetTransportStatus("Render finished: track is gone");
        fWin->UpdatePulse();
        return;
    }
    switch (fRegionOp) {
        case RegionOp::Freeze: {
            // The render is a WAV we just wrote, but it goes through the same
            // factory as any import so this path cannot drift from the others.
            std::unique_ptr<IAudioSource> src = OpenAudioSource(fRegionPath);
            if (!src) {
                fWin->SetTransportStatus("Freeze failed");
                break;
            }
            const double fileRate = src->FrameRate();
            const double projRate = fWin->fProject->sampleRate;
            Clip fc;
            fc.startFrame   = 0;
            fc.sourceOffset = 0;
            fc.sourcePath   = fRegionPath;
            fc.lengthFrames = (Frame)llround(src->TotalFrames()
                                * (fileRate > 0 ? projRate / fileRate : 1.0));
            fWin->fStack->Execute(
                std::make_unique<FreezeTrackCommand>(fRegionTrack, true, fc),
                *fWin->fProject);
            (*fWin->fPeaks)[fRegionPath].Build(*src);
            fWin->fTimeline->Invalidate();
            fWin->SetTransportStatus("Track frozen");
            break;
        }
        case RegionOp::Normalize: {
            if (t->FindClip(fRegionClipId) == nullptr) {
                fWin->SetTransportStatus("Render finished: region is gone");
                break;
            }
            fWin->fStack->Execute(
                std::make_unique<SetClipGainCommand>(fRegionTrack, fRegionClipId,
                                                     fRegionGain),
                *fWin->fProject);
            fWin->fTimeline->Invalidate();
            fWin->SetTransportStatus("Region normalized");
            break;
        }
        case RegionOp::Reverse: {
            const Clip* cur = t->FindClip(fRegionClipId);
            if (cur == nullptr) {
                fWin->SetTransportStatus("Render finished: region is gone");
                break;
            }
            // Replace the clip with one pointing at the reversed file (fades
            // swap so the fade follows the now-reversed audio); same position,
            // length, gain.
            Clip nc = *cur;
            nc.id = kInvalidClipId;
            nc.sourcePath   = fRegionPath;
            nc.sourceOffset = 0;
            nc.takeGroup    = 0;
            std::swap(nc.fadeInFrames, nc.fadeOutFrames);
            auto macro = std::make_unique<MacroCommand>("Reverse Clip");
            macro->Add(std::make_unique<RemoveClipCommand>(fRegionTrack,
                                                           fRegionClipId));
            macro->Add(std::make_unique<AddClipCommand>(fRegionTrack, nc));
            fWin->fStack->Execute(std::move(macro), *fWin->fProject);
            std::unique_ptr<IAudioSource> src = OpenAudioSource(fRegionPath);
            if (src) (*fWin->fPeaks)[fRegionPath].Build(*src);
            fWin->fTimeline->Invalidate();
            fWin->SetTransportStatus("Region reversed");
            break;
        }
        case RegionOp::StripSilence: {
            const Clip* cur = t->FindClip(fRegionClipId);
            if (cur == nullptr) {
                fWin->SetTransportStatus("Render finished: region is gone");
                break;
            }
            if (fRegionSpans.size() <= 1) {   // no gaps worth cutting
                fWin->SetTransportStatus("No silence to strip");
                break;
            }
            const double toProj = (fRegionSourceRate > 0)
                                ? fWin->fProject->sampleRate / fRegionSourceRate
                                : 1.0;   // src frames -> project frames
            auto macro = std::make_unique<MacroCommand>("Strip Silence");
            macro->Add(std::make_unique<RemoveClipCommand>(fRegionTrack,
                                                           fRegionClipId));
            for (const daw::Span& s : fRegionSpans) {
                Clip nc = *cur;
                nc.id           = kInvalidClipId;
                nc.sourceOffset = cur->sourceOffset + s.start;   // source frames
                nc.startFrame   = cur->startFrame
                                + (Frame)llround(s.start * toProj);
                nc.lengthFrames = (Frame)llround((s.end - s.start) * toProj);
                nc.fadeInFrames = 0;
                nc.fadeOutFrames = 0;
                nc.takeGroup    = 0;
                macro->Add(std::make_unique<AddClipCommand>(fRegionTrack, nc));
            }
            fWin->fStack->Execute(std::move(macro), *fWin->fProject);
            fWin->fTimeline->Invalidate();
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Stripped to %d regions",
                          (int)fRegionSpans.size());
            fWin->SetTransportStatus(msg);
            break;
        }
    }
    fWin->UpdatePulse();
}

std::string RenderJobs::RenderPath(const std::string& tag) const {

    // First free <dir>/<tag>-N.wav: a render must not overwrite one a clip of
    // the reopened project still references (same hazard as the takes).
    return NextFreeWavPath(fWin->fRecCtl.fTakeDir.empty() ? std::string(".") : fWin->fRecCtl.fTakeDir, tag);
}

} // namespace daw
