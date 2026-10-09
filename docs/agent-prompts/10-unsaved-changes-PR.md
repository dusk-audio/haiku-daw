# feat: unsaved-changes tracking (plan M0.1)

Branch `feature/unsaved-changes` off `master` (`51e52c5`). Three commits: the
spec (`10-unsaved-changes.md`), the kit-free serial tracking, the window half.

## The design, and why not a saved-position index

An index (compare the current undo position with the saved one) is wrong twice:

- a drag that continues after a save **coalesces into the saved entry**, so no
  position moves while the project changes;
- the stack is capped at 256 entries, so after a trim, **undoing everything
  left** lands `extra` edits into the project while position zero claims to be
  the original.

Each entry instead carries a serial that is never reused (`fNextSerial`,
monotonic). A coalesced post renews the top entry's serial; a trim sets the
empty stack's **base serial** to the serial of the last dropped entry;
`MarkSaved()` records `CurrentSerial()`; dirty is `CurrentSerial() != saved`.
Redo keeps an entry's serial, because the state it lands on is identical.

## What changed

- `src/model/Command.h` — the serial scheme above (`Serial`, `Entry`,
  `CurrentSerial`, `MarkSaved`, `IsDirty`). `Clear()` keeps dropping history
  only; callers `MarkSaved()` once the replacement project is in place.
- `src/ui/MainWindow.{h,cpp}` — `fProjectPath`, `UpdateTitle()` (`*name —
  Haiku DAW`), a 2 Hz `MSG_TITLE` runner (the 60 Hz pulse only runs while the
  transport does; the timeline and inspector edit the model with no message to
  the window), `ConfirmDiscardChanges()` (synchronous `BAlert`, like the
  recovery prompt: Cancel / Discard / Save, Escape = Cancel), `RemoveRecoveryFile()`,
  `SaveTo()` returns whether the file was written.
- Wired into `QuitRequested` (Quit and the window's close button are the same
  `B_QUIT_REQUESTED` path) and `LoadFrom` (Open). The prompt runs **before**
  playback is stopped and before editors are flushed/closed, so a Cancel
  leaves the session exactly as it was.
- Recovery-file rules: removed only on a clean save, an explicit Discard, or a
  quit of an already-clean project. Cancel keeps it. `MSG_RECOVER`'s Discard
  uses the same helper.
- `src/main.cpp` — a fresh launch `MarkSaved()`s the seeded project: the demo
  is where the session starts, not unsaved work to be prompted about.

## What is verified, and how

- **Host: 51/51** (`ctest --test-dir build-host`), including the new
  `tests/commandstack_tests.cpp` (31 checks). Mutation-checked, each restored
  after: removing the coalesce renewal fails the drag test; removing the base
  update fails the trimmed-stack tests; making `CurrentSerial()` a position
  fails **exactly Marc's two cases** (the coalesced drag and the trimmed stack
  undone to empty).
- **ASan/UBSan: 51/51** (`ctest --test-dir b-asan`).
- **`scripts/haiku_syntax_check.sh`**: `src/main.cpp`, `src/ui/MainWindow.cpp`,
  `tests/ui_functional_tests.cpp` all OK, 0 FAIL.
- **VM**: `build` 53/53 and `build-off` 49/49, both including
  `ui_functional_tests` — 85 checks with LV2 (77 in `build-off`), 0 failures.
  The new `test_unsaved_changes` drives the window: the marker appears after
  an edit; Quit prompts; Cancel keeps the window up and dirty; Save with no
  path opens the save panel and does not quit; Discard on Open loads the
  fixture and lands clean with the file's name in the title.

## Harness lessons (cost a long debugging session — keep them)

- **`BWindow::Name()` is not the title.** Haiku's `BWindow::_SetName` renames
  the window's THREAD to `"w>" + title` and makes that the handler name, so
  `Name()` reads `w>*Untitled — Haiku DAW`. The first cut of the flow compared
  `Name()` to titles, never found its own prompt, and hung with the alert up.
  `WindowTitle()` strips the prefix; every comparison goes through it.
- **A modal `BAlert` blocks the window's looper.** Any unbounded
  `win->Lock()` in a test predicate then deadlocks the whole run instead of
  failing it. The flow uses `LockWithTimeout` everywhere and finds prompts by
  title, never by window count (an unrelated file panel can satisfy a count).
- `ui_functional_tests` now has a ctest `TIMEOUT 300` and line-buffered
  stdout, so a wedge fails the suite and says which stage it reached.

## What is NOT verified

| Claim | State |
| --- | --- |
| Save with an existing path from the prompt | Implemented (`SaveTo(fProjectPath)`); the ui flow exercised the no-path branch (panel + refusal). A click-test line: save a project, edit, Quit, Save → no panel, clean exit. |
| The prompt on New | New does not exist yet — it is plan M0.2, and it must call `ConfirmDiscardChanges()`. |
| A load that fails after Discard | The recovery copy is already gone (the user authorised that), the old project stays in memory (ProjectIO swaps only on a clean parse). Accepted; the error dialog is M0.4. |
| Flake rate of `ui_functional_tests` | One `build-off` ctest run failed the test (the first pass of the pre-hardening binary; its log was overwritten by the confirming re-run, so the failing check is unknown). After the retry hardening: **42 consecutive runs green** — 1 ctest pass, 3 direct, an 8-run loop, then a 30-run hunt (20 `build-off`, 10 `build`). Watch it on the next branch; a repeat now leaves a log. |
