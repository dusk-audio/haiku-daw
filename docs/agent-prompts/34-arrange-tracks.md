# Task: M2 "Arrange window feel" — track depth (2.6) and markers (2.7)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/arrange-tracks` off `feature/arrange-depth` (the chain is master →
feature/theme-modes → feature/dock-browsers → feature/lv2-state →
feature/arrange-feel → feature/arrange-depth → yours; T1's two theme modes are
in it, so **every UI change is reviewed in both modes**). Same rules as every
package: spec first, one verification bar per branch, a record
(`34-arrange-tracks-PR.md`) kept as you go, no `Co-Authored-By:` trailer.

This is the last slice of `docs/PLAN_1.0.md` §M2: items **2.6 (track depth) and
2.7 (markers)**. Items 2.1–2.5 shipped on the two branches below yours; this
package extends their files (`TimelineView`, `MainWindow`, `Commands`,
`ProjectIO`, `ui_functional_tests`) and must not regress them.

## Codebase orientation (read before coding)

- Layering. `src/model/` is kit-free and builds + tests on the Linux host
  (`ctest --test-dir build-host`); `src/ui/` and `src/engine/` are Haiku-only and
  are verified on the VM and by `sh scripts/haiku_syntax_check.sh`. Logic that
  can live in the model belongs there — it is the only place a behaviour can be
  host-tested and mutation-checked cheaply.
- `TimelineView` (`src/ui/TimelineView.{h,cpp}`) owns the arrangement: the tool
  strip, the ruler, the lanes, the track headers, every gesture. Geometry is
  `FrameToX`/`XToFrame` over `fScrollFrame` + `fFramesPerPixel`; `LaneRect(i)`
  walks the per-track heights (`LaneHeightOf`, clamped at `TrackHeight()`);
  `TrackIndexAt` is the inverse and returns -1 in a lane gap;
  `HeaderControlAt(idx, where)` is the ONE header hit-test the click path, the
  hover highlight and the drawing share. **Drags preview by writing the model,
  then `MouseUp` restores the pre-drag value and pushes ONE `CommandStack`
  command** — absolute (`24-…-ENTRY.md` §7). `MouseDown` takes focus.
- The header today (150×74 design px): name text, M/S/R/I boxes (y+20..38), a
  pan knob (100..128 × y+15..43), a gain fader (6..128 × y+50..62), a per-track
  meter on the right edge (134..148, `HdrMeterW`). `Track.colorIndex` exists and
  tints clips/regions; the header's 4 px left strip is by track TYPE. The
  palette lives in one place: `TrackColor(i)` / `kTrackColorCount`
  (`src/ui/Theme.h`).
- The track name's double-click currently opens `RenameWindow`; that window (and
  its `kMsgRenameTrack`/`kMsgRenameMarker` posts) stays as the menu path.
- The INSPECTOR (`src/ui/InspectorView.cpp`) already has a full input picker
  (`fInputR`): a `BPopUpMenu` of None + `EnumerateMidiEndpoints()` (producers
  only, minus our own) for a MIDI track, None + "Default Input" for an audio
  track, committed as `SetTrackInputCommand`. That list-building code is the
  one definition the header selector must reuse.
- Markers already exist end to end: `Track`-less `Project::markers` (sorted),
  `AddMarkerCommand`/`RemoveMarkerCommand`/`RenameMarkerCommand`, the
  `marker <frame> "<name>"` IO line, `m` = drop a marker at the playhead,
  Cmd-←/→ = prev/next, `L` = loop between the bracketing pair, flags drawn on
  the ruler, click a flag to jump, right-click the ruler for
  Add/Rename/Delete. What 2.7 adds is a **list window** and a **marker lane**.
- Child windows (`MixerWindow`) run on their own looper with NO model access:
  the main window pushes snapshots (`kMsgMixStrips`) and the window posts
  intents (`kMsgApplyMix`, …). The markers window is the second of that shape.
  A `BPopUpMenu::Go` is SYNCHRONOUS (deliversMessage=false, openAnyway=true,
  async=false), so a functional test must never click something that opens one:
  the established pattern is a public method the menu item also calls
  (`ToggleClipMute`, `SetSnapGrid`, …), plus a `Shot()` of the drawn control.
- `MainWindow` owns the menu bar (File/Edit/Track/View/Audio/Help), the
  `fMixerMsgr` single-instance plumbing and `LoadSettings`/`SaveSettings`
  (`AppSettings`, kit-free + host-tested, persists the panes).
- Tests: `tests/ui_functional_tests.cpp` — free `TestXxx` functions called in
  order from `TestThread`, `CHECK(cond)`, `Shot("name")` under `DAW_UI_SHOTS`,
  synthetic mouse messages carrying **both** `where` and `screen_where`, window
  closes as `Lock(); Quit();` with nothing after (a `Unlock()` after `Quit()`
  trips "looper must be locked" — a crash dialog, not a failed check). Kit-free
  behaviour goes in the host suites (`tests/arrange_cmd_tests.cpp`,
  `tests/projectio_tests.cpp` are the neighbours to match).

## Goal

Track depth: a lane whose header does the things a commercial one does — rename
where the name is, resize by dragging its edge, pick a colour, duplicate the
whole track, Alt-solo it exclusively, choose its input, and fold it away.
Markers: a list window that shows every marker and jumps the transport to one,
and a marker lane in the arrangement that can be toggled, drawn, clicked and
edited in place.

## Work items

1. **Model** (`src/model/Project.h`): `Track::collapsed` (false by default) —
   the lane folds to a title strip. IO: a new optional line
   `trackfold <trackId> 1`, written only for a collapsed track and directly
   after its `track` line; **append-only** (a project that never collapses a
   track writes the legacy bytes exactly), parsed by id so an older/newer file
   is unaffected; compat tests both directions.
2. **Commands** (`src/model/Commands.{h,cpp}`, host-tested):
   - `SetTrackCollapsedCommand(track, collapsed)` — stores the old flag.
   - `DuplicateTrackCommand(track)` — copies the WHOLE track (name + " copy",
     gain/pan/flags, colour, height, fx, sends, automation, instrument, input,
     routing) with **fresh clip/region ids** (including the freeze stash), right
     after the source. `Do()` reuses the first allocated id across redo (the
     `AddTrackCommand` pattern) and `CreatedId()`/`NewIndex()` let the caller
     select it. Undo removes exactly it.
   - Exclusive solo is a `MacroCommand` of existing `SetTrackSoloCommand`s
     ("Exclusive Solo"): solo the clicked track, unsolo every other one; if the
     clicked track is already the only soloed one, clear it (the Alt-click
     toggle).
3. **Header layout + hit-test** (in `TimelineView`). One enum-classed
   hit-test — `HeaderControl { Mute, Solo, Arm, Monitor, Pan, Gain, Colour,
   Input, Fold }` with `HeaderControlAt(int lane, BPoint)` **public** (the
   drawing, the click path, the hover and the functional test all read it) —
   replaces the file-static `kHdr*` ints. Layout per lane:
   - name row: fold triangle (5..15), name at x 18, colour swatch 116..132 ×
     y+2..16;
   - M/S/R/I unchanged; pan knob unchanged;
   - **input selector** 6..96 × y+44..58, showing `In: …` (none / Default /
     the endpoint's tail, truncated) and lit when a source is chosen;
   - gain fader moved to 6..128 × y+60..72.
   A collapsed lane is `kDesignCollapsedH = 22`: fold triangle, colour swatch,
   name (truncated), M, S and the meter; no clips, no waveform, no
   pan/fader/input.
4. **Input choices, one definition** (`src/ui/TrackInputs.h`, header-only,
   Haiku-side): `TrackInputChoices(const Track&)` → the ordered
   `std::vector<InputSource>` (None first, then MIDI endpoints / Default
   Input) and `InputSourceLabel(const InputSource&)`; `InspectorView` is
   refactored to use them, so the inspector and the header cannot drift.
5. **Colour swatch + picker.** The swatch draws `TrackColor(t.colorIndex)`
   (the one palette). Clicking it opens a popup of `kTrackColorCount` entries
   whose labels come from `TrackColourName(i)` (new, beside `TrackColor`), the
   active one marked; the choice runs `SetTrackColour(track, i)`
   (`SetTrackColorCommand`) — the method the test calls. Right-click the swatch
   is the same menu.
6. **Rename in place.** Double-clicking the name (not the whole header row)
   starts an in-place edit: the name field goes LCD, the track's name becomes an
   edit buffer with a caret, printable keys/Backspace/arrows edit it within
   `TimelineView::KeyDown`, Enter commits ONE `SetTrackNameCommand`, Esc
   cancels, and any press outside the field commits first. The context menu's
   "Rename…" still opens `RenameWindow`, so nothing is lost. `SetProject` and a
   vanished track cancel the edit (stale content is a bug).
7. **Lane-edge resize.** `LaneEdgeAt(where)` (public, in the header column or
   lane content) names the lane whose bottom edge is within `Themed(3)` px;
   pressing there starts `Drag::LaneResize`, previewing `Track.height` clamped
   to `[Themed(72), Themed(300)]`, and `MouseUp` restores and pushes ONE
   `SetTrackHeightCommand`. Cursor `B_CURSOR_ID_RESIZE_NORTH_SOUTH`
   (`Pointer::LaneSize`), tooltip "Drag to resize the lane".
8. **Collapse + duplicate + exclusive solo in the UI.** The fold triangle
   toggles `SetTrackCollapsedCommand`; the header's context menu gains
   "Duplicate Track" and "Collapse"/"Expand" (label from state) beside the
   existing entries, and the **Track menu** gains "Duplicate Track" (disabled
   with no track selected) which runs the same
   `TimelineView::DuplicateSelectedTrack()`. Alt-clicking the header's **S**
   button runs the exclusive-solo macro. A duplicated track is selected (and
   scrolled to) so the user sees what happened.
9. **Markers list window** (`src/ui/MarkersWindow.{h,cpp}`, new). Its own
   looper, no model access: a snapshot `std::vector<MarkerInfo{frame,name}>`
   pushed by the main window (`kMsgMarkerList`), and intents posted back —
   `kMsgMarkerJump` (int64 "frame"), `kMsgMarkerAdd` (at the playhead),
   `kMsgMarkerDelete` (frame + name), plus `kMsgMarkerActivate` for
   single-instance. Custom-drawn rows (flag, name, bar:beat time), a click seeks
   (`kMsgMarkerJump`) and selects, a double-click opens `RenameWindow`
   (`kMsgRenameMarker`), buttons "Add at Playhead" / "Rename…" / "Delete", an
   empty state saying how to fill it, themed via the tokens. The MainWindow
   gains `MSG_MARKERS` (View menu "Markers", Cmd-M) with the single-instance
   pattern, and pushes a fresh snapshot on open and on every `kMsgUiRefresh`.
10. **Marker lane** (in `TimelineView`). A `Themed(kDesignMarkerLaneH = 20)`
    strip between the ruler and the lanes, off by default, toggled by
    `SetMarkerLane(bool)` from **View ▸ Marker Lane** (`MSG_MARKER_LANE`) and
    **Shift+M**; `ContentTop()` includes it when on (the ruler keeps its own
    band: `RulerBottom()` is what the ruler hit-tests use). It draws every
    marker as a flag + name (truncated) + the playhead crossing it.
    Interactions: left-click a flag seeks (and `SetPlayhead` now moves the line
    — the ruler's flag click was missing it), double-click empty space adds a
    marker at the snapped frame, right-click a flag removes it (one undoable
    command; no popup, so it stays drivable), right-click empty space adds.
    Markers are added/removed through the ONE pair of methods
    (`AddMarkerAt(Frame)`, `RemoveMarkerAt(frame, name)`) the ruler menu, the
    `m` key and the markers window all use.
11. **Every marker edit tells the window.** `AddMarkerAt`/`RemoveMarkerAt`/
    rename post `kMsgUiRefresh`, and `MainWindow`'s `kMsgUiRefresh` handler
    pushes `kMsgMarkerList` to an open markers window — otherwise the window is
    stale content, which the guidelines call a bug.
12. **Preference**: `AppSettings::markerLaneVisible` (default false) saved and
    restored with the other view state, host round-trip tested.

## Definition of done

- Host: `cmake --build build-host` exit 0, `ctest --test-dir build-host` green
  (baseline on this branch: 59/59), ASan build + ctest green, and
  `sh scripts/haiku_syntax_check.sh` 0 FAIL.
- VM: `build` and `build-off` ctest green (baseline 60/60 and 55/55 + this
  package's checks).
- New host tests (`tests/track_depth_tests.cpp`): `DuplicateTrackCommand`
  (clips/regions get fresh ids, content carried, lands after the source, exact
  Undo, redo keeps the ids, the freeze stash is copied with fresh ids), and
  `SetTrackCollapsedCommand` (set/undo/no-op). `projectio_tests`: the
  `trackfold` round-trip, an old file loading with `collapsed=false`, and a
  non-collapsed save staying byte-identical to the legacy spelling.
  `appsettings_tests`: `markerLaneVisible` round-trip.
- New `ui_functional_tests` checks, each **mutation-checked** (break it, watch
  it fail, restore) and the mutation named in the commit body, each driving the
  real widget: the in-place rename (double-click + keystrokes + Enter, one undo
  step; Esc leaves the name), the lane-edge drag (synthetic mouse on the edge,
  height grows, one `Set Track Height` undo), the colour swatch
  (`HeaderControlAt` classifies the swatch's own rect, then the method the menu
  item calls), the input selector (same shape, `kMsgMonitorRefresh` path), the
  fold triangle (lane height collapses to the title strip and restores), Alt-S
  exclusive solo, Duplicate Track from the Track menu, the marker lane
  (toggle, click-to-seek, add by double-click, remove by right-click), and the
  markers window (View ▸ Markers opens ONE window; its own buttons add / jump /
  delete through the real widget).
- Screenshots: the `DAW_UI_SHOTS` pass run twice (`DAW_UI_THEME=light` and
  `DAW_UI_THEME=dark`, screen unlocked), every shot showing the new header, the
  swatch's picker, the in-place rename, a collapsed lane, the marker lane and
  the markers window opened at full size, plus the 150 % shots where geometry
  moved (the header rows DID move); what was looked at and what was fixed named
  in the record.
- The record names what could not be verified and any decision Marc owes.

## Boundaries recorded (so a later package does not re-litigate them)

- "Collapse or hide": **collapse** (fold to a title strip) is built; there is no
  hidden state, because a hidden track would need a "show hidden" affordance
  the plan does not ask for and a second selection model with it.
- A popup cannot be opened by the test suite (synchronous `Go`), so the
  pickers' *choices* are driven by the method the menu item calls and their
  *look* by the shots — the same boundary 30/33 recorded.
- Drag-to-move a marker is NOT built (the plan asks for click-to-seek and
  add/remove); Range/Insert Silence still do not move markers (33's decision).
