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
    opts.format    = ExportFormat{ c.bitDepth, c.dither };
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
        BAlert* a = new BAlert("Export", "The export failed. Nothing was "
                               "written to that name.", "OK", nullptr, nullptr,
                               B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        a->Go(nullptr);   // async, like the other warnings
    }
    fWin->UpdatePulse();
}

void RenderJobs::FreezeTrack(TrackId track, bool freeze) {

    Track* t = fWin->fProject->FindTrack(track);
    if (!t) return;

    // Freezing renders this track from the model, so an editor gesture still
    // inside its debounce has to land first -- otherwise the frozen audio is
    // missing the change the user just heard (unfreezing only checks the flag).
    if (freeze) fWin->FlushFxEditors();

    if (!freeze) {                     // unfreeze: pure model restore
        if (!t->frozen) return;
        fWin->fStack->Execute(std::make_unique<FreezeTrackCommand>(track, false),
                        *fWin->fProject);
        fWin->RebuildPeaks();
        fWin->fTimeline->Invalidate();
        return;
    }
    if (t->frozen) return;

    const std::string path = RenderPath("frozen");
    if (!RenderTrackToWav(*fWin->fProject, track, path)) {
        std::fprintf(stderr, "Freeze: render failed for track %ld\n", (long)track);
        return;
    }
    WavSource src;
    if (!src.Open(path)) return;
    const double fileRate = src.FrameRate();
    const double projRate = fWin->fProject->sampleRate;
    Clip fc;
    fc.startFrame   = 0;
    fc.sourceOffset = 0;
    fc.sourcePath   = path;
    fc.lengthFrames = (Frame)llround(src.TotalFrames()
                        * (fileRate > 0 ? projRate / fileRate : 1.0));
    fWin->fStack->Execute(std::make_unique<FreezeTrackCommand>(track, true, fc),
                    *fWin->fProject);
    (*fWin->fPeaks)[path].Build(src);
    fWin->fTimeline->Invalidate();
}

void RenderJobs::RegionNormalize(TrackId track, ClipId clip) {

    const Track* t = fWin->fProject->FindTrack(track);
    const Clip*  c = t ? t->FindClip(clip) : nullptr;
    if (!c) return;
    std::vector<float> buf; double rate = 0;
    const int64_t n = DecodeClipRegion(*c, buf, rate);
    if (n <= 0) return;
    const float peak = PeakLinear(buf.data(), n);
    if (peak <= 1e-6f) return;              // silent: nothing to normalize
    float g = 1.0f / peak;
    if (g > 64.0f) g = 64.0f;               // ceiling for near-silent clips
    fWin->fStack->Execute(std::make_unique<SetClipGainCommand>(track, clip, g), *fWin->fProject);
    fWin->fTimeline->Invalidate();
}

void RenderJobs::RegionReverse(TrackId track, ClipId clip) {

    const Track* t = fWin->fProject->FindTrack(track);
    const Clip*  c = t ? t->FindClip(clip) : nullptr;
    if (!c) return;
    std::vector<float> buf; double rate = 0;
    const int64_t n = DecodeClipRegion(*c, buf, rate);
    if (n <= 0 || rate <= 0) return;
    ReverseStereo(buf.data(), n);

    const std::string path = RenderPath("reversed");
    std::vector<int16_t> pcm((size_t)n * 2);
    for (int64_t i = 0; i < n * 2; ++i) {
        float s = buf[i];
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = (int16_t)lround(s * 32767.0f);
    }
    WavWriter w;
    // Exclusive: the path is scanned-free, and if that ever raced, refusing
    // beats overwriting a file a clip may reference.
    if (!w.Open(path, (int)lround(rate), 2, /*exclusive*/ true)
        || !w.WriteInt16(pcm.data(), pcm.size()) || !w.Close()) {
        std::fprintf(stderr, "Reverse: cannot write %s\n", path.c_str());
        return;
    }

    // Replace the clip with one pointing at the reversed file (fades swap so the
    // fade follows the now-reversed audio); same position, length, gain.
    Clip nc = *c;
    nc.id = kInvalidClipId;
    nc.sourcePath   = path;
    nc.sourceOffset = 0;
    nc.takeGroup    = 0;
    std::swap(nc.fadeInFrames, nc.fadeOutFrames);
    auto macro = std::make_unique<MacroCommand>("Reverse Clip");
    macro->Add(std::make_unique<RemoveClipCommand>(track, clip));
    macro->Add(std::make_unique<AddClipCommand>(track, nc));
    fWin->fStack->Execute(std::move(macro), *fWin->fProject);

    WavSource src;
    if (src.Open(path)) (*fWin->fPeaks)[path].Build(src);
    fWin->fTimeline->Invalidate();
}

void RenderJobs::RegionStripSilence(TrackId track, ClipId clip) {

    const Track* t = fWin->fProject->FindTrack(track);
    const Clip*  c = t ? t->FindClip(clip) : nullptr;
    if (!c) return;
    std::vector<float> buf; double rate = 0;
    const int64_t n = DecodeClipRegion(*c, buf, rate);
    if (n <= 0 || rate <= 0) return;

    const float   thresh = 0.00316f;               // ~ -50 dBFS
    const int64_t minSil = (int64_t)(0.25 * rate); // 250 ms of silence = a gap
    const int64_t pad    = (int64_t)(0.02 * rate); // keep 20 ms of air each side
    auto spans = NonSilentSpans(buf.data(), n, thresh, minSil, pad);
    if (spans.size() <= 1) return;                 // no gaps worth cutting

    const double projRate = fWin->fProject->sampleRate;
    const double toProj = (rate > 0) ? projRate / rate : 1.0;   // src -> project
    auto macro = std::make_unique<MacroCommand>("Strip Silence");
    macro->Add(std::make_unique<RemoveClipCommand>(track, clip));
    for (const daw::Span& s : spans) {
        Clip nc = *c;
        nc.id           = kInvalidClipId;
        nc.sourceOffset = c->sourceOffset + s.start;              // source frames
        nc.startFrame   = c->startFrame + (Frame)llround(s.start * toProj);
        nc.lengthFrames = (Frame)llround((s.end - s.start) * toProj);
        nc.fadeInFrames = 0;
        nc.fadeOutFrames = 0;
        nc.takeGroup    = 0;
        macro->Add(std::make_unique<AddClipCommand>(track, nc));
    }
    fWin->fStack->Execute(std::move(macro), *fWin->fProject);
    fWin->fTimeline->Invalidate();
}

int64_t RenderJobs::DecodeClipRegion(const Clip& c, std::vector<float>& out,
                                    double& outRate) {

    out.clear();
    outRate = 0.0;
    WavSource src;
    if (!src.Open(c.sourcePath))
        return 0;
    outRate = src.FrameRate();
    if (c.sourceOffset > 0)
        src.Seek(c.sourceOffset);
    // The clip plays lengthFrames project-frames == that many source-frames
    // scaled by the rate ratio, starting at sourceOffset.
    const double projRate = fWin->fProject->sampleRate;
    int64_t wantSrc = c.lengthFrames;
    if (outRate > 0 && projRate > 0)
        wantSrc = (int64_t)llround((double)c.lengthFrames * outRate / projRate);
    const float* chunk = nullptr;
    size_t frames = 0;
    while ((int64_t)(out.size() / 2) < wantSrc && src.ReadChunk(&chunk, &frames)) {
        int64_t have = (int64_t)(out.size() / 2);
        int64_t take = wantSrc - have;
        if ((int64_t)frames > take) frames = (size_t)take;
        out.insert(out.end(), chunk, chunk + frames * 2);
    }
    return (int64_t)(out.size() / 2);
}

std::string RenderJobs::RenderPath(const std::string& tag) const {

    // First free <dir>/<tag>-N.wav: a render must not overwrite one a clip of
    // the reopened project still references (same hazard as the takes).
    return NextFreeWavPath(fWin->fRecCtl.fTakeDir.empty() ? std::string(".") : fWin->fRecCtl.fTakeDir, tag);
}

} // namespace daw
