# feat: the File menu (plan M0.2)

Branch `feature/file-menu` off `master` (`6f0fce7`, the three merged M0 items).
Two commits: the spec, the change.

## What changed

- **The menu** (File): New (Cmd-N), Open (Cmd-O), **Open Recent ▸**, Save
  (Cmd-S, no ellipsis any more: once the project has a path it writes straight
  to it), **Save As…** (Shift-Cmd-S, always a panel), Import ×2, Export ×3,
  separator, **Close** (Cmd-W), Quit (Cmd-Q). Close posts the window's own
  `B_QUIT_REQUESTED`, so the M0.1 prompt applies exactly as it does to the
  title-bar button and Cmd-Q.
- **`AppSettings`**: `recentProjects` (at most `kMaxRecent` = 10, one `recent`
  line each so a path with spaces survives, `lastdir`-style) and kit-free
  `RememberRecent()` (front, deduplicated, capped). `Deserialize` clears the
  list first: a parse is the whole state.
- **`MainWindow`**: `NewProject()` — ask, stop, flush and close the editors,
  an empty `Project` at the session's sample rate, stack cleared and
  `MarkSaved()`, path and take dir forgotten, armed-track list cleared (its
  ids belonged to the old project), then title/peaks/master/timeline/readout
  refreshed. Same "the engine rebuilds at the next play" stance as `LoadFrom`.
  The recent list is carried through `LoadSettings`/`SaveSettings`; the submenu
  is rebuilt whenever the list changes (save, load, prune, settings load) with
  each item carrying its path; a vanished file is pruned with a stderr note
  instead of being handed to `LoadFrom`.
- **`MSG_SAVE`, `MSG_SAVE_AS`, `MSG_NEW_PROJECT`** moved to the public id block
  (the functional tests post them exactly as the menu does); `MSG_CLOSE` and
  `MSG_OPEN_RECENT` stay private (menu-only).

## What is verified, and how

- **Host: 52/52** (the settings test grew to 42 checks: round trip with spaces,
  order, dedupe, cap at 10, `RememberRecent("")` a no-op, re-parse replaces,
  an over-long file is truncated on read). Mutation-checked, each restored:
  dropping the dedupe fails the dedupe checks, dropping the cap fails the cap
  checks.
- `sh scripts/haiku_syntax_check.sh`: `src/ui/MainWindow.cpp`,
  `tests/ui_functional_tests.cpp` OK, 0 FAIL.
- **VM `ui_functional_tests`** (`test_file_menu`): Save As opens the panel, its
  answer writes `/tmp/haiku_daw_ui_save.dawproj` and the window lands clean
  with `haiku_daw_ui_save` in the title; Save afterwards writes the same path
  with **no** panel appearing and clears the marker; New over a dirty project
  raises the prompt, and Discard leaves an empty project, a clean stack and
  `Untitled` in the title — with no path, so the M0.1 flow that follows still
  exercises the save-panel branch.
- VM: `build` **54/54** and `build-off` **50/50**, both including `ui_functional_tests`
  with the new flow.

## What is NOT verified

| Claim | State |
| --- | --- |
| The Open Recent **menu itself** (clicking an item) | A popup cannot be opened from a test. The handler is driven by the same message the item posts (`MSG_OPEN_RECENT` with a path); the pruning path (`ForgetRecent`) is code-read, not clicked. One click line for the man with the mouse: save two projects, reopen the app, File ▸ Open Recent shows both, the newest first; delete one file and its entry disappears on the next click. |
| Cmd-N / Cmd-S / Shift-Cmd-S / Cmd-W accelerators | The menu items carry them; a test cannot press a menu shortcut without opening the menu. The handlers behind them are the tested ones. |
| The engine after New while idle-monitoring | Same hole as `LoadFrom` has had all along (the monitor engine is rebuilt at the next transport start); M4.1 restructures engine lifetimes. |
