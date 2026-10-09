# Task: Split up MainWindow (plan M1.1)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/split-mainwindow` off master (M0 complete). `MainWindow` is 3.2k lines
with an 84-case `MessageReceived` handler, and every later branch (M1.2 theme,
M1.3 widgets, M1.4 docking, M2, M3) edits that one file. This is a pure moving
of code — no behaviour changes — verified by the existing suites, above all
`ui_functional_tests` on the VM.

## The four extractions (in order, one slice per commit)

1. **`ProjectDocument`** (`src/ui/ProjectDocument.{h,cpp}`) — what the session
   IS on disk: the path, the window title string, the recovery copy's path and
   removal, the recent-project list, and the bookkeeping that every flow
   shares (`NoteSaved`, `NoteLoaded`, `NoteRecovered`, `NoteNew`). The window
   keeps the prompts, the panels, the menus and the timeline. **DONE.**
2. **`TransportController`** — engine lifetime (build/rebuild/swap), play,
   stop, seek, loop, the buffer size, the metronome, `UpdatePulse`'s transport
   half. The window keeps the widgets and the messages that drive it.
   **State DONE** (`src/ui/TransportController.h`: the engine pointer, playing,
   monitoring, buffer frames, metronome and the monitor flags; the window's
   member is `fTransportCtl` — `fTransport` is the bar widget). The METHOD
   moves (play, stop, rebuild, seek, loop, the pulse's transport half) are the
   next pass on this item: they interleave with the widgets, so they move with
   the window passed in, and each one is a mechanical transplant verified by
   the suites. Do them one method per commit.
3. **`RecordController`** — arm state, take setup (targets, counter-free take
   naming, latency compensation), capture start/stop, the disk-thread failure
   report, loop-record.
4. **`RenderJobs`** — export, freeze, region ops: the snapshot, the worker
   thread, the progress window, the cancel flag. Mostly moving today's
   `fExport*` machinery behind one object.

## Rules for the move

- **No behaviour changes.** Renames and moves only; where a piece of state
  needs a setter, name it after what happened (`NoteSaved`), not after the
  field.
- Each slice: host suite green, `haiku_syntax_check.sh` 0 FAIL, VM `build` and
  `build-off` ctest green with `ui_functional_tests` at the same check count as
  before the slice (the move must not change what the flows cover; counts only
  grow when a slice adds a deliberate new check).
- Keep each extracted class free of `BWindow` knowledge where it can be (the
  document needs `BPath` only); the window remains the only message handler.

## Definition of done

- The four controllers exist and are owned by `MainWindow`; the .cpp is
  materially smaller (record the line count before/after in the PR record).
- Every suite green (counts in the record); the ui flows untouched.
- PR record `docs/agent-prompts/18-split-mainwindow-PR.md`, with the four
  slices, what stayed in the window and why, and the line-count ledger.
