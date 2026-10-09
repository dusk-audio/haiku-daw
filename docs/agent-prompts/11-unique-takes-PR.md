# feat: unique take names (plan M0.3)

Branch `feature/unique-takes` off `master` (`51e52c5`). Two commits: the spec,
the change.

## The defect

`MainWindow::StartCapture` named takes `take-<++fTakeCounter>.wav` with the
counter a per-session member, and `WavWriter::OpenFormat` opened with
`std::ios::trunc`: recording again after reopening a project overwrote
`take-1.wav` while that project's clips still pointed at it — silent data loss.
`RenderPath` (reverse/freeze renders) had the same shape (`fRenderSeq` from 0),
so it is fixed with the same helper.

## What changed

- **`src/model/TakeNames.h`** (kit-free, header-only): `NextFreeWavPath(dir,
  prefix, first=1)` — the first `<dir>/<prefix>-N.wav` that does not exist;
  empty `dir` = the working directory; gaps are reused.
- **`src/engine/WavWriter.{h,cpp}`**: `exclusive` flag on `Open`/`OpenFormat`
  (default `false`). When set, a POSIX `O_CREAT|O_EXCL` probe creates the file
  first; if it already exists the open fails and the file is left untouched.
- **`src/engine/Recorder.cpp`**: the take writer opens exclusively.
- **`src/ui/MainWindow.{h,cpp}`**: takes and reverse/freeze renders name
  themselves by scanning (`fTakeCounter` and `fRenderSeq` are gone); the
  reverse render also opens exclusively. A refused open still aborts the take
  with the existing message.
- The exporter keeps the truncating default: its `.part` temp file must be
  able to replace a stale one from an interrupted render.

## What is verified, and how

- **Host: 51/51** including the new `tests/takenames_tests.cpp` (22 checks):
  empty directory, occupied names, gap reuse, render prefixes, the regression
  itself (session A's take survives session B's independent scan, byte for
  byte), the exclusive refusal against a sentinel, and that the default still
  truncates.
- Mutation-checked, each restored: a scanner that never skips existing names
  fails the regression checks (and demonstrates the overwrite); dropping the
  `O_EXCL` probe fails the sentinel checks.
- `sh scripts/haiku_syntax_check.sh`: `src/ui/MainWindow.cpp`,
  `src/engine/Recorder.cpp` OK, 0 FAIL.
- **VM**: `build` 53/53, `build-off` 49/49 including `ui_functional_tests`.

## What is NOT verified

| Claim | State |
| --- | --- |
| The take path end to end (record → file lands on the next free name) | **Cannot be driven from a test**: the VM has no usable capture device for a take, and the harness has no recording flow. The chooser and the writer are host-tested; the wiring (`StartCapture` → `Recorder::Start`) is compile-checked and unchanged in shape. This is a **click line for Marc's next hardware pass / R5**: record a take, reopen the project, record again → `take-2.wav` appears and `take-1.wav` still plays in the old clip. |
| Freeze/reverse renders across sessions | Same scanner + exclusive writer; the render itself is exercised by existing export/render tests, the naming by the new host test. |
