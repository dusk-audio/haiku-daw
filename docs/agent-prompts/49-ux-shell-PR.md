# Record — the UX-shell pass (`fix/ux-shell`)

Branch `fix/ux-shell`, cut from `master` (`0783e5e`). Spec:
`docs/agent-prompts/49-ux-shell.md`. Scope: a ranked UX review's findings A–E
— the keyboard map dying on a focused control, no kit button reachable by Tab
and Return never accepting a dialog, Escape cancelling nothing, feedback the
app never gives, and one vocabulary pass. Not a redesign: every item is the
one the spec named, and the two places where the spec left a choice are
settled below and argued.

## The decisions the spec left open

**The vocabulary.** Settings-style windows (the window holds values, and
leaving may discard them) are **Cancel + Apply**: Apply commits the whole view
as one change, Cancel and Escape leave without applying what was not applied,
and the old "Close" is gone from both. Action dialogs are **Cancel + a verb**.

| Window | Before | Now |
|---|---|---|
| Preferences | "Close", **instant apply** on every control change | Cancel + Apply, Apply is the default; a pick or a tick is a setting until Apply |
| Project Settings | "Close" (discarded) + Apply | Cancel + Apply (behaviour unchanged, the label now says which) |
| New Project | Cancel / Create | unchanged |
| Export options | Cancel / "Export" \[sic: "Export stems" for stems\] | Cancel / "Export Mix" or "Export Stems", matching the title and the file panel |
| Rename | "OK" only | Cancel / OK |
| Quantize | "Quantize" only | Cancel / Quantize |

Preferences is the one **behaviour** change here, and it is deliberate: the
finding is that "Close" meant "done" in one window and "discard" in the
sibling, and there is no way to make one word mean both. Since the settings
vocabulary is Cancel + Apply, Preferences had to stop applying on every click
— otherwise its Cancel would be a lie. Its hints say what Apply does
("Applied at the next Play", "Applied now; window sizes settle at the next
launch"), and the window still never touches the model or the settings file:
Apply posts the whole `PrefsView` through the same `kMsgPrefsChanged` funnel
it always used.

**The key-routing rule.** The spec's rule is implemented literally: a text
field being typed into owns the keyboard; otherwise the key table decides. A
key the table binds is the app's, whatever control has the focus — including
the arrows (they are bound to pan), so a focused slider no longer changes with
Left/Right; a key the table does not bind (a list's Up/Down, a view's own
editing keys) stays with the focused control. `TimelineTakesKey()` in
`src/model/KeyMap.h` is that rule, kit-free so the host suite checks it; menu
accelerators (`KeyRoute::Menu`) are never the timeline's.

**The Plugins page.** The spec offered "a status line or select the first
track". The status line was chosen: silently moving the selection would also
move the inspector, and the page's title already names the chain a pick lands
in, so a title that asks for a selection plus a pick that answers with
"Select a track first" is the honest pair.

## What landed, finding by finding

**A — the map survives a focused control.** `MainWindow::DispatchMessage` no
longer computes `held` from "anything but Space"; it asks
`TimelineTakesKey(bytes[0], mods, textFocus)`. `src/model/KeyMap.h` gained the
predicate and its doc comment; `tests/keymap_tests.cpp` checks both halves
(bound keys with a non-text focus, unbound keys and menu accelerators without
one) on the host.

**B — Tab and Return.** `DawButton` derives from `BButton` (as `DawSlider`
derives from `BSlider`) and its default flags carry `B_NAVIGABLE`. The cast
the spec suggested is **not safe**, and checking Haiku's own source is what
settled it: `BWindow::SetDefaultButton(BButton*)`
(`src/kits/interface/Window.cpp:1813`) stores the pointer and calls the
**virtual** `BButton::MakeDefault(bool)` on it, and `BWindow::_DetermineTarget`
(`:3284`) hands a `B_ENTER` key to whatever that pointer is. A `DawControl*`
cast to `BButton*` would dispatch through a vtable slot its class does not
have. With a real `BButton` base there is no cast at all, `MakeDefault`
registers with the window, and `BWindow`'s Return routing works unchanged.
Two details worth keeping: the registration is deferred to
`AttachedToWindow` (the dialogs call `MakeDefault(true)` before the button has
a window), and `MakeDefault(false)` only clears the button's own state —
`BWindow` calls it from *inside* `SetDefaultButton`, so re-entering that call
there would recurse.

The dialogs already called `MakeDefault(true)`; that call is now the
registration. Preferences gained an Apply button and made it the default, so
all six named dialogs have one.

**C — Escape.** `B_CLOSE_ON_ESCAPE` on Rename, Quantize, the export options,
New Project, Project Settings and Preferences (Haiku's `_HandleKeyDown` turns
the key into `B_QUIT_REQUESTED`, which every one of those windows already
handles as "leave"). Rename and Quantize gained the Cancel button they lacked,
posting the same message, so the visible and the keyboard way out cannot
diverge.

**D — feedback.** `FinishExport` sets the status line on all three outcomes
("Exported mix.wav" — file base name only, the bar truncates; "Exported N
stems"; "Export cancelled"; "Export failed" beside the existing alert).
`StartExport`'s refusal says "A render is already running", the region jobs'
words. `SetTransportStatus` timestamps its message and
`MainWindow::ExpireTransportStatus` (called from the existing 2 Hz `MSG_TITLE`
poll — no second runner) restores the transport's own state line after 4 s.
Both device-failure alerts name the real path ("The buffer size is in File ▸
Preferences.") instead of the Audio menu item M3.5 removed.

**E — vocabulary and titles.** As tabulated above, plus: File ▸ New carries
the ellipsis the other dialog-opening items do; the Plugins page's title asks
for a selection and a pick with none answers on the status line
(`kMsgPluginNoTrack`, posted by the browser to a status messenger the dock
passes in — the page's own target is the inspector, which is where a *real*
pick goes, so the refusal needed its own address); and the Preferences device
row now reads "Output device: System default" with "The system's default
output device is used; choosing one is not available yet." where it carried
the internal roadmap line about the multichannel-input package.

## Commits

| # | Subject |
|---|---|
| `b8e54a3` | docs: spec the UX-shell package (fix/ux-shell) |
| `d728ec2` | fix(ui): the keyboard map belongs to the app, not to the focused control |
| `68a959b` | fix(ui): a kit button takes Tab, and Return accepts the dialog |
| `f0f263e` | feat(ui): one way out of every dialog, and one vocabulary |
| `35ebbc2` | fix(ui): render jobs report on the status line, and messages expire |
| `95df51b` | test(ui): the UX-shell checks, VM-side |
| `74d5333` | docs: the UX-shell record (fix/ux-shell) |
| `b7e9385` | test(ui): pin the render-feedback fixture's export choices |
| `8bcf2be` | docs: name the test-pin commit in the UX-shell record |
| `24f0420` | fix(ui): the unlatched toggles and the flags that never applied |

**Reopened once** — the integration's first VM run found three failures in this
package (and wedged at the third). All three are fixed in `24f0420`; the
section at the end of this record is their root-cause write-up.

`b7e9385` is a fix to the test added one commit earlier: by the point
`TestRenderFeedback` runs, an earlier test has left the remembered export
range on "Loop range", and a one-second bounce can finish before the second
export request is handled — the refusal would then pass or fail on timing
rather than on behaviour. The test pins whole-project WAV first, which is
also what makes its first render a long one.

Each commit leaves the tree it touches self-consistent (checked by grepping
the commit's own blobs for the symbols it introduces): A adds
`TimelineTakesKey` and its only caller together, B's `DawButton` compiles
against every existing use, E removes `kMsgPrefsEdited` and its last user
together, D adds `ExpireTransportStatus` and its call site together.

## Verification (this host)

| Command | Result |
|---|---|
| `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release` | exit **0** |
| `cmake --build build-host -j2` | exit **0** |
| `ctest --test-dir build-host` | **100% tests passed, 75/75** |
| `sh scripts/haiku_syntax_check.sh` | **83 OK, 4 FAIL** (the four pre-existing `FlacSink/FlacSource/VorbisSink/VorbisSource` files, untouched here — the script exits 1 on them on `master` too) |

`tests/keymap_tests.cpp` gained 24 checks for the routing predicate (1120
checks, 0 failures, up from 608).

## Mutation checks

Actually run on this host, each watched fail and then restored:

| # | Mutation | Caught by |
|---|---|---|
| M1 | `TimelineTakesKey` returns false always (the rule as it was) | `keymap_tests`: 9 failures (every bound-key check) |
| M2 | the `KeyRoute::Dispatch` filter dropped | `keymap_tests`: 3 failures (the Cmd-N/Cmd-S "menu's key" checks) |
| M3 | `textFocus` ignored | `keymap_tests`: 3 failures (the three text-field checks) |

Written for the VM suite, and **not runnable on this host** (the suite needs
an app_server and only builds on Haiku) — the orchestrator's pass runs them;
the mutation each is there to catch is named so a reviewer can check the claim
by hand:

| # | Mutation it catches | Where |
|---|---|---|
| M4 | `DawButton` default flags lose `B_NAVIGABLE` | `TestWidgetKit` (`button->Flags() & B_NAVIGABLE`) |
| M5 | `MakeDefault` back to "private flag + border" (no `SetDefaultButton`) | `TestWidgetKit` (`probe->DefaultButton() == button`) and every `CheckVocabulary`'s default check |
| M6 | `B_CLOSE_ON_ESCAPE` dropped from any of the six windows | `CheckVocabulary` (the flags check) |
| M7 | Rename's or Quantize's Cancel button removed | `CheckVocabulary` (the `cx` lookup + label) |
| M8 | `FinishExport` success without a status line | `TestExportFlow`'s wait for "Exported …haiku_daw_ui_export.wav" |
| M9 | `StartExport`'s refusal silent again | `TestRenderFeedback` (the second export must say "A render is already running") |
| M10 | `SetTransportStatus` without the timestamp (no expiry) | `TestRenderFeedback`'s wait for "Stopped" after the 4 s hold |
| M11 | `PluginBrowserView::PostChoice` inert again with no track | `TestDockPages` (the "Select a track first" status) |
| M12 | the dock title back to "add to no track selected" | `TestDockPages` (the title check) |
| M13 | Preferences' menus carrying a change message again (instant apply) | `TestPreferences` ("the app is untouched" before Apply) |
| M14 | Preferences' Cancel applying instead of discarding | `TestPreferences` (the look must not have switched after Cancel) |
| M15 | the Export window's title back to "Export" | `CheckVocabulary` + `FindExportDialog` |
| M16 | the Preferences device hint back to the roadmap line | `TestPreferences` (the "not available yet" copy + the "System default" row) |

## What only the VM can prove, and what to look for

- The whole `ui_functional_tests` addition (the mutation table above) — this
  host cannot run it. Nothing here has been observed on a screen.
- The screenshot pass, both theme modes, and in particular: **a default
  button draws differently** (the look's `B_DEFAULT_BUTTON` frame) — compare
  `widget-kit-default` with the earlier `widget-kit` shot, and the Rename /
  Quantize / both export dialogs (new shots: `rename-dialog`,
  `quantize-dialog`, `export-stems-dialog`) for a Cancel beside the verb, the
  verb's own wording, and the accent frame on it.
- The Preferences window: Cancel + Apply on the right, the "Output device:
  System default" row with its note, and the level readout still moving while
  the slider is dragged (it no longer applies until Apply — the readout is the
  live part).
- The dock's Plugins page with nothing selected: the title should read
  "Plugins — select a track to add to", and a double-click should put
  "Select a track first" on the status line.
- A bounce's "Exported <name>" and its disappearance after a few seconds
  (`export-progress` / `export-panel` shots are before it).
- 150% scale is the riskiest for the two new button rows (Rename's and
  Quantize's second button): the dialogs size themselves to content, so the
  shot pass at 150% is worth the look.

## Things in the findings that were wrong, when I got to the code

- **The cast in B is not the fix it looks like.** `BWindow::SetDefaultButton`
  calls the virtual `BButton::MakeDefault` on the stored pointer; a button
  that is not a `BButton` cast to one would call through a vtable slot its
  class does not have. The spec's own "check what Haiku's API wants" is what
  settles it: the API wants a real `BButton`, so that is what `DawButton` is
  now.
- **Where the review undersold the work.** Two places: E's "one pass, no
  redesign" is a real behaviour change for Preferences (a settings window's
  Cancel cannot mean anything while the window applies on every click), and
  the Plugins page could not "just answer with a status line" — the docked
  page posts its picks to the *inspector*, so the refusal needed a messenger
  of its own (`kMsgPluginNoTrack` to the main window) rather than reusing the
  existing one.
- Everything else (A's line numbers and the swallowed keys, C's two "Close"
  meanings, D's three silences and the stale menu path, E's titles, ellipsis
  and copy) was exactly as the review described it.

## Reopened: the VM pass's three failures (all fixed in `24f0420`)

The first VM run of the merged `feature/ux-pass` reported three failures in
this package:

```
FAIL tests/ui_functional_tests.cpp:388   (w->Flags() & B_CLOSE_ON_ESCAPE) != 0
FAIL tests/ui_functional_tests.cpp:971   WaitFor(... !WindowListed("Export Mix") ...)
FAIL tests/ui_functional_tests.cpp:4651  probeState(kMsgKitToggle, B_CONTROL_OFF)
```

…and then **wedged**: no print after the third line, the process killed at
600 s. Root causes, in the order they matter:

**1. `DawToggle` stopped latching — a real app regression, every toggle in the
app.** `BButton::Invoke` (`src/kits/interface/Button.cpp:451`) ends with
`if (fBehavior != B_TOGGLE_BEHAVIOR) SetValue(B_CONTROL_OFF);`. `Invoke` is
virtual, so once `DawButton` derived from `BButton` its clicks went through
that: the flip was sent (the message carried ON) and then the control's value
was wiped — the lamp held OFF while claiming ON, and the next click flipped
OFF→ON again, which is why the test's second click reported ON. Fixed by
overriding `Invoke` in `DawButton` to call `BControl::Invoke` — what the kit
had before the base class changed (BButton's also `Sync()`s the window and
sleeps 50 ms per click). The widget-kit test's second click is the regression
test, and it now also checks that the control **holds** what it reported.

**2. `B_CLOSE_ON_ESCAPE` never reached three of the six dialogs.** `BWindow`
has two constructors — `(frame, title, window_type, flags, workspace)` and
`(frame, title, window_look, window_feel, flags, workspace)` — and

```c++
BWindow(frame, title, B_TITLED_WINDOW, B_FLOATING_APP_WINDOW_FEEL, theFlags)
```

resolves to the **first**, because `B_TITLED_WINDOW` is a `window_type` and the
five arguments fit. Everything after the feel went into `workspace`; the only
thing left in `flags` was `B_FLOATING_APP_WINDOW_FEEL` = `0x4` =
`B_NOT_H_RESIZABLE`. Export, Rename and Quantize had been running with no
Escape, no `B_NOT_ZOOMABLE`, no `B_NOT_RESIZABLE` — and no way for the pass's
flag to take, which is exactly what line 388 saw. The other three
(New Project, Project Settings, Preferences) used the four-argument form,
which does put the flags in the flags slot, with a stray feel bit riding
along. All six now write the look and the feel as separate arguments
(`B_TITLED_WINDOW_LOOK`, `B_NORMAL_WINDOW_FEEL`), which is the form the kit
already uses correctly in `MarkersWindow.cpp:205`. **Look and feel are
unchanged from what the windows actually were** (titled, normal — the
"floating" the M1.4 record believed in never happened either), so the only
behaviour deltas are the flags that now apply.

Proven on the host with a shim of `BWindow`'s two overloads and the real
constant values (`B_FLOATING_APP_WINDOW_FEEL` 4, `B_NOT_ZOOMABLE` 0x40,
`B_NOT_RESIZABLE` 2, `B_ASYNCHRONOUS_CONTROLS` 0x00080000,
`B_CLOSE_ON_ESCAPE` 0x00800000):

```
5-arg today -> window_type(type, flags, workspace)       look 1 feel 0 flags 0x00000004 workspace 0x00880042  ESCAPE=LOST
4-arg today -> window_type(type, flags, workspace)       look 1 feel 0 flags 0x00880046 workspace 0x00000000  ESCAPE=SET
fixed       -> window_look(look, feel, flags, workspace) look 1 feel 0 flags 0x00880042 workspace 0x00000000  ESCAPE=SET
```

**3. The hang was a test fault: two off-lock `BView::Flags()` reads.**
`BView::Flags()` is not a plain getter — it runs `_CheckLock()`
(`src/kits/interface/View.cpp:1020`), which calls the debugger when the
view's window lock is not held by the calling thread. The widget-kit block
read `button->Flags()` and `toggle->Flags()` from the test thread with no
lock; the assertion raised the debug-server dialog and stopped the whole
team, which is why nothing printed after it (the HANDOFF's faulted-run
lesson). Both reads are under the probe's lock now, in one `CHECK`.

**4. The export flow stalled because the panel and the dialog share a
title.** `ShowPanel` titles the file panel "Export Mix" — and this pass
renamed the *dialog* to the same name. The wait was
`!WindowListed("Export Mix") && VisibleWindows() >= windowsBefore + 1`, which
can never come true once the panel is up. It waits for the dialog **by
pointer** now (as the shot window above it always did), and `FindExportDialog`
documents that its title lookup is only unambiguous with no panel up.

### The same constructor trap, outside this package (not touched here)

Three windows still write the five-argument form and so still lose their
flags: `ExportProgressWindow.cpp:15` (its `B_NOT_CLOSABLE` never applied — the
progress bar's close box works, which is why Cancel exists), and
`InstrumentWindow.cpp:74` / `SendsWindow.cpp:34` (their `B_NOT_ZOOMABLE`).
They are outside this pass's six dialogs, so they are reported rather than
changed; the fix is the same two-line spelling, with `MarkersWindow` as the
in-tree example.

### Verification after the fixes (this host)

| Command | Result |
|---|---|
| `cmake --build build-host -j2` | exit **0** |
| `ctest --test-dir build-host` | **100% tests passed, 75/75** |
| `sh scripts/haiku_syntax_check.sh` | **83 OK, 4 FAIL** (the same pre-existing codec files) |

The VM re-run is the orchestrator's; the three VM mutations this pass must
now catch are #4/#5/#6 in the table above under the new spellings, plus the
toggle-value check that caught root cause 1 in the first place.
