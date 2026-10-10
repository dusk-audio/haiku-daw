# T2 docked browsers + single-instance effects window — PR record

Branch `feature/dock-browsers`, off `feature/theme-modes` (T1 is unmerged and is
the prerequisite this task's review needs: a UI change is looked at in both
theme modes now, so the branch carries T1 until Marc merges it). Spec:
`26-dock-browsers.md`.

## What changed

### The browsers became views, with the windows kept as hosts

`PluginBrowserView` and `SampleBrowserView` hold everything the windows used to
build — the filter, the list, the catalogue/query, the choice — and
`PluginBrowser` / `SampleBrowser` are thin `BWindow` hosts around them. Every
message id and field is unchanged (`kMsgPluginChosen` to the inspector,
`kMsgBrowserImport` and `kMsgSampleDrag` to the main window/timeline, the
spacebar passthrough in the hosts). The sample browser's content was rebuilt
with the Layout Kit: it had absolute rectangles measured from its own window's
bounds, which a pane of any size cannot have.

### The dock has pages

The strip carries a **segmented control** (new kit control `DawSegments`, drawn
through the control look like the rest of the kit: stock in System mode,
`DawControlLook`'s in Dark): **Editor** (the docked roll, or the hint that says
how to open one), **Samples**, **Plugins**. The body holds exactly one page —
the old one leaves the group, because a kept page keeps its size (the same
lesson the split items taught in M1.4). The strip's title names the page, and
on the Plugins page it names the chain a pick will land in ("Plugins — add to
kick"); **Pop out** now hands whichever page is showing to its own window, and
the page persists in `AppSettings` as `uidockpage`.

The browsers' entry points follow the content: View ▸ Sample Browser and the
inspector's empty insert slot (and the mixer strip's Add Effect) show the dock
on the right page, pointed at the right track, instead of opening a window.

### One effects window

`MainWindow::ShowFxWindow(track, focusSlot)` is the single place that decides:
retarget + show + activate the window that exists, or create it. All four open
sites go through it (the inspector's two post `kMsgShowFx`). `EffectsWindow`
handles `kMsgFxRetarget` — the view takes the new chain/track/focus, drops any
drag or pending debounced commit that belonged to the old chain, refits its
height and retitles — and re-announces itself so the meter follows the track.

### The two M1.4 leftovers

- The generic parameter panel's slider row now draws a **handle** at the value
  position, clamped inside the trough. At the minimum — where a fresh insert's
  parameters sit — the row used to be an empty trough with nothing to grab.
- The window's title names the **track** as well as the insert
  ("EQ (5-band) — kick", "Effects — Master"), which is what telling two chains
  apart takes when there is only one window.

### Found while looking: the window could not be made narrow

The screenshot pass caught it, and a check now guards it: a layout view without
an explicit minimum reports its **current frame** as its minimum, so the
timeline's width became the window's floor. Once the window had been wide (a
restored frame, or the 150% layout pass) it could not be narrowed again, and on
the VM's 1024-wide screen the right end of the window — the dock's own Pop out
and × — sat off the screen. Both panes now declare small minimums
(`SetExplicitMinSize`) and the restored frame is clamped to the screen.

## Verification

| Check | Result |
|---|---|
| Host build (exit 0) + `ctest --test-dir build-host` | 53/53 |
| ASan+UBSan `b-asan` | 53/53 |
| `scripts/haiku_syntax_check.sh` | 0 FAIL (67 files) |
| VM `build` ctest | 55/55 |
| VM `build-off` ctest | 51/51 |
| `ui_functional_tests` | **236 checks, 0 failures** (was 220), both modes |
| Screenshot pass, System mode (light Appearance) | 32 shots |
| Screenshot pass, Dark mode | 32 shots |

New checks in `ui_functional_tests` (`TestDockPages`): each page shows and the
others do not (`samplebrowserview` / `pluginbrowserview` by name), the strip's
own controls are **inside** the strip, the docked plugin browser's pick reaches
the inspector as a `SetFxCommand` (the track's chain grows to one insert), one
effects window is opened twice and retargeted (window count unchanged, title
naming the second track), and the new shots. `appsettings_tests` covers the
page round trip.

Mutation checks (each broken on purpose, seen to fail, restored):

1. `SetDockPage` not removing the outgoing page (the pages then overlap and the
   "the other page is gone" checks fail).
2. `ShowFxWindow` never reusing the window: a second one is left behind and the
   window-count check fails.
3. `PostChoice` not sending `kMsgPluginChosen` (the chain stays empty and the
   `SetFxCommand` check fails).

## For Marc

- Shots: `docs/agent-prompts/shots-26/{light,dark}/` (32 each, in test order;
  the dock pages are `19`–`22`, the effects window `23`/`24`).
- The branch sits on top of T1 (`feature/theme-modes`), so its PR should wait
  for T1 or be retargeted after it merges — say which you want.
- Still a click list: the dock's pages with a real project, Pop out from each
  page, and a plugin with an internal state opened, retargeted and closed.
