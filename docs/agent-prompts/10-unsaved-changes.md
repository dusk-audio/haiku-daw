# Task: Unsaved-changes tracking — dirty state, the save prompt, the recovery file (plan M0.1)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/unsaved-changes` off master. This is plan item M0.1 (data safety and
trust, the first item of the 1.0 push) — see `~/.claude/plans/bright-juggling-bird.md`.

## Codebase orientation (read before coding)

- `CommandStack` (`src/model/Command.h`) is the only sanctioned way to mutate a
  `Project`, kit-free, host-tested. `Execute` pushes onto `fUndo` (cap
  `kMaxUndoDepth` = 256, oldest dropped past it), `Undo`/`Redo` move entries
  between `fUndo`/`fRedo`, `CoalesceInto` folds a continuing drag into the
  previous entry, `Clear` drops all history (used by `MainWindow::LoadFrom`).
- `MainWindow` (`src/ui/MainWindow.cpp`, Haiku-only): `QuitRequested` (:3208)
  quits and deletes the recovery file with no prompt; `SaveTo` (:2332);
  `LoadFrom` (:2386); the recovery file is `RecoveryPath` (:2891), written by
  the 30 s `MSG_AUTOSAVE` runner (:1181), offered at startup by `MSG_RECOVER`
  (:1194). The 60 Hz `MSG_PULSE` runner only exists while playing/recording
  (`UpdatePulse`, :1562), so it cannot carry a title refresh.
- `tests/ui_functional_tests.cpp` drives the real window on the VM by posting
  exactly what the widgets post — including a `BAlert`'s buttons, which are
  named `_b0_`, `_b1_`, `_b2_` and post `"which"` (see Haiku's
  `src/kits/interface/Alert.cpp`). One window per run; `win->Quit()` at the end
  does not call `QuitRequested` (Haiku `BWindow::Quit`), so a dirty project
  there is safe.
- Kit-free tests: one `int main()` + `CHECK` file per feature in `tests/`,
  `add_executable` + `add_test` in `CMakeLists.txt`, linked against `daw_model`.

## Goal

The app knows whether the project has unsaved changes: the title shows it, and
Quit / Open / New / window close ask before losing work. The recovery file is
deleted only when it is genuinely not needed. This is the release blocker:
`QuitRequested` today quits dirty and deletes the recovery copy.

## Design (decided by Marc — do not fall back to an index comparison)

Comparing the current undo position with the saved one is wrong twice over:

- **A drag that continues after a save.** The drag is merged into the existing
  undo entry, so the position does not move and the project looks saved when it
  is not.
- **Undo after old history has been dropped.** The stack keeps only 256
  entries. Undoing everything that is left does not return to the original
  project, but a position of zero would say it does.

Instead:

- Each undo entry carries a **serial number that is never reused**; a drag
  merged into an entry gives that entry a **new** serial.
- `MarkSaved()` records the serial of the top entry. The project is dirty
  whenever `CurrentSerial() != saved serial`.
- An empty stack has a **base serial**: 0 at first, then the serial of the last
  entry dropped when history is trimmed.

## Work items

1. **`CommandStack` (`src/model/Command.h`, kit-free).** Entries carry serials
   (a small `Entry` struct; `fNextSerial` monotonic from 1). `Execute` assigns
   a serial on push and **renews it when `CoalesceInto` absorbs a command**;
   trimming sets `fBaseSerial` to the last dropped entry's serial. Public:
   `CurrentSerial()`, `MarkSaved()`, `IsDirty()`. `Redo` must restore an
   entry's serial unchanged (redo returns to the same state). Document that
   callers of `Clear()` must `MarkSaved()` once the new project is in place.
2. **Host tests** (`tests/commandstack_tests.cpp`, new): dirty across
   execute/undo/redo cycles; a coalesced drag after `MarkSaved()` reads dirty;
   a stack trimmed past `kMaxUndoDepth` then undone to empty does **not** read
   clean unless the save happened at the base; save at the empty-after-trim
   state then execute/undo reads clean again; serials survive redo; a new
   command after undo does not reuse the dropped redo serials.
3. **Window title** (`MainWindow`): `*name — Haiku DAW` when dirty, `name —
   Haiku DAW` when clean, name = the project file's base name (extension
   dropped), `Untitled` before the first save. Refresh on a low-rate
   `BMessageRunner` (the 60 Hz pulse is not always running) plus directly
   after save/load/undo/redo. `fProjectPath` (new member) is set by `SaveTo`
   and `LoadFrom`.
4. **Prompt** (`MainWindow::ConfirmDiscardChanges`): when dirty, a three-button
   `BAlert` "Cancel" / "Discard" / "Save" (Escape = Cancel) — the same
   synchronous `Go()` the recovery prompt uses. Cancel → do not proceed.
   Discard → remove the recovery file (explicit authorisation) and proceed.
   Save → `SaveTo(fProjectPath)`; with no path yet, open the save panel and do
   not proceed (the caller is retried); a failed save does not proceed either.
5. **Wire the prompt in:** `QuitRequested` (Quit and window close are the same
   path — Haiku delivers both as `B_QUIT_REQUESTED`), and `LoadFrom` (Open).
   `New` arrives in M0.2 and must use the same helper.
6. **Recovery file rules:** delete it only after a clean save, an explicit
   Discard, or a quit of an already-clean project — never on Cancel. Centralise
   as `RemoveRecoveryFile()`. Marking saved at startup (`main.cpp`, after
   seeding) keeps a fresh launch clean.
7. **`ui_functional_tests` flows:** dirty then quit shows the prompt; Cancel
   leaves the window up and still dirty; Save with no path opens the save panel
   and keeps the window; Discard on Open loads the chosen project, clears the
   dirty mark and updates the title.

## Definition of done

- Host build 0 + `ctest --test-dir build-host` all green (exact counts in the
  PR record); the new tests mutation-checked (break the serial renewal, the
  base-serial update, the dirty comparison — each must fail the suite).
- `sh scripts/haiku_syntax_check.sh` 0 FAIL (UI changed).
- VM `build` and `build-off` ctest green, including `ui_functional_tests`.
- PR record `docs/agent-prompts/10-unsaved-changes-PR.md`.
