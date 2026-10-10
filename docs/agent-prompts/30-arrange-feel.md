# Task: M2 "Arrange window feel" — pointer feedback, navigation, tool palette

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/arrange-feel` off `feature/lv2-state` (the chain is master →
feature/theme-modes → feature/dock-browsers → feature/lv2-state → yours; T1's two
theme modes are in it, so **every UI change is reviewed in both modes**). Same
rules as every package: spec first, one verification bar per branch, a record
(`30-arrange-feel-PR.md`) kept as you go, no `Co-Authored-By:` trailer.

This is the first slice of `docs/PLAN_1.0.md` §M2: items **2.1 (pointer
feedback), 2.2 (navigation) and 2.3 (the arrange tool palette)**. Items 2.4–2.7
(range selection, clip depth, slip/fade shapes/nudge, track depth, markers) are a
follow-up branch and are **not** in scope; where one of them owns something this
slice touches, the boundary is stated below and repeated in the record.

## Codebase orientation (read before coding)

- Layering. `src/model/` is kit-free and builds + tests on the Linux host
  (`ctest --test-dir build-host`); `src/ui/` and `src/engine/` are Haiku-only and
  are verified on the VM and by `sh scripts/haiku_syntax_check.sh`. Logic that
  can live in the model layer belongs there: it is the only place a behaviour can
  be host-tested and mutation-checked cheaply.
- `TimelineView` (`src/ui/TimelineView.{h,cpp}`) is the whole arrangement view,
  custom-drawn with no child `BControl`s except the two scrollbars this package
  adds. Geometry: `FrameToX`/`XToFrame` over `fScrollFrame` + `fFramesPerPixel`
  (zoom), `fScrollY` for tracks, `LaneRect`/`TrackIndexAt` walking per-track
  heights, `HeaderWidth()` as the left gutter. `DrawLanes` culls against the
  update rect; `DrawRuler` is drawn last so it sits above lane content.
  `MouseDown`/`MouseMoved`/`MouseUp` implement every gesture; **drags preview by
  writing the model, then `MouseUp` restores the pre-drag value and pushes ONE
  `CommandStack` command** — that discipline is absolute (`24-…-ENTRY.md` §7).
- `Snapped()` (`TimelineView.cpp:312`) currently hard-codes a 16th-note division
  (`kSnapDivision = 4`) and snaps in beat space through `TempoMap::BeatAt` /
  `FrameAt` so a variable tempo map is honoured. `GridOf()` returns a `Grid`
  (`src/model/Grid.h`) but `Snapped()` does not use it. `Grid` is
  frames-per-beat math on a *fixed* tempo; `TempoMap` is the tempo/ramp/meter
  map of record and is what the ruler and `BarStartFrameAt` already use.
- The tool-palette pattern to match is `PianoRollView` (`src/ui/PianoRoll.{h,cpp}`):
  an icon-only strip at the top of the view (`kToolbarH = 28`),
  `BRect ToolRect(int)` / `int ToolAt(BPoint)`, `DrawButton(this, rect, "",
  active, ColAccent())` for the button and `icons::DrawTool` for the glyph
  (`src/ui/widgets/DawIcons.h`), keys `1`–`7` in `KeyDown`, and a hover-driven
  tooltip (`fHoverTool` + `SetToolTip`/`ShowToolTip` in `MouseMoved`, cpp:888).
  `MouseDown` dispatches per tool (`cpp:746-846`).
- Key routing: `MainWindow::DispatchMessage` (`MainWindow.cpp:624-648`) hands a
  `B_KEY_DOWN` with no Command modifier straight to
  `fTimeline->MessageReceived` when nothing (or the timeline itself) holds focus,
  so `TimelineView::KeyDown` is the place for new bare keys. Cmd-keys go to the
  menu bar.
- Wheel: `TimelineView::MessageReceived` (`cpp:181-187`) reads only
  `be:wheel_delta_y` and scrolls vertically. `PianoRollView` (`cpp:1010-1026`)
  already does the Ctrl = zoom / Shift = scroll-time mapping. A view receives
  `MouseMoved` with transit events while the pointer is over it, so the last
  hover position is trackable state; a wheel message itself carries only deltas
  (`MouseInputDevice.cpp:490`).
- `BScrollBar` (Haiku source: `src/kits/interface/ScrollBar.cpp`) has a **virtual
  `ValueChanged(float)`** and, with **no target view set**, does nothing but
  update itself — so a subclass can drive a view that scrolls by frame offset
  instead of by bounds. Note `BView::_ResizeBy` (`View.cpp:6468`): a view with
  `B_SUPPORTS_LAYOUT` (the timeline) does **not** reposition children by their
  follow modes, so child scrollbars must be placed explicitly in `FrameResized`.
- The piano roll playhead is pushed from one place —
  `MainWindow::PushRollPlayhead` (`MainWindow.cpp:3307`), reached from the
  transport pulse (`:1874`, `:1903`) — into the docked `fDockRoll` directly and
  into the popped-out window through the single `fRollMsgr` (`PianoRoll` forwards
  `kMsgRollPlayhead` to its view, `PianoRoll.cpp:1092`).
- Tests: `tests/ui_functional_tests.cpp` — free `TestXxx` functions called in
  order from `TestThread`, a `CHECK(cond)` macro, `Shot("name")` under
  `DAW_UI_SHOTS`, synthetic mouse events as posted
  `B_MOUSE_DOWN`/`B_MOUSE_MOVED`/`B_MOUSE_UP` messages carrying **both** `where`
  and `screen_where` (the view's `MouseDown` point comes from `screen_where`), and
  `win->FindView("timeline")` to reach the view. `TestThemeScale` currently
  clicks the ruler at `kDesignRulerHeight + 7` — geometry this package moves.
  Kit-free behaviour goes in the host suites instead.

## Goal

The arrange window feels like the commercial DAWs the plan names: the pointer
says what it is about to do and the window highlights the thing it would do it
to; the time axis navigates the way a DAW's does (wheel + scrollbars, zoom under
the pointer, real scroll past the end); and there is a tool palette and a
snapping grid that can be seen and switched, both matching the piano roll's.

## Work items

1. **A kit-free snap grid** (`src/model/SnapGrid.h`, header-only, host-tested).
   `SnapKind { Off, Bar, Half, Quarter, Eighth, Sixteenth, ThirtySecond }` +
   `bool triplet`, `SnapFrame(const TempoMap&, Frame, SnapGrid)` and
   `SnapGridLabel(...)` ("Bar", "1/2", "1/16", "1/8T", "Off") so the menu, the
   indicator and the snapping mathematics cannot disagree. Snapping stays in beat
   space through the tempo map (the existing behaviour); `Off` returns the frame
   unchanged; triplets use 3 steps in the space of 2. `TimelineView::Snapped`
   drops `kSnapDivision` and calls it (Shift still means "no snap").
2. **Join + slip commands** (`src/model/Commands.{h,cpp}`, host-tested).
   `JoinClipsCommand(track, left)` and `JoinMidiClipsCommand(track, left)`: the
   clicked clip absorbs the next one on the same track when the next starts at or
   before the left one's end (the exact geometry `SplitClipCommand` leaves
   behind); the right clip is removed and both halves are restored on Undo.
   `SlipClipCommand(track, clip, newSourceOffset)` moves the audio inside the
   clip without touching its position or length (Undo restores the offset). Both
   are ordinary commands: no new `ProjectIO` lines, no model fields.
3. **The arrange tool palette** (in `TimelineView`). A strip across the top of
   the view, `ToolbarHeight()` tall, above the ruler — the same shape and
   drawing as the piano roll's strip: `DrawButton` + a new
   `icons::DrawArrangeTool` that reuses the piano roll's shapes for the four
   shared tools and adds mute and fade glyphs. Six tools, keys **1–6**:
   - **1 Pointer** — today's behaviour, unchanged (select, band-select, move,
     trim, fade corners, Ctrl gain);
   - **2 Pencil** — drag on empty lane content of a **MIDI** track creates a
     region from press to release, snapped, one `AddMidiClipCommand`; an audio
     track has no source-less clip to draw, so the tool does nothing there (the
     tooltip says so);
   - **3 Scissors** — click a clip/region splits it at the (snapped) click;
   - **4 Glue** — click a clip/region joins it with the next one when they meet;
   - **5 Mute** — click toggles the mute of the clip's **track**
     (`SetTrackMuteCommand`). Per-clip mute is `Clip.muted`, which is M2.5's
     model change; the tool records this boundary in its tooltip ("Mute track")
     and the record;
   - **6 Fade** — press and drag inside a clip sets the fade on the nearer edge
     from the distance to that edge (snapped), previewed live and committed as
     one `SetClipFadeCommand`/`SetMidiClipFadeCommand` on release.
   A **Fade** tool selection makes the fade grips obvious; a **Scissors** hover
   draws the future split line on the clip under the cursor.
4. **Split at playhead, `S`.** `SplitAtPlayhead()` splits every clip/region the
   playhead falls strictly inside — the selected ones when there is a selection,
   otherwise every track — as ONE `MacroCommand` ("Split at Playhead"), a no-op
   when nothing is crossed. It posts `kMsgReloadEngine` like any other clip edit.
5. **Grid menu and snap indicator** in the same strip: a button labelled with
   `SnapGridLabel` (lit while snapping is on, dim for Off) that opens a popup of
   Bar / 1/2 / 1/4 / 1/8 / 1/16 / 1/32 / Triplets (marked) / Off, the active
   entry marked. `SetSnapGrid()`/`Snap()` are public so the functional test can
   drive the state a menu cannot be opened for. Zoom −/+ buttons complete the
   strip, matching the piano roll's two.
6. **Pointer feedback (2.1).** One hit-test helper answers "what is under the
   cursor" for both `MouseDown` and `MouseMoved` (zone = body / trim-left /
   trim-right / fade-in / fade-out / gain / slip, plus the clip/region and lane),
   so the cursor, the highlight and the gesture can never disagree. Cursors:
   **move** (over a clip body, pointer tool), **trim** (`B_CURSOR_ID_RESIZE_EAST_WEST`
   at an edge), **gain** (`…_NORTH_SOUTH`, Ctrl over an audio clip), **slip**
   (Alt over an audio clip — see item 7), and **fade** / scissors / pencil / glue
   / mute as small glyph cursors drawn from the same `icons::DrawArrangeTool`
   the palette uses, so a button and its cursor are one definition. Hover
   highlight: the grabbed clip edge (a bright bar), the fade corner grips, the
   tool button and the track-header control (M/S/R/I, pan knob, gain fader) under
   the pointer. Tooltips: the six tool buttons, the grid button, the header
   controls, the clip edges and the ruler.
7. **Slip by Alt-drag** (audio clips only — this is what makes the slip cursor
   honest, and it is the gesture M2.5 names as "slip edit"). Alt-drag over an
   audio clip moves `sourceOffset` by the horizontal delta (clamped at 0),
   previewed live and committed as one `SlipClipCommand` on release. A MIDI
   region has no source to slip, so Alt over one stays a move and there is no
   slip cursor there.
8. **Navigation (2.2).** Ctrl+wheel zooms **anchored on the pointer** (the frame
   under the cursor keeps its x; the pointer position is the last `MouseMoved`
   position, falling back to the view's centre); the keyboard −/+ and the menu's
   zoom keep the **playhead** under the same x (it was the left edge).
   Shift+wheel and a horizontal wheel (`be:wheel_delta_x`) scroll sideways;
   a plain wheel keeps scrolling vertically. **Real scrollbars**: one horizontal
   and one vertical `BScrollBar` subclass, children of the view, positioned in
   `FrameResized`, ranges in frames/pixels, proportions = visible/total, driving
   the view's offsets through `ValueChanged` (a re-entrancy guard keeps a
   programmatic sync from echoing back). **Scroll past the end**: the horizontal
   clamp becomes `ContentEndFrame()` (last clip/region, playhead, loop, punch and
   marker end) instead of the last clip's end, so the whole project can be
   scrolled off the left edge. **The piano roll follows the playhead**:
   `PianoRollView::SetPlayhead` scrolls the region when the playhead leaves the
   visible span (same 10 % margin policy as the timeline's follow).
   **Minimap: not built** — with real scrollbars, Zoom-to-Fit and scroll-past-end
   the orientation job is done, and a minimap is a second full-project draw per
   repaint while M1.5's 4 ms draw budget has still never been measured on the VM
   (Marc's T11 decision: measure first). Recorded so the decision is visible.
9. **Functional tests + Shots** (`tests/ui_functional_tests.cpp`): tool switching
   by keys 1–6, the split-at-playhead command, the snap indicator and
   `SnapFrame`-driven click placement, the zoom anchor (pointer and playhead),
   scroll-past-end, the sidebar scrollbars, and the roll following the playhead.
   A `Shot()` for the arrange strip, the tool palette in use, the snap menu
   indicator, the scrollbars and the hover highlight — in **both** theme modes
   and at 150 % where geometry moved.

## Definition of done

- Host: `cmake --build build-host` exit 0, `ctest --test-dir build-host` green
  (baseline on this branch: 56/56), ASan build + ctest green, and
  `sh scripts/haiku_syntax_check.sh` 0 FAIL.
- VM: `build` and `build-off` ctest green (baseline 55/55 and 51/51 + this
  package's checks).
- New host tests: `SnapGrid` (every kind, triplets, Off, variable tempo, a frame
  before 0) and the three commands (join audio, join MIDI incl. note re-basing,
  refuse when not adjacent, slip clamps at 0, undo restores exactly).
- New `ui_functional_tests` checks for each behaviour in item 9, each one
  **mutation-checked** (break it, watch the check fail, restore) and the mutation
  named in the commit body.
- Screenshots: the `DAW_UI_SHOTS` pass run twice (`DAW_UI_THEME=light` and
  `DAW_UI_THEME=dark`, screen unlocked), every shot showing the arrange strip,
  the palette, the scrollbars or the hover highlight opened at full size and the
  150 % shots too where geometry moved, what was looked at and what was fixed
  named in the record. Green tests are not done.
- The record names what could not be verified (a real mouse's cursor glyphs and
  hover feel are not testable on the VM) and any decision Marc owes.
