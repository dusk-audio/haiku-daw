# fix: CodeRabbit's third pass (five findings)

Branch `fix/review-3` off `master` (`3685874`, M0 complete). One commit; this is
the record. All five findings were verified against the code before anything
was changed.

## 1. A recovered session was treated as an opened document (data loss)

`MSG_RECOVER` called `LoadFrom(recoveryPath)`, which set `fProjectPath` to the
recovery file, `MarkSaved()`, added it to Open Recent, and pointed the take
directory at the settings dir. Two ordinary actions then destroyed the only
copy of the user's work:

- **Quit**: the stack read clean, so no prompt — and `QuitRequested` deleted
  the recovery file.
- **Cmd-S**: `SaveTo` wrote the project to the recovery path and
  `RemoveRecoveryFile()` then deleted that same path.

`LoadFrom(path, asRecovery)` now clears the path and take dir, calls the new
`CommandStack::MarkUnsaved()` (a serial no position can ever equal, so
`IsDirty()` is true from any position — including an empty stack), skips
RememberProject, and the title reads `*Untitled`. Saving asks for a path;
quitting prompts. Host-tested, mutation-checked (a no-op `MarkUnsaved` fails
the dirty checks).

## 2. A stale generic panel could revert a native editor's commit

Package 07's rule is that an open panel's copy of the chain is refreshed
whenever the chain changes; only `SyncFxToEngine()` did it. Four paths change
fx without it: `kMsgFxParamCommit` (which deliberately skips the engine sync),
`MSG_UNDO`, `MSG_REDO`, and `FlushFxEditors()`' native half. A panel knob move
after any of them wrote the pre-commit chain back into the model — and
`SyncFxToEngine` then put it back into the audio. All four now call
`PushChainToFxWindow()`.

## 3. `EffectsView::SetChain` refused a refresh during a drag, not during a pending wheel commit

`fCommit` (the 400 ms debounce timer) means an edit is in flight against this
copy; a refresh inside that window replaced the copy, and the timer then
committed the stale chain. The guard now covers both.

## 4. A forwarded `B_ARGV_RECEIVED` lost the sender's cwd

With `B_SINGLE_LAUNCH`, `daw rel/song.dawproj` reaches the running instance as
a message carrying the SENDER's `"cwd"` — which
`ArgvReceived(int32, char**)` cannot see. A relative path was resolved against
the running instance's directory (wrong file, or none). The handler moved to
`MessageReceived`, where `"cwd"` is visible; `ArgvReceived` is gone (the
message path covers launch and forwarding alike).

## 5. The new ui waits passed 15000 as microseconds

Four sites in `test_keyboard_focus`: 15 ms, not 15 s — `WaitFor` sleeps in
20 ms steps, so each wait checked twice and failed. They use the harness's
default (15 s) now. (The test passed on the VM anyway because a toggle against
a one-clip project lands inside a millisecond or two — which is exactly the
kind of luck a timeout should not depend on.)

## What is verified, and how

- Host 52/52, `commandstack_tests` at 37 checks with the `MarkUnsaved` case
  (mutation-checked).
- `sh scripts/haiku_syntax_check.sh`: `main.cpp`, `MainWindow.cpp`,
  `EffectsWindow.cpp`, `ui_functional_tests.cpp` OK, 0 FAIL.
- VM: `build` **54/54** and `build-off` **50/50**, `ui_functional_tests` in both.

## What is NOT verified

| Claim | State |
| --- | --- |
| The recovery flow end to end | Not drivable: the recovery path is a static inside `MainWindow.cpp`, and a test cannot make the app die uncleanly. Click line: make an edit, `kill` the app, relaunch, choose Recover — the title must read `*Untitled`, Quit must prompt, and Save must ask for a path (then the recovery file is gone and the saved project is where the user said). |
| The panel-staleness fixes at the GUI | Needs a plugin with a native editor (4K EQ 2) and the generic panel open on the same track: turn a knob in the editor, then a knob in the panel — the editor's value must survive. That is a click line for the hardware pass; the code paths are the four named sites, each compile-checked. |
| A forwarded relative argv | Needs two terminals (one in another directory); the fix follows the documented message fields. |
