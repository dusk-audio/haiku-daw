# Task: Unique take names, and a writer that never clobbers (plan M0.3)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/unique-takes` off master. Plan item M0.3 — data safety, the second half
of the confirmed release blocker: **recording again after reopening a project
overwrites takes the project still uses.**

## The defect, precisely

- `MainWindow::StartCapture` (`src/ui/MainWindow.cpp:1717`) names the take
  `take-<++fTakeCounter>.wav`. The counter is a per-session member starting at
  0, so a reopened project records `take-1.wav` again.
- `WavWriter::OpenFormat` (`src/engine/WavWriter.cpp:40`) opens with
  `std::ios::trunc`, so that name collision silently destroys the earlier file
  — which the project's clips still reference.
- `MainWindow::RenderPath` (`:2571`) has the same shape for reverse/freeze
  renders: `<tag>-<fRenderSeq>.wav`, counter from 0 each session, files the
  project references.

## Work items

1. **Kit-free name chooser** (`src/model/TakeNames.h`, header-only): the first
   free `<dir>/<prefix>-<n>.wav`, n counting from 1, where free = the path does
   not exist. Empty `dir` = the working directory. Document that the app picks
   names one thread at a time and that `WavWriter`'s exclusive open is the
   backstop.
2. **`WavWriter` refuses to clobber**: an `exclusive` flag on `Open` and
   `OpenFormat` (default `false`, so the exporter's `.part` temp keeps its
   trunc semantics). When set, create the file with POSIX `O_CREAT|O_EXCL`
   first and return `false` (with a stderr line) if it already exists — the
   existing file is left byte-for-byte intact. The Recorder passes it, and so
   does the reverse/freeze writer (their paths come from the scanner).
3. **`MainWindow` uses the scanner** for takes (drop `fTakeCounter`) and for
   `RenderPath` (drop `fRenderSeq`). A failed exclusive open still aborts the
   take with the same message as today.
4. **Host tests** (`tests/takenames_tests.cpp`, new): empty dir → `-1`; a dir
   with `-1` and `-2` → `-3`; gaps are reused (`-7` alone → `-1`); **the
   regression itself** — "session A" creates `take-1.wav`, a fresh scan (the
   reopened session) must return `take-2.wav`; `WavWriter` exclusive open on an
   existing file fails and leaves its bytes untouched; a non-exclusive open
   still truncates (the exporter's contract).
5. **CMakeLists**: `add_executable` + `add_test` for the new test.

## Definition of done

- Host build 0 + `ctest --test-dir build-host` all green (counts in the PR
  record); the new tests mutation-checked (make the scanner return n=1
  unconditionally; drop the O_EXCL probe — each must fail).
- `sh scripts/haiku_syntax_check.sh` 0 FAIL (UI and Recorder changed).
- VM `build` and `build-off` ctest green (both include `ui_functional_tests`).
- PR record `docs/agent-prompts/11-unique-takes-PR.md`. Recording cannot be
  driven from a test (no capture device on the VM); say so there.
