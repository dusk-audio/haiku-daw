# refactor: split up MainWindow (plan M1.1)

Branch `feature/split-mainwindow` off `master` (`3685874`, M0 complete). Eight
commits: four slices, the wiring fix, the harness hardening and the lessons.
Pure moving of code — no behaviour changes — verified by the suites, above all
`ui_functional_tests` on the VM.

## The ledger

| | before | after |
| --- | --- | --- |
| `src/ui/MainWindow.cpp` | 3636 | 2802 |
| `src/ui/MainWindow.h` | 325 | 272 |

New homes: `ProjectDocument.{h,cpp}` (the file, recovery copy, recent list),
`TransportController.{h,cpp}` (the engine and the transport state + play, stop,
rebuild, record-engine), `RecordController.{h,cpp}` (the record state + the
nine record methods), `RenderJobs.{h,cpp}` (export, freeze, region rendering).

## What stayed in the window, and why

The prompts, the file panels, the menus, the timeline/meter refreshes, the
message handlers, the pulse's UI half, the export dialog and its remembered
choices. A moved body reaches them through the window it was given
(`SetWindow`, friendship) — deliberately mechanical, so the suites that
already covered play/stop/record/export are the verification, and later
slices can tighten the seams one accessor at a time.

The destruction-order invariant moved with the record state
(`RecordController` is declared before `TransportController`, so the engine
dies first and the RT thread is stopped before the recorder it reads).

## What the VM found (worth keeping)

- **A new `.cpp` must join `daw_ui`'s source list.** The cross-compile syntax
  check builds a single file, so it passed while the link had no definition
  (`undefined reference to StopPlayback`) — the VM build is what caught it.
- **A crashed app under `debug_server` reads as a hang.** One slice left
  `fRecCtl.SetWindow(this)` unwritten (a scripted replacement whose anchor was
  in the wrong file, silently a no-op). The first record method the export
  path reaches dereferenced the null window, the app segfaulted, and
  `debug_server`'s dialog froze the process: the suite sat until ctest's
  timeout, and — this is the part that cost the cycles — **the last stderr
  line is never where it stopped, because the frozen process holds stderr's
  lock**. Told apart now: `Crashed program` windows in `ps -a`.
- **Every harness lock is bounded.** Six predicates, five blocks and one bare
  `Lock()` in `ui_functional_tests` waited forever; they are
  `LockWithTimeout(1 s)` now, so a wedged window fails a check instead of
  stopping the run — which is what named the wedge above.

## Verification

- VM `build` 54/54 and `build-off` 50/50 on every slice (the ui suite at the
  same check count throughout — the point of a move), plus host 52/52 and
  `haiku_syntax_check.sh` 0 FAIL on each.
- Each transplant audited after the transform: every `fWin->` member and
  method reference checked against `MainWindow.h`, and none of a controller's
  own state left prefixed.

## What is NOT verified

| Claim | State |
| --- | --- |
| The moved methods' behaviour on real hardware | The suites drive what they can (space-key play/stop, the export flows, the record-engine build); a real take still needs the hardware pass, as before the move. |
| The seams are the *right* ones | This is a first cut: the controllers take the window whole. Tightening them (passing the widgets a method needs instead of the window) is deliberate future work, not done here. |
