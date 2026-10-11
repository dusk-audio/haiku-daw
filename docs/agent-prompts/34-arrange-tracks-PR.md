# Record — M2 "Arrange window feel" (track depth 2.6, markers 2.7)

Branch `feature/arrange-tracks` off `feature/arrange-depth`
(PR: https://github.com/dusk-audio/haiku-daw/pull/12). Spec:
`34-arrange-tracks.md`. Scope: `docs/PLAN_1.0.md` §M2 items **2.6** and **2.7** —
the last two arrange-window items.

## What landed

**Model (kit-free, host-tested).**

- `Track.collapsed` — the lane folds to its title strip. Persisted on its OWN
  optional line `trackfold <trackId> <0|1>`, applied **by id after the whole
  file is read**, so the line's position relative to its track record cannot
  decide whether it survives. A project that folds nothing writes the legacy
  bytes exactly (compat tests both directions).
- `DuplicateTrackCommand` — copies a whole track (name + " copy", gain/pan/
  flags, colour, height, fold, fx, sends, automation, instrument, input,
  routing, and the freeze stash) with **fresh clip/region ids** (an id shared
  with the original would make every id-addressed edit hit two blocks), placed
  directly after the source. The copy is built ONCE and reused on redo, so its
  ids stay stable across undo/redo (a redo that re-allocated them would leave
  every id-addressed selection pointing at nothing).
- `SetTrackCollapsedCommand`.
- Exclusive solo is a `MacroCommand` of the existing `SetTrackSoloCommand`s
  ("Exclusive Solo"): solo the clicked track, unsolo every other; Alt-clicking
  the track that is already the only soloed one clears it (the toggle every
  solo button has).

**Arrangement view (Haiku-side).**

- **Header depth**: a fold triangle, a colour swatch (the popup is the ONE
  palette: `TrackColor`/`TrackColourName`), a record-input field showing the
  chosen source, and a name field that **renames in place** — double-click the
  name, type, Enter commits ONE `SetTrackNameCommand`, Esc cancels, a press
  outside commits first, and the open field owns the keyboard (a bare `,` or
  `1` must not nudge a clip or switch tools behind a text field).
- **Lane-edge resize**: the bottom edge in the header gutter drags the height
  (preview writes the model, mouse-up pushes ONE `SetTrackHeightCommand`),
  cursor `B_CURSOR_ID_RESIZE_NORTH_SOUTH`, tooltip included.
- **One hit test**: `HeaderControlAt` + `HeaderRectFor` over an enum class —
  the drawing, the click path, the hover, the cursor AND the functional test
  read the same answer, so a control cannot be drawn somewhere and clicked
  somewhere else.
- **Fold**: the triangle toggles it; a collapsed lane draws a compact row
  (triangle, swatch, name, M, S, meter) and nothing else, and the view scrolls
  so the lane the user just folded/expanded stays visible.
- **Duplicate** from the Track menu (disabled with no track selected) and the
  header's context menu; the copy is selected and scrolled into view.
- **Alt-S** on a header solo button runs the exclusive macro.
- **Input choices in one place**: `src/ui/TrackInputs.h` now owns the list
  (None + MIDI producers / the system default) and the labels; the inspector
  was refactored onto it, so the header field and the inspector cannot drift.
- **Marker lane** (toggleable, off by default): a `Themed(20)` strip between
  the ruler and the lanes, drawn with flags + names; left-click a flag seeks,
  double-click empty space adds at the snapped frame, right-click a flag
  removes it — all one undoable command, no popup, so the whole gesture is
  drivable by a synthetic click. It is part of `ContentTop()` while it is on
  (`RulerBottom()` is what the ruler's own hit tests use).
- **Seeking**: `SeekTo` moves the model's playhead AND the drawn line — the
  ruler's marker click used to move only the model, so a stopped transport kept
  showing the old position.
- **`AddMarkerAt` / `RemoveMarkerAt`** are the one pair of methods the `m` key,
  the ruler's menu, the lane and the markers window use; each posts
  `kMsgUiRefresh`, which is the funnel that keeps an open markers window from
  going stale.

**Markers window** (`src/ui/MarkersWindow.{h,cpp}`, new). Its own looper, no
model access: a snapshot (`frame`, `name`, pre-formatted `time`) pushed by the
main window (`kMsgMarkerList`) and intents posted back — `kMsgMarkerJump`,
`kMsgMarkerAdd`, `kMsgMarkerDelete`, `kMsgMarkerActivate`. Rows seek on click
and rename on double-click (through `RenameWindow`, which posts
`kMsgRenameMarker`), buttons "Add at Playhead" / "Rename…" / "Delete", an empty
state that says how to fill it, both themes. View ▸ Markers (Cmd-M),
single-instance, and asking again brings it to the front.

**AppSettings**: `markerLaneVisible` (default false), saved with the panes,
host round-trip tested.

## Decisions (recorded, not discovered)

- **"Collapse or hide"**: collapse is built; there is no hidden state (a hidden
  track would need a "show hidden" affordance the plan does not ask for and a
  second selection model with it).
- **The pickers' choices are driven by the method the menu item calls** (the
  established boundary: `BPopUpMenu::Go` is synchronous, so a synthetic click
  on a swatch would wedge the window thread); the *control's own rect* is
  asserted through `HeaderControlAt`, so the press that would open the menu is
  proven to land on the control, and the menu itself is what the shots show.
- **Right-click on a marker-lane flag removes it immediately** (one undoable
  step, tooltip says so) instead of opening the ruler's popup.
- **Fold persists** (it is project state, `trackfold`), while the marker lane's
  visibility is a per-machine preference (`AppSettings`).
- A lane's height drag clamps to `[Themed(72), Themed(300)]`, the same range the
  context menu's Taller/Shorter uses.

## Findings that are NOT this package's (for Marc)

- **The markers window's constructor argument order was wrong in the first cut**
  (`BWindow(frame, title, B_TITLED_WINDOW, B_FLOATING_APP_WINDOW_FEEL, flags)`
  binds to the `(…, window_type, uint32 flags, uint32 workspace)` overload, so
  the FEEL lands in the flags slot and the app_server clamps it to NORMAL —
  `src/servers/app/Window.cpp` does exactly that for an invalid look/feel).
  A screenshot caught it: the window existed, was not hidden, and sat behind
  the main window. `MarkersWindow` now passes `(…, B_TITLED_WINDOW_LOOK,
  B_FLOATING_APP_WINDOW_FEEL, flags)` and activates on open.
  **Six older windows still use the shifted form** — `RenameWindow`,
  `SendsWindow`, `ExportWindow`, `QuantizeWindow`, `InstrumentWindow`,
  `ExportProgressWindow` — so they are NORMAL-feel windows that can open behind
  the main window instead of floating above it. Not touched here (other
  packages' files); a one-line change each, worth a decision.
- **A message sent to a window is not delivered to its child views.** The
  markers window first had no `kMsgMarkerList` handler, so the pushed list went
  nowhere — and the test's own size check passed anyway, because the window is
  built WITH a snapshot. The check now adds a marker afterwards and asserts the
  open window's list grows (see the mutations), which is what makes it a test.

## Verification

### Host

| Check | Result |
|---|---|
| `cmake --build build-host` | exit 0 |
| `ctest --test-dir build-host` | 60/60 (baseline 59/59 + `track_depth_tests`) |
| ASan build + ctest | 60/60 |
| `sh scripts/haiku_syntax_check.sh` | 69 files, 0 FAIL |

New host tests: `tests/track_depth_tests.cpp` (60 checks: the fold command's
undo/redo and a vanished track, `DuplicateTrackCommand` — the whole track
carried, every clip/region/freeze-stash id fresh, the copy right after the
source, exact Undo, redo keeping the SAME ids, a second duplicate, a vanished
track refusing — and a save/load round trip). `projectio_tests` +14 (the
`trackfold` round trip, a save that writes it only for a folded track and keeps
the track line byte-identical, and an old file loading expanded). `appsettings_
tests` +6 (`markerLaneVisible`).

### Host mutation checks (applied, watched to fail, reverted)

Trivial host mutations were checked while the code was written (the assert
pattern is one line); the interesting ones are the VM list below.

### VM mutation checks (applied, synced, built and run on the VM, reverted)

Each row is one build+run of the whole suite (`mut.sh` applies the edit to the
worktree, syncs it as a throwaway ref, builds and runs).

| mutation | failures |
|---|---|
| `rename` — the in-place commit does not execute `SetTrackNameCommand` | 2: the name check and `undoName() == "Rename Track"` |
| `fold` — the fold triangle's handler does nothing | 4: collapsed state, the undo label, the folded lane height, the unfold |
| `laneedge` — `LaneEdgeAt` returns -1 | 3: the edge classification and both height checks |
| `exclusive` — Alt-S falls through to the plain solo toggle | 2: A staying soloed, and the undo label |
| `markerseek` — `SeekTo` leaves out `SetPlayhead` | 1: the drawn-line check (model and line must both move) |
| `pushmarkers` — `PushMarkers` returns early | 1: the open window's list does not grow after a marker is added elsewhere |
| `duplicate` — `DuplicateTrack` returns early | 7: the count, the label, the fresh id, the name, the clips, the selection — plus one downstream cascade |

The `pushmarkers` row was re-run after the child-view forwarding fix: with the
first version of the check it failed for the wrong reason (the window never
handled the message, so the check failed on a CORRECT build too — that is how
the missing handler was found).

### VM

| Check | Result |
|---|---|
| VM `build` — `ctest -E ui_functional_tests` | 61/61 (baseline 60/60 + `track_depth_tests`) |
| VM `build-off` — `ctest -E ui_functional_tests` | 56/56 (baseline 55/55 + 1) |
| VM `ui_functional_tests`, `DAW_UI_THEME=light` (with shots) | **491 checks, 0 failures** |
| VM `ui_functional_tests`, `DAW_UI_THEME=dark` (with shots) | **491 checks, 0 failures** |

One earlier run of `TestBigProjectPlayback` failed its 5 s play-start smoke
bound at 5.12 s while the host's load average was 7–9 (its own printout: "45
tracks, 320 clips, play in 5090 ms" against a 2.9 s measurement last week); the
next run on the same revision was green. The test's bound is consumed by M4.1's
work either way, and this test now removes its fixtures so it does not feed the
next test's measurement.

**A sibling agent shares this VM**, and two of its documented scratch refs
(`_vmwt`, `/tmp/ui.log`, `~/haiku-daw`, the `build` dir) are shared state:
- the first two UI runs reported 329 checks and 0 failures — they had built and
  run the SIBLING's tree, because a sync in an earlier lock hold was stolen by
  the sibling's next sync (this package now uses its own ref `_vmwt_at` and
  copies the log to `/tmp/at-ui.log` inside the same hold);
- a mutation run's results were read from `/tmp/ui.log` after the sibling had
  overwritten it. Every number above comes from a run whose sync, build and
  test happened in ONE lock hold, and whose log was copied to a private name.

## Screenshots

The `DAW_UI_SHOTS` pass ran in **both** modes on the final revision
(`DAW_UI_THEME=light` and `DAW_UI_THEME=dark`; 58 shots each, both 491/0). This
package's shots are committed in `docs/agent-prompts/shots-34/<mode>/` (11 per
mode). Shots I opened at full size, cropping and zooming on the header rows, the
field, the flags and the list:

- `29-tracks-rename-field` (both) — the open name field: LCD well, accent
  border, the typed text and the caret (dark mode too: white on dark, readable).
- `30-tracks-rename` (both) — the committed name on the lane.
- `31-tracks-lane-height` (both) — the lane after the edge drag.
- `32-tracks-colour` (both) — the swatch in the header and the clip tinted by
  `colorIndex` (orange).
- `33-tracks-input` (both) — the `In: …` field after `SetTrackInput`.
- `34-tracks-collapsed` (both) — the folded lane as a title strip at the bottom
  of the view (this is the shot that caught the lane being cut off under the
  horizontal scrollbar; the clamp fix landed after it).
- `35-tracks-exclusive-solo`, `36-tracks-duplicated` (both) — the state the
  checks assert (model-level; the drawing is the pre-existing button/fader
  furniture).
- `37-markers-lane` (both) — the labelled marker strip with a flag and its name,
  the ruler's own flag above it and the playhead crossing both.
- `38-markers-window`, `39-markers-window-after` (both) — the list (columns,
  flags, bar:beat + min:sec), the sized buttons and the empty space below the
  rows; dark mode reads as one panel with the rest of the app.

**What I fixed from the pass:**
1. the markers window opened BEHIND the main window (the constructor's
   argument-order finding above) — visible in the first shot as "no window";
2. its three buttons stretched to slab size (a group layout stretches a button
   unless it is given an explicit size — UI_GUIDELINES §4) — the first shot of
   the window showed three tall slabs;
3. the folded lane was cut off under the horizontal scrollbar: `ScrollToY`'s
   and `SyncScrollBars`' vertical `viewH` ignored `fBarThickness`, so "scrolled
   to the bottom" left the last lane's bottom row under the bar; both now take
   the bar's strip out of the lane area, and collapsing a lane scrolls the lane
   into view;
4. `TestArrangeDepth`'s mid-drag check pressed the header fader by a HARD-CODED
   offset (`Themed(56)`), which after this package's header rework landed on
   the new input field — whose picker is a synchronous popup, so the click
   blocked the window thread and every later check timed out (a wedged run: 418
   checks, 8 failures, all after that click). The check now asks the view for
   the fader's own rect (`HeaderRectFor`), so geometry can move without the
   test pressing the wrong control.

**150 %:** the header rows all moved, so the 150 % case matters; the existing
`14-theme-150` shot was opened in BOTH modes (cropped on the header) and shows
the reworked header at 1.5×: the fold triangle, swatch, name, M/S/R/I, pan
knob, `In: —` field and fader all scale with the font, nothing overlaps, and
the clip names truncate (`haiku_daw…`). Dark mode reads the same.

## What could not be verified, and questions for Marc

- **The popup menus (colour, input) cannot be photographed on this VM** — an
  open menu needs a real pointer held down. Their *choices* run through the
  methods the menu items call (asserted), and their controls' hit rects are
  asserted through `HeaderControlAt`; the menu itself is unreviewed.
- **A real mouse's gestures**: the drag's feel (the 3 px edge band, the 4 px
  range threshold) and the tooltips' timers are desktop checks.
- **The six windows with the shifted constructor** (see Findings): I verified
  the mechanism and the effect on MY window, not each of the six. If Marc wants
  them fixed, it is a one-line change each plus a shot of each dialog.
- **Two shots in each pass report `shot failed:`** — the `screenshot` tool exits
  non-zero even when the file lands (the known noise, `docs/UI_GUIDELINES.md`
  §1); all 58 files are present in both modes.
- **TestBigProjectPlayback's 5 s bound** is host-load-sensitive (see VM above);
  it is the pre-existing smoke bound, not this package's.
