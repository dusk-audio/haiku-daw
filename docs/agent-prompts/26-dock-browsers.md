# Task: finish M1.4 — docked browsers, single-instance effects window (T2)

Branch `feature/dock-browsers`, off `feature/theme-modes` (T1 is unmerged and is
the prerequisite this task's review needs: every UI change is now looked at in
both theme modes, so the branch carries T1 until Marc merges it).

## Codebase orientation (read before coding)

- **The dock** (M1.4): `MainWindow::fDock` is a `BGroupView` (`MainWindow.cpp`
  ~492) whose first child is a `PaneHeader` — the title strip with **Pop out**
  and **×** — over `fEditorPane`, which holds either the docked `PianoRollView`
  (`OpenDockedEditor`, ~1921) or the empty-state hint (`fDockEmpty`/`fDockHint`).
  `SetDockShown` (~1905) adds/removes the dock from `fRootSplit` (a collapsed
  `BSplitView` item keeps its size in this Haiku). `HeaderButton()` (~143) makes
  the strip's small buttons. Pane state persists in `AppSettings`
  (`uiinsp`/`uibottom`).
- **UI_GUIDELINES §4:** no `BTabView` for this — a tab's label comes from its
  view's name and an unnamed view gives an empty tab; the strip is where the
  page switch belongs.
- **The browsers are windows.** `SampleBrowser` (`SampleBrowser.{h,cpp}`) and
  `PluginBrowser` (`PluginBrowser.{h,cpp}`) are `BWindow`s that build their
  content — a `DawTextField` filter, a `BScrollView`+`BListView`, and (samples)
  a BPM field and Tag button — in their constructors, with a `ThemedView` root.
  Their messages must not change:
  - `PluginBrowser` → `kMsgPluginChosen` (`'pbch'`) to its **target messenger**,
    which is the *inspector* (`MainWindow.cpp` ~1047, `InspectorView.cpp` ~462):
    the inspector owns the "turn a choice into a `SetFxCommand`" path
    (`InspectorView.cpp:518`), so the browser itself never touches the model;
    plus `kMsgMixFxAdd` from the mixer strip to open it.
  - `SampleBrowser` → `kMsgBrowserImport` to MainWindow, and its list rows
    initiate `kMsgSampleDrag` drops the timeline accepts.
  - Both forward the spacebar to the transport from their window
    (`DispatchMessage`).
- **The effects window is per-track.** `EffectsWindow` (`EffectsWindow.{h,cpp}`)
  is created fresh at four sites — `MainWindow.cpp:889` (master, View ▸ Master
  Effects), `:1345` (a plugin with no native editor → the generic parameter
  panel), `InspectorView.cpp:456` and `:615` — and announces itself with
  `kMsgFxWinOpen` / `kMsgFxWinClosed`, which is how MainWindow learns which
  chain to push to (`fFxMsgr`/`fFxTrack`, `PushChainToFxWindow`,
  `ValidateFxWatch`, `CloseFxEditors`). Chain updates already flow main → window
  as `kMsgFxChain` (`MainWindow.cpp:2127`), so the window can already be told
  about a *different* chain than it opened with.
- **The two M1.4 leftovers** (entry prompt §2): the generic parameter panel's
  slider row (`EffectsView::Draw`, ~735) draws a trough and an accent fill that
  is zero-width at the minimum, so **at value 0 there is no handle at all**;
  and the window's title (`EffectsWindow` ctor, ~1217) is
  `EffectDisplayName(...)` alone — an LV2 editor's title names the track too.
- **Tests:** `tests/ui_functional_tests.cpp` drives windows by posted message
  (`TestDockedEditor` is the dock's existing coverage) and screenshots each test
  under `DAW_UI_SHOTS`; the pass runs once per mode (`DAW_UI_THEME`).

## Goal

1. The dock has **pages — Editor, Samples, Plugins** — switched from a
   segmented control in its header strip, with the pages' contents being the
   browsers' own views (no duplicated UI, no changed messages).
2. **One effects window**: asking for it again (any track, any insert) shows and
   activates the existing one, retargeted.
3. The two leftovers fixed.

## Work items

1. **Browser content becomes a view.**
   - `PluginBrowserView : public BGroupView, public ThemeAware` holding
     everything the window builds today (filter field, list, `fAll`/`fShown`,
     `Rebuild`, `PostChoice`), with the same message ids and fields. Its ctor
     takes the same `(TrackId track, BMessenger target)`.
   - `SampleBrowserView` likewise (`(BMessenger target)`), including the BPM
     tag row and the drag index/drop support.
   - `PluginBrowser` / `SampleBrowser` stay as thin `BWindow` hosts (root +
     content view + the spacebar `DispatchMessage`), so Mixer/Inspector/Main
     window call sites keep working unchanged.
2. **Dock pages.**
   - The header strip gets a **segmented control** (a small kit control: N
     labels, one selected, `kMsgDockPage`), the title showing the current page
     ("Piano Roll — <track>", "Samples", "Plugins"), and the strip's controls
     changing with the page (Pop out only on the editor page).
   - The body holds one view at a time: the docked roll (or the hint) for
     Editor, a `SampleBrowserView` for Samples, a `PluginBrowserView` for
     Plugins. Switching adds/removes the child in `fEditorPane` (the split's
     items keep their sizes, so the dock itself stays put).
   - The browser views post to the **inspector** (plugins) and **MainWindow**
     (samples) exactly as the windows do today; the docked plugin browser needs
     a track for its `kMsgPluginChosen` (int64 "track"): the current selection
     when it opens, so the dock's page is retargeted when the selection changes
     (or the row is inert with no track selected — decide in the code and say so
     in the record).
   - The dock's current page persists in `AppSettings` as `uidockpage` (kit-free
     round-trip check in `appsettings_tests`).
3. **Single-instance effects window.**
   - `MainWindow::ShowFxWindow(track, focusSlot)` — one place that decides:
     retarget + show + activate the live window, or create it. All four open
     sites go through it (the inspector's two become a message to MainWindow).
   - `EffectsWindow` handles a retarget message (`kMsgFxRetarget`): the view
     takes the new chain/track/focus, drag state and any pending debounced
     commit are dropped, the scroll range and title are recomputed, the window
     is shown and activated.
   - The `kMsgFxWinOpen`/`Closed` bookkeeping must survive retargeting (the
     track it meters changes with the window).
4. **The leftovers**: a visible handle on the parameter slider row at any value
   (including 0), drawn from the theme's tokens; the effects window's title
   `"<insert> — <track>"` (Master for the master chain), with the openers
   passing the track's name so the window itself needs no model.
5. **Tests** (`ui_functional_tests`): the page switch (each page shown, its
   content view found by name, the strip naming it), the docked plugin browser's
   choice reaching the inspector as a `SetFxCommand`, the single effects window
   (open twice → one window, retargeted title, `CountWindows` unchanged), and a
   `Shot()` of each dock page and of the retargeted window. `appsettings_tests`
   gets the page round trip.
6. **Record** `docs/agent-prompts/26-dock-browsers-PR.md`: what moved, the
   message flow, the shots reviewed **in both modes**, mutation checks by name,
   and the click list (the dock's pages with a real project; pop out with each
   page up; a plugin with an internal state opened, retargeted, and closed).

## Definition of done

- Host build exit 0 + `ctest --test-dir build-host` green; ASan green;
  `scripts/haiku_syntax_check.sh` 0 FAIL; VM `build` and `build-off` green;
  `ui_functional_tests` green with the new checks.
- Screenshot pass run in **both** modes (System/light and Dark) with every shot
  of every window looked at, at 150% too where geometry moved, and what was
  found and fixed named in the record.
- Every new behaviour has a test that was mutation-checked (break it, watch it
  fail, restore), named in the commit body.
- No `Co-Authored-By:` or other AI attribution in commits or PR text.
