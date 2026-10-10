# Record — M2 "Arrange window feel" (pointer feedback, navigation, tool palette)

Branch `feature/arrange-feel` off `feature/lv2-state`. Spec: `30-arrange-feel.md`.
Scope: `docs/PLAN_1.0.md` §M2 items **2.1, 2.2, 2.3**. Items 2.4–2.7 (range
selection, clip depth, track depth, markers) are the follow-up branch.

## What landed

**Model (kit-free, host-tested).**

- `src/model/SnapGrid.h` — `SnapKind` (Off/Bar/Half/Quarter/Eighth/Sixteenth/
  ThirtySecond) + a triplet flag, `SnapFrame(TempoMap&, Frame, SnapGrid)`,
  `SnapGridLabel`, `SnapDivisions`. Snapping stays in beat space through the
  tempo map (a ramp or a meter change moves the grid with the music); a bar is
  the governing meter's bar; triplets take 3 steps in the space of 2. The menu
  values, the indicator's labels and the mathematics are ONE definition, so a
  menu entry and the button cannot disagree — and the whole grid is host-tested
  instead of only reachable by opening a popup.
- `JoinClipsCommand` / `JoinMidiClipsCommand` — Glue's joins, the exact inverse
  of `SplitClipCommand`: the left clip grows to cover the right one's end, the
  right one is removed, the interior (seam) fade-out is cleared, and a MIDI
  join rebases the absorbed region's notes and events so glued music does not
  move. Refuses on a gap, on the last clip, and on take-group members.
- `SlipClipCommand` — `sourceOffset` moves, the clip does not. Audio only: a
  MIDI region has no source to slip.
- Tests `tests/snapgrid_tests.cpp` (65 checks) and
  `tests/arrange_cmd_tests.cpp` (64 checks), registered in `CMakeLists.txt`.

**TimelineView (Haiku-side).**

- The tool strip above the ruler (`ToolbarHeight()`), drawn with the piano
  roll's `DrawButton` and a new `icons::DrawArrangeTool` that REUSES the roll's
  shapes for the four shared tools (pointer, pencil, scissors, glue) and adds
  mute and fade. Six tools, keys **1–6**; the strip also carries the snap field
  and the roll's zoom pair. `ContentTop()` (a shared free function in
  `UiMetrics.h`) is now where the lanes start.
- Tools: **pointer** unchanged; **pencil** draws a MIDI region (drag = the
  drawn span, click = one bar); **scissors** splits at the snapped click;
  **glue** joins with the meeting neighbour (clicking either half works — a
  backwards join is tried when the forward one refuses); **mute** toggles the
  clip's TRACK mute (see the boundary note below); **fade** drags the nearer
  edge's fade anywhere in the clip.
- **Split at playhead on `S`**: every clip/region the playhead strictly
  crosses, the selection's when there is one, as ONE `MacroCommand`.
- **Grid menu + snap indicator**: the field is labelled with `SnapGridLabel`
  ("Bar" … "1/32T") and is lit only while snapping is on, so "Off" reads at a
  glance; the popup is the plan's list (bar, 1/2 … 1/32), the triplet modifier
  and Off, with the active entry marked. `Snapped()` uses it (Shift still means
  free placement) instead of the hard-coded 16th.
- **Pointer feedback**: one `HitTest` (zone + clip + lane) answers what a press
  would act on, and `MouseDown`, the hover highlight and `CursorFor` all read
  it. Cursors: move, trim (`RESIZE_EAST_WEST`), gain (`RESIZE_NORTH_SOUTH`),
  split (cross-hair), plus glyph cursors for fade/pencil/glue/mute/slip drawn
  from the same `DrawArrangeTool` their buttons use. Hover highlights: the
  grabbed clip edge or fade corner, the scissors' future cut line, the palette
  button and the track-header control (M/S/R/I, pan, gain). Tooltips on the
  palette, the grid field, the header controls, the ruler and the clip edges.
- **Navigation**: Ctrl+wheel zooms anchored on the pointer (the last hover
  position; a wheel message carries no coordinates — the live keyboard state is
  the modifier fallback for the same reason); `-`/`+` and the menu/`strip` zoom
  anchor on the playhead when it is visible, else the view centre; Shift+wheel
  and a horizontal wheel scroll sideways; plain wheel keeps scrolling tracks.
  Real `BScrollBar`s (`TimelineScrollBar`, a `ValueChanged` override — the view
  scrolls by frame offset, so the default "ScrollBy on a target" is the wrong
  mechanism) are children of the view, placed in `FrameResized` because a
  `B_SUPPORTS_LAYOUT` view does not reposition children by follow modes. The
  horizontal clamp is `ContentEndFrame()` (last clip/region, playhead, loop,
  punch, markers), so the whole arrangement scrolls past the end of the last
  clip.
- **The piano roll follows the playhead**: `PianoRollView::SetPlayhead` scrolls
  the region into view (10 % margin, the timeline's chase policy) for the
  docked and the popped-out editor; its Ctrl+wheel zoom anchors on the pointer
  too, and its keys/buttons on its own playhead.

## Boundaries (deliberate, and what the follow-up owns)

- **Mute is the track's mute.** Per-clip mute is `Clip.muted`, which is item
  2.5's model change (`audio clips gain name, colorIndex and muted`). The tool
  says "Mute track" in its tooltip and the record says it here, so 2.5 changes
  the tool's body rather than discovering it. `SetTrackMuteCommand` is the same
  command the header's M button issues (mute groups included).
- **Slip is audio-only** and IS implemented (Alt-drag): item 2.1 asks for a slip
  cursor, and a cursor for an edit that does not exist is worse than no cursor.
  Item 2.5's "slip edit" bullet is therefore done; what 2.5 still owns there is
  fade shapes, nudge and the clip depth model.
- **The overview minimap is NOT built.** With real scrollbars, Zoom-to-Fit and
  scroll-past-end the orientation job is done, and a minimap is a second
  full-project draw on every repaint while M1.5's 4 ms budget has still never
  been measured on the VM (Marc's T11 decision: measure first). Recorded here so
  the decision is visible rather than implied.
- Tool and snap selection are per-session state, not persisted in `AppSettings`
  (the plan does not ask for it; M3's Preferences can carry them later).

## Verification

(filled in below — see "Numbers" and "Screenshots")

## Numbers

| Check | Result |
|---|---|
| host `cmake --build build-host` | exit 0 |
| host `ctest --test-dir build-host` | 58/58 |
| ASan build + ctest | 58/58 |
| `sh scripts/haiku_syntax_check.sh` | 67 files, 0 FAIL |
| VM `build` ctest | see below |
| VM `build-off` ctest | see below |
| VM `ui_functional_tests` | see below |

## Mutation checks

Host (each was applied, watched to fail, reverted):

- `SnapGrid.h`: triplet multiplier 1.5 → 1.0 — 4 snap checks failed.
- `Commands.cpp` (`JoinableSuccessor`): the gap refusal bypassed — 4 failed.
- `Commands.cpp` (`JoinClipsCommand::Do`): the seam fade-out left in place — 1.
- `Commands.cpp` (`JoinMidiClipsCommand::Do`): the rebase delta zeroed — 2.

VM (each applied, watched to fail, reverted) — see below.

## Screenshots

See below.
