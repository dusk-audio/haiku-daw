# Task: M4.4 (second half) — region jobs off the window thread

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/m3-transport-mixer-jobs` off `master`. This is the second half of item
M4.4 of `docs/PLAN_1.0.md`: "Freeze, Reverse, Normalize and Strip Silence run
as `RenderJobs` with progress". The first half (background peaks with an
on-disk cache) is already merged (`src/ui/PeakLoader.*`).

## Codebase orientation (read before coding)

- `src/ui/RenderJobs.cpp` — the region work today runs ON THE WINDOW THREAD:
  `FreezeTrack` (`:124`) renders the track through `RenderTrackToWav` and then
  runs its command; `RegionNormalize` (`:167`), `RegionReverse` (`:183`) and
  `RegionStripSilence` (`:228`) each call `DecodeClipRegion` (`:262`, which
  reads the whole clip through `OpenAudioSource`), do their DSP, and then run
  their command. A large clip freezes the window for the whole decode — every
  60 Hz meter push, every keystroke and every repaint waits for it.
- The export already does it right (`StartExport` `:25`, `FinishExport` `:87`):
  snapshot the model, run the worker, show `ExportProgressWindow`, have the
  window's own 60 Hz pulse (`MainWindow::UpdatePulse` `:2480`, the
  `if (!fRender.fExportHandled)` block in the `MSG_PULSE` handler `:1976`) push
  the progress and finish the job on the looper. Cancelling raises a flag the
  worker polls; the export's rule is "a cancelled or failed export leaves
  nothing behind" (temp + rename, `Exporter.cpp`).
- `src/ui/ExportProgressWindow.{h,cpp}` — the progress bar; its title and label
  are hard-coded for an export ("Exporting", "Rendering mix").
- Kit-free code is host-tested (`ctest --test-dir build-host`); `src/ui/` and
  `src/engine/Engine.cpp` do not compile on the Linux host — check them with
  `sh scripts/haiku_syntax_check.sh` (0 FAIL) and keep them pattern-faithful.
- `tests/exporter_job_tests.cpp` is the job-surface test to follow: progress
  monotonic and reaching exactly 1.0, cancel stopping the run, and **nothing
  left at the destination** after a cancel or a failure.
- `tests/ui_functional_tests.cpp` (`TestExportFlow`, `TestExportCancel` near
  `:630`) is the closest VM coverage; the same shapes apply to a region job.

## Goal

Freeze, Reverse, Normalize and Strip Silence decode and render on a worker
thread, with the progress window (and its Cancel) up while they run, and the
model is touched only on the looper, only when the job finished successfully.

## Design

### 1. The work becomes kit-free and host-testable

New `src/model/RegionJobs.{h,cpp}` (STL + the kit-free readers/writers already
in `daw_model`), holding exactly what the window's bodies do today:

```cpp
struct RegionJob {                      // the ExportJob shape
    std::function<void(float)> progress;   // worker thread, 0..1, optional
    const std::atomic<bool>*   cancel = nullptr;
};
int64_t DecodeClipRegion(const Clip& c, double projectRate,
                         std::vector<float>& out, double& outRate,
                         const RegionJob* job = nullptr);
bool MeasureClipPeak(const Clip& c, double projectRate, const RegionJob&,
                     float* outGain);              // Normalize
bool RenderReversedClip(const Clip& c, double projectRate,
                        const std::string& outPath, const RegionJob&);
bool AnalyzeClipSilence(const Clip& c, double projectRate,
                        const StripSilenceParams&, std::vector<Span>* spans,
                        const RegionJob&);
bool RenderTrackToWav(const Project&, TrackId, const std::string& out,
                      const ExportJob* job);       // Freeze (moved from MainWindow.cpp)
```

Each polls `cancel` between decode chunks / blocks and reports progress;
`RenderReversedClip` writes to `<path>.part` and renames on success, so a cancel
or a failure leaves nothing where a finished file is expected — the export's
rule, applied to a region job.

### 2. The UI side keeps only the wiring

`src/ui/RenderJobs.{h,cpp}`:

- `StartRegionJob(op, track, clip)` — one job at a time (a second request while
  one runs is refused with a status line). It snapshots what the worker needs
  (the clip by value, the project's rate, the destination path, the freeze
  snapshot), raises the progress window with the op's own title and label
  ("Freezing — Bass", "Reversing — take-2.wav", ...), and starts the thread.
- `FinishRegionJob()` — runs on the looper from the pulse: reaps the thread,
  closes the bar, and **only then** applies the result as a command on the
  current model (re-finding the track and clip by id: the model may have been
  edited while the job ran; if either is gone, nothing is applied and the status
  line says so). A cancelled or failed job applies nothing and rebuilds no
  peaks.
- Unfreeze stays synchronous: it is a pure model restore with no file work.

`src/ui/ExportProgressWindow.{h,cpp}` becomes the general job bar: the
constructor takes the window title as well as the label. The message ids
(`kMsgExportProgress`, `kMsgExportCancel`) are unchanged — the bar is one
window with one protocol, whatever job is behind it.

`MainWindow::UpdatePulse`'s `need` test counts a running region job too, so the
pulse keeps ticking while the window is otherwise idle.

## Work items

1. `src/model/RegionJobs.{h,cpp}` (new) — the functions above, plus
   `StripSilenceParams` (the thresholds the window hard-codes today:
   -50 dBFS, 250 ms, 20 ms) so the caller and the tests agree.
2. `CMakeLists.txt` — add the new source to `daw_model`.
3. `src/ui/RenderJobs.{h,cpp}` — the worker + progress + cancel + finish, and
   the removal of the synchronous bodies.
4. `src/ui/MainWindow.{h,cpp}` — the region-op messages route to
   `StartRegionJob`; `RenderTrackToWav` moves out; the pulse finishes the job;
   the frozen-track command still lands on the looper.
5. `src/ui/ExportProgressWindow.{h,cpp}` — the title parameter.
6. Tests: `tests/exporter_job_tests.cpp` gains the region-job cases (progress
   monotonic + 1.0, cancel leaves no file, a finished reverse's samples are the
   input reversed, silence analysis spans, a normalize gain, and the freeze
   render); `tests/ui_functional_tests.cpp` gains a region-job flow (post the op,
   see the bar, cancel it, prove the project is untouched; run it to completion
   and prove the clip was replaced).

## Out of scope (say so in the record)

- A job queue: one region job at a time, like the export.
- The peak cache (M4.4's first half) is already on the worker; a region job
  still rebuilds the *new* file's peaks on the looper after it applies, which
  is what the synchronous path did too.
- Freeze's command semantics do not change (the same `FreezeTrackCommand`).

## Definition of done

- `cmake --build build-host` exits 0, `ctest --test-dir build-host` green (count
  in the record), `sh scripts/haiku_syntax_check.sh` 0 FAIL.
- Every new behaviour has a test, mutation-checked (mutations named in the
  record).
- The record states what is NOT verified: the VM run and the screenshots
  (including the progress bar's label for each op) belong to the orchestrator.
