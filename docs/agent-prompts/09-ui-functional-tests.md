# Functional tests for the UI (automating the click lists)

Marc's instruction, 2026-10-09: *"make sure to automate all functionality tests
and have them report back to you"* and *"I should only perform manual testing
when absolutely necessary."* This is the harness that does it, and the rule for
what still earns a click list.

## The assumption that blocked this, and why it was wrong

Every handoff so far said the GUI could not be tested from here:

> **`app_server` is not reachable from SSH.** No test can create a `BWindow`,
> and nothing is audible. You cannot click-test.
> — `docs/agent-prompts/08-release-1.0-ENTRY.md` §4

That came from `screenshot` exiting non-zero over SSH. It is not what a window
test says. On 2026-10-09 a probe — a `BApplication`, a `BWindow`, `Show()`,
`WINDOW_OK` — ran over SSH and passed:

```
$ cat /tmp/wintest.cpp   # BApplication + BWindow + Show + snooze + print
$ g++ -std=c++17 wintest.cpp -o wintest -lbe && ./wintest
WINDOW_OK
```

A GUI process launched from an SSH session **does** reach this VM's
app_server. (The `daw` binary itself stays alive for as long as you let it, too;
nobody had tried that.) So the windows can be constructed and driven, and a
click list is only needed where a human eye or ear is genuinely the instrument.

## The shape

- **`daw_ui`** — the UI sources are a static library now (`CMakeLists.txt`), so
  `daw` is a `main()` around it and a test binary can link the same windows.
- **`tests/ui_functional_tests.cpp`** — a real `BApplication`, a real
  `MainWindow`, and messages posted *exactly* what the widgets post:
  `MSG_NEW_MIDI` for a menu item, `kMsgExportOptions` for the dialog,
  `MSG_EXPORT_REF` (with a real `entry_ref`) for the file panel. The model is
  read under the window's lock, the way any looper-external reader has to.
- **The test body runs on its own thread** while `BApplication::Run()` drives
  the message loop; a window only receives messages while its looper runs. The
  summary prints before the window is closed, because closing the main window
  quits the app.
- **It skips (exit 77) without an app_server** — `SKIP_RETURN_CODE` is set, so
  "no display" can never be counted as a pass.
- **One window per run.** `MainWindow::QuitRequested` quits the application, so
  a per-test window would end the run after the first test.

## What it automates today

| Click-list behaviour | Test |
| --- | --- |
| A posted message reaches the window's looper and changes the model | `test_message_round_trip` |
| The export dialog's answer opens the file panel, and nothing renders yet | `test_export_flow` |
| The panel's answer starts the worker; the file appears; no `.part` survives | `test_export_flow` |
| **The window keeps answering messages while a render runs** (the "stays responsive" step) | `test_export_flow` |

9 checks, 0 failures, wired into `ctest` (the VM suite reports 51/51 with it).

## What still needs a person, and why

- **Sound.** The VM has no usable audio; anything audible is R5's, on the real
  hardware. That is what the release checklist is for.
- **How it looks.** A test can say a window appeared and a value changed; it
  cannot say the layout is right. Look at screenshots, not at tests.
- **Third-party plugin GUIs** (package 07's editors): the windows are the
  plugin's, and driving them means simulating input inside someone else's view
  tree. In scope later; not today.

## Adding a test

1. Post what the widget posts (`MainWindow.h` exports the ids that are app
   wiring; the rest stay private to `MainWindow.cpp`).
2. `WaitFor(...)` rather than `snooze` where the work is asynchronous; take the
   window lock inside the predicate.
3. Assert on the model, on a file, or on window counts — never on a pixel.
4. If it needs a widget-only path (a drag), stop: extract the decision into
   kit-free code and test that instead, per the project's kit-free rule.
5. A window the test brings up gets a `Shot("name")` call while it is on
   screen, and a model change made under the lock is followed by
   `kMsgUiRefresh`. Assertions never read pixels, but a person (or agent)
   reviewing the `DAW_UI_SHOTS` run does — see `docs/UI_GUIDELINES.md`.
