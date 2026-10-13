# Record — PR 1's integration fixes (the preferences wedge, the UI-scale truth, the screen pass)

Branch `feature/m3-transport-mixer-jobs`, fast-forwarded onto the integration branch
`feature/m3-shell` (`7307402` = my M3.1/M3.4/M4.4 work + the M3.5–M3.7 preferences,
project-settings and templates packages). The failing item was `TestPreferences`: one
FAIL, then a wedge that killed the whole UI suite in `build` and `build-off`.

## Defect 1 — the wedge: a settings reload ran view calls off the window thread

`TestPreferences` calls `win->ReloadSettings()` from the test thread. The hook was
`void ReloadSettings() { LoadSettings(); }`, and `LoadSettings` walks VIEWS — the marker
lane, the dock page and its pages, the master slider, the panes. The dock-pages test
already knew this and wrapped its call in `Lock()/Unlock()`; the preferences test did
not.

The VM's kernel log names it exactly:

```
KERN: 41253: DEBUGGER: Looper must be locked.
KERN:   BView::Invalidate() + 0x1d
KERN:   daw::MainWindow::LoadSettings() + 0x357
KERN:   TestPreferences
```

A Haiku assertion is not a failed check: `debug_server` stops the whole team and the
run hangs until it is killed (which is why the coordinator saw one FAIL and then a
200 s+ hang, and why `ctest` reported it through its TIMEOUT).

**What held what:** the *test* thread held no window lock and called a window method
that touches views — a BView call whose owner lock is not held. The window's own looper
was never involved. Two things fixed it:

1. `MainWindow::ReloadSettings()` now takes the window lock itself when the caller is
   not the window thread (`find_thread(NULL) == Thread()` → run inline; else
   `Lock()/LoadSettings()/Unlock()`). A caller that already holds the lock still works,
   so the dock-pages test needed no change.
2. `Theme.h`'s `ApplyThemeToAllWindows()` — the one funnel every theme change walks —
   locked each window with an UNBOUNDED `BWindow::Lock()`. It ran on the main window's
   looper, so a window whose own looper was busy held the app's front door for as long
   as that took. The wait is bounded now (1 s, skip on timeout): a window that cannot be
   locked keeps the previous mode's cached colours until its next theme change, which is
   a cosmetic miss and not a frozen application.

A third wedge of the same family turned up behind it (see "the templates test" below).

## Defect 2 — the buffer and the UI-scale semantics, made honest

The failing check was the LIVE UI scale (`ThemeScale() == 1.25`), not the buffer.

* **Buffer size — "applied at the next Play".** That is what the app has done since the
  Audio menu existed (`ApplyPrefs` sets `fTransportCtl.fBufferFrames`; `StartPlayback`
  hands it to the engine and re-opens the device only if the value changed). It is now
  what the row SAYS (`"Applied at the next Play."` under the row) and what the test
  checks: the app's value *and* the engine the next Play builds (`MSG_PLAY` →
  `EngineBufferFramesForTest() == 1024`), so a value nothing read could not pass.
* **UI scale — applied live.** `ApplyPrefs` already applied it, but `LoadSettings` only
  *remembered* it: after a reload the app could hold 125% while drawing at 100% — the
  Preferences row showed "125%" over a 100% layout, and a later change to the same value
  was a no-op that never reached the screen (which is exactly the failing check, and it
  was reproducible from a settings file an earlier crashed run had left at 1.25).
  `LoadSettings` now applies what it reads (override + theme walk), so what the app
  remembers and what it draws at are one number. The row says so: `"Applied now; window
  sizes settle at the next launch."`
* The test's teardown now restores the values the run STARTED with, matched by VALUE
  rather than by menu index, so a file left behind by an earlier run cannot turn the
  restore into a different change.

## Defect 3 — the New Project dialog quit itself under a caller's lock

The next wedge, found while verifying: `NewProjectWindow::PostChosen()` called `Quit()`
on whoever's thread reached it. The test drove the dialog with its lock held, so the
`Unlock()` after `PostChosen` ran on a destroyed looper — the same `"looper must be
locked before proceeding"` stop, this time in `TestNewProjectTemplates` (also in the
kernel log, with `BLooper::Unlock()` and `TestNewProjectTemplates + 0x676`). The quit is
now posted to the window's own looper unless the call is already on it.

## What the screenshots showed (and what was fixed)

I ran the `DAW_UI_SHOTS` pass **myself**, once with `DAW_UI_THEME` unset and once with
`DAW_UI_THEME=dark`, 79 shots each, and opened every shot of every window this branch
touches. (The VM's screen was blanked when I started — a keypress woke it, and an
unanswered debug_server dialog from the wedged run was dismissed by clicking its
"Terminate" — so the draws the pass needs actually happened. Nothing on the VM was
reconfigured: input only.)

Shots opened: `transport-bar`, `transport-bar-playing`, `mixer-strips`, `preferences`,
`project-settings`, `project-settings-applied`, `new-project`, `new-project-band`,
`startup`, `docked-editor`, `fx-window-track`, `fx-window-retargeted`, `meter-playing`,
`meter-clipped`, `dock-theme-other`, `big-project-playing` — each in both modes (the
dark-mode counterparts of the first eight in `shots2-dark`, after the fixes). What they
showed and what changed:

1. **The loop button read as the letter C** (an open arc whose head was lost in a 22 px
   button) and **the metronome was a bare triangle**. Both are drawn as the icons they
   are now: a ring with a solid head, and a trapezoid body with a base and a leaning
   pendulum. Re-shot and verified at 4× in both modes.
2. **The New Project dialog did not fit its text**: it called `ResizeToPreferred()`
   BEFORE `UpdateDescription()`, so it was as wide as an empty string — the description
   ran off the right edge mid-word and the **Create button was pushed out of the window
   entirely**. The text now goes in first and a template change re-fits. Verified: full
   description and both buttons visible in System and dark.
3. **Every menu field in the app was unreadable in Dark mode** (Preferences, Project
   Settings, New Project, the export dialog): a `BMenuField`'s inner `BMenuBar` draws the
   marked item's label with `ui_color(B_MENU_ITEM_TEXT_COLOR)` — the *system* colour
   table, which this app may not write (T1) — so it was black text over the app's own
   dark well. `DawMenuField` now hides the inner bar and draws the value itself in the
   theme's colour; the click and the keys that used to reach the bar open the menu from
   the view (`BPopUpMenu::Go`, the same idiom the inspector and the effects view use).
   Verified in both modes at 4×. The *click* was checked on the VM by posting a
   mouse-down to the field: the pop-up opens and, being a real menu, takes only real
   input — exactly the behaviour the suite's own notes describe for pop-up-opening
   controls (it cannot drive them, and neither could my synthetic `sendkey`s).
4. Everything else the pass touched was already right: the two-row transport strip
   (buttons, LCD with mode tag, tempo/meter/DSP/xrun line, status line, meter, and the
   documented fallbacks at a 840 px window), the mixer strips with the trim knob, its
   value, the Ø button, the send rows and the G box (no overlap, values legible), the
   Preferences rows and their new hints, the docked editor, the effects window.

## Two test fixtures that were racing (found on the way)

* **The region-job test** waited for the progress bar to CLOSE — which is already true
  before it opens — so it returned at once and its cleanup deleted the test's own source
  file while the worker was still decoding it, then read the clip back by the id the op
  had just replaced. It now waits for the bar to appear, then to close, then for the clip
  to change, and never removes the source. (Found by instrumenting the job: the worker's
  `access()` said ENOENT for a file the test thread had just stat'ed at 3.8 MB.)
* **The effects drag test's point was in design pixels** while the panel's geometry is
  `Themed()`; it is themed now.

Two diagnostics stayed, because "cannot open" on a file that is plainly there cost an
afternoon here: `WavSource::Open` prints the errno, and the region jobs name the path
and the step that failed.

## Verification (all at the head of this branch)

| Command | Result |
|---|---|
| `ctest --test-dir build-host` (host) | **71/71 passed** |
| `sh scripts/vm.sh ssh 'cd ~/haiku-daw && ctest --test-dir build'` | **73/73 passed**, exit 0 (`ui_functional_tests` 51 s) |
| `cmake -B build-off -DDAW_LV2=OFF && cmake --build build-off -j6` | **exit 0** |
| `ctest --test-dir build-off` | **68/68 passed**, exit 0 (`ui_functional_tests` 50 s) |
| `sh scripts/haiku_syntax_check.sh` | **81 OK, 4 FAIL** — the four pre-existing FLAC/vorbis files (the cross toolchain has no FLAC headers; untouched by this branch) |
| `DAW_UI_SHOTS` pass, System and Dark | **960 checks, 0 failures** each, 79 shots each |

One incident worth recording: a `ctest --test-dir build` run timed out in the middle of
this pass, and the cause was **my own** leftover `./daw` — the app I had started by hand
for the click experiment was still running (Haiku has no `pkill`; the first attempt to
stop it silently failed), so two instances shared the device, the settings file and the
`/tmp` fixtures. `kill`ing it and re-running gave 73/73 and 68/68 again. The suite is
sensitive to that state, which is worth knowing for the integration machine.

## Not verified / honest notes

* The menu field's pop-up could not be *chosen from* with synthetic input (a menu takes
  real pointer/keyboard events). What is verified: the click opens it, the field draws
  its own value in both modes, and the suite (which marks items directly, by design)
  stays green in both build dirs.
* The suite's own `TestPreferences` cannot run at any other scale than the scale the
  settings file carries; with the value applied on load this is now consistent (a stale
  file no longer makes the check fail — it makes the whole run start at that scale).
