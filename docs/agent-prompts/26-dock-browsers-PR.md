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

---

# T2 review follow-up (2026-10-10)

A code review of this commit arrived with ten findings, explicitly unverified.
All ten are real; each was checked against the code (Haiku's own kit sources are
on the review machine, `/home/marc/haiku-cross/haiku/src/kits/`) and now has a
functional check that fails without its fix. One more defect fell out of driving
the dock's controls for real. Nothing was refuted — the notes below say how each
one was confirmed, because "confirmed" has to mean something specific here.

## The ten, and what was wrong

1. **The browser views' controls and lists posted to their WINDOW.** Confirmed
   in the kit's source: `BControl::AttachedToWindow` and
   `BListView::AttachedToWindow` both do `SetTarget(Window())` when no target is
   set, and neither MainWindow nor the browser windows handle
   `'srch'`/`'pick'`/`'tbpm'`/`'pbfl'`/`'pbpk'`. The old test posted `'pbpk'`
   straight to the view, which proves nothing about the target — that is how a
   browser whose filter, Search, Tag BPM and both picks were dead passed. Each
   view now `SetTarget(this)` on its controls after the base
   `AttachedToWindow` (the base is what attaches the children; the kit's own
   attach is what had put the window target there).

2. **`MSG_DOCK_PAGE` read an `"index"` nothing ever sent.** `BControl::Invoke`
   adds `when`/`source`/`be:value`/`be:sender` and nothing else (Control.cpp),
   so every click and arrow key reached the window as index 0 and
   `SetDockPage(0)` showed the editor page: the page switch was unreachable by
   mouse and keyboard alike. `DawSegments::Invoke` now adds the segment it
   selected (the contract its own header documents), and the window *ignores* a
   `MSG_DOCK_PAGE` without an `"index"` instead of guessing Editor.

3. **`OpenDockedEditor` added the roll without removing the browser page.**
   Confirmed by reading: `ClearDockedEditor` only takes the browser views out
   when the page is already Editor, so with Samples showing the roll became a
   second child of the body (they split the pane) while the strip still named
   the browser. It goes through `SetDockPage(kDockEditor)` now.

4. **`LoadSettings` restored the page number and the segment highlight but never
   materialised the page.** Confirmed by reading; the state was reachable only
   across a restart, which is why no test had it. The restore now goes through
   `SetDockPage`, and `MainWindow::ReloadSettings()` (a test hook beside
   `IsPlaying()`) lets the suite drive the restore with the dock hidden, the
   page saved as Plugins.

5. **`SetDockPage` ended with `SetDockShown(true)`**, so the track selection —
   which re-points the plugin page — re-opened a dock the user had closed, every
   time a track was clicked, and right after a restart with the page restored
   while the dock was down. Showing is the callers' business now
   (`ShowDockBrowser`, `OpenDockedEditor`); the check is "a closed dock stays
   closed while the mixer selects a track, and the strip re-titles itself".

6. **`EffectsView::Retarget` dropped a pending wheel commit and an in-flight
   drag.** Confirmed: the engine had already heard those values
   (`kMsgFxLive` on every drag move and wheel notch), so the model — and the
   saved project — stayed on the old value while the audio played the new one
   until the next rebuild reverted it, exactly what `QuitRequested` flushes
   against. `Retarget` now settles them onto the chain it is leaving before the
   swap. (The spec of this package said "drag state and any pending debounced
   commit are dropped"; the spec was wrong, and this is the note that says so.)

7. **The theming regressed in the split.** Confirmed: `PluginBrowserView` lost
   its attach-time theme entirely (a page created in Dark mode kept the system's
   list colour until something else happened to walk the tree), both views
   cached the constructor's panel colour and never re-took it on a mode switch,
   and the two browser windows stopped being `ThemeAware`, so the walker treated
   them as stock windows. The views derive from `ThemedGroupView` and re-take
   the well colours in `ApplyTheme`; the windows are `ThemeAware` again.

8. **`fSelTrack` did not follow the selection everywhere.** Confirmed: a mixer
   strip click moved the inspector but not the page (so "add to X" and the
   inspector disagreed), and File ▸ New/Open left the page on a track id from
   the project that went away — which the new project may reuse. Both paths go
   through the same `SelectTrack()` the timeline's own selection uses.

9. **The boot-volume query ran on every re-attach whose list was empty.**
   Confirmed by reading `AttachedToWindow`: the dock's page is re-attached on
   every page switch, and a filter that matches nothing (or a volume with no
   tagged samples) leaves the list empty, so the whole query re-ran on
   MainWindow's looper each time. A "queried once" flag, set even when the
   result is empty.

10. **`ShowDockBrowser` did `SetDockPage`'s work twice** (two `SetTrack` calls,
    view creation, `fDockPage`), leaving three places that set the page and the
    track to drift apart. It is now: `fSelTrack` if this is the plugin page and
    a track was named, then `SetDockPage`. Plus the duplicate
    `<LayoutBuilder.h>` include and the mis-indented `s.bottomVisible` line —
    both confirmed by reading.

## Found while fixing: the page switch ate the Space bar

Driving the segmented control for real exposed it: `DawSegments` is
`B_NAVIGABLE` and takes the focus on a click, and a focused `BControl` eats
Space in the kit's own `KeyDown` (`B_SPACE` toggles its value and invokes). So
after clicking a page, Space re-sent the *same* page and the transport never
started, however many times it was pressed. It showed up as the run's
`big project playback` check failing at its 5 s wait — the transport simply
never started, not a slow start.

`MainWindow::DispatchMessage` now routes Space to the transport whatever control
holds the focus, except a text field being typed into (the rule the aux windows
already apply to their own spacebar passthrough). Arrows still belong to the
focused control. The check: after a page click, Space plays and stops the
transport.

## Tests

`TestDockPages` is rewritten to drive the REAL widgets instead of the ids they
would post: a synthetic mouse-down on the segmented control (its own
`SegmentRect` geometry, computed in the test), the sample browser's field
`Invoke` (what Enter does) and its list `Invoke` (what a double-click does,
after selecting the row the way the click before it would), the plugin page's
filter and list the same way, a region opened over a browser page, a settings
reload with the dock hidden and the page saved as Plugins, a theme flip with a
page detached and the well colours asserted in both modes, a closed dock staying
closed while the mixer selects a track, and Space after a page click.

`TestFxRetargetFlush` (new) opens the real effects window on an existing track
with one Limiter, drags the first knob (a synthetic mouse-down and move; the
panel's hit regions exist once it has drawn), retargets the window to another
track mid-drag, and asserts the MODEL of the chain it left holds what the engine
had already played, and that the one window's title now names the new track.

`TestThemeMode` and the colour checks report the two colours when they differ —
a bare `==` in a `CHECK` says nothing about which side was wrong.

## Verification

| Check | Result |
|---|---|
| Host build (exit 0) + `ctest --test-dir build-host` | 53/53 |
| ASan+UBSan `b-asan` | 53/53 |
| `scripts/haiku_syntax_check.sh` | 0 FAIL (67 files) |
| VM `build` ctest | 55/55 |
| VM `build-off` ctest | 51/51 |
| `ui_functional_tests`, System/light | **285 checks, 0 failures** (was 236) |
| `ui_functional_tests`, Dark | (filled in below) |
| Screenshot pass, System mode (light Appearance) | (see below) |
| Screenshot pass, Dark mode | (see below) |

Mutation checks (each broken on purpose in its own commit, built on the VM,
seen to fail, restored — nothing is on the branch):

1. `DawSegments::Invoke` back to `BControl::Invoke` (no `"index"`) → the page
   switch never happens; the page/content/colour checks fail.
2. The browsers' `SetTarget(this)` removed → the sample search and pick and the
   plugin filter and pick checks fail.
3. `OpenDockedEditor` adding the roll without the page switch → the "editor over
   browser" checks fail.
4. `SetDockShown(true)` back in `SetDockPage` → "a closed dock stays closed"
   fails.
5. `Retarget` dropping the in-flight edit → `TestFxRetargetFlush`'s model check
   fails.
6. The browser views' attach-time `ApplyTheme` removed → the panel-colour checks
   fail.
7. The query flag back to "while the list is empty" → the no-re-query check
   fails.

