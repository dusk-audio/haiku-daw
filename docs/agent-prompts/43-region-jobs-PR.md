# Record — M4.4 (second half) the region jobs move off the window thread

Branch `feature/m3-transport-mixer-jobs` (shared with 39 and 40), based on
`master` (`85b19b1`). Spec: `docs/agent-prompts/43-region-jobs.md`. Plan:
`docs/PLAN_1.0.md` §M4.4 ("Freeze, Reverse, Normalize and Strip Silence run as
`RenderJobs` with progress").

## The defect this closes

`RenderJobs`' four region operations ran on the window thread. Each opened the
clip's file, decoded the WHOLE clip (`DecodeClipRegion`), did its DSP and then
ran its command — so for the length of a decode the looper answered nothing:
no meter push, no repaint, no keystroke, no Cancel. The export has run on a
worker with a progress bar since R1; this is the same treatment for the ops the
clip menu runs.

## What landed

**The work is kit-free and host-tested** (`src/model/RegionJobs.h/.cpp`, new):
`DecodeClipRegion`, `MeasureClipPeak`, `RenderReversedClip`,
`AnalyzeClipSilence` and `RenderTrackToWav` (the freeze render, moved out of
`MainWindow.cpp`), each taking a `RegionJob` — the exporter's own
progress+cancel shape, deliberately, so this app has one kind of long job. Each
polls the cancel flag between decode chunks and reports progress as it goes.
`RenderReversedClip` writes `<path>.part` and renames on success, so a stopped
or failed job leaves nothing where a finished file is expected (the export's
rule).

**The UI keeps only the wiring** (`src/ui/RenderJobs.{h,cpp}`):
`StartRegionJob` snapshots what the worker needs (the clip by value, the
project's rate, and — for a freeze — a copy of the project), raises the
progress bar and starts the thread; `FinishRegionJob` runs on the looper from
the pulse, reaps the thread, closes the bar and applies the result as a command.
**The model is touched in exactly one place, and only when the job succeeded**:
a cancelled or failed job applies nothing and rebuilds no peaks. Both the track
and the clip are re-found by id at that point, so an edit made while the worker
ran cannot be applied to something that moved or is gone (the status line says
so instead).

`ExportProgressWindow` is the general job bar now: its title and label come
from the caller, so it reads "Freezing track" / "Reversing region" / "Stripping
silence" while the protocol (progress in, cancel back) is unchanged. One job at
a time across both kinds — a second request is refused with a status line.
Unfreeze stays synchronous (a pure model restore, no file work).

## Verification (this host)

| Command | Result |
|---|---|
| `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release` | exit **0** |
| `cmake --build build-host -j2` | exit **0** |
| `ctest --test-dir build-host` | **69/69 passed**, exit 0 |
| `sh scripts/haiku_syntax_check.sh` | **75 OK, 4 FAIL** (the 4 pre-existing FLAC/vorbis files, untouched here — see `39-transport-bar-PR.md`) |

`tests/exporter_job_tests.cpp` gained the region jobs' own surface, following
the export's: progress monotonic, in range and reaching exactly 1.0; the
reversed file's first frame equals the source's last; a cancel up front, a
cancel at the first progress report (which must also STOP the reports — a
cancel that only discards the result still burns the window's time), and a
cancel after the samples but before the rename (nothing left, not even the
temp); a silent source refusing to normalize; the strip-silence spans with the
pad arithmetic; and the freeze render's progress, content, cancel and
missing-track cases. **104 checks, 0 failures.**

`tests/ui_functional_tests.cpp` gained `TestRegionJob` (VM): reverse a real
20 s clip through the same message the clip menu posts, with the bar up, the
window still answering, a cancel that leaves the clip path unchanged, and a
completed run that swaps the clip onto the rendered file.

## Mutation checks (each one watched fail, then restored)

| # | Mutation | Caught by |
|---|---|---|
| M13 | `DecodeClipRegion` ignores the cancel flag | `exporter_job_tests` (the reports must stop) |
| M14 | no cancel check after the samples (the temp gets renamed anyway) | `exporter_job_tests` (nothing at the destination) |
| M15 | `MeasureClipPeak` normalizes silence | `exporter_job_tests` (the silent source) |
| M16 | the freeze render is handed no job (no progress, no cancel) | `exporter_job_tests` (progress + cancel) |

M13–M15 initially **survived** the first version of the tests: the layered
cancel checks in the worker hid a mutation that only removed one of them, and
the fixture never exercised the write-phase cancel or a silent source. The
tests were then strengthened (the report-count bound after a cancel, the
late cancel, the silent clip) until each mutation is caught — and the
`seen`-vector reads were guarded so a job that reports nothing FAILS its check
instead of walking off an empty vector into a segfault (M16 did exactly that
before the guard; a suite that crashes says far less than one that reports the
failing check).

## Not verified here

- **The VM run and the screen**: the bar for each op, the window staying
  responsive during a decode, and the click-through of the frozen/undone states
  are the orchestrator's (`TestRegionJob` plus the screenshot pass).
- The freeze render's *audible* result (that a frozen track sounds like the
  track did): host-tested only for "it carries signal", not for equality with
  the live engine — the live engine's own graph is not host-buildable.

## Deviations from the spec

- The job's progress for Normalize/Reverse/Strip Silence is coarse by design
  (half the bar is the decode, the rest the DSP); Freeze shows the exporter's
  real fraction. The spec did not promise a finer granularity, but the numbers
  are not the export's continuous ones either.
- `AnalyzeClipSilence` gained an out-parameter for the source rate it decoded
  at (the spans are in SOURCE frames, so the looper needs it to place the
  resulting clips); the spec's signature did not have it, and the alternative —
  re-opening the file on the looper — would have moved disk work back onto the
  window thread.
- Strip Silence's "no gaps worth cutting" case reports it in the status line and
  applies nothing; the old synchronous code simply returned.
