# Task: the arrange timeline — the UX defects a review ranked (package 50)

Branch `fix/ux-timeline` off `master`. Source: the UX review's ranked findings
list; **this brief is the scope and the justification** — the items below are
implemented as written, not re-litigated. Everything lives in
`src/ui/TimelineView.cpp` (5055 lines) unless a file is named.

## Orientation (read before coding)

- **The view.** `TimelineView` draws the ruler, one lane per track, and the
  clips/regions; `MouseDown`/`MouseMoved`/`MouseUp` own every gesture;
  `HitTest` is the one hit-test behind the hover highlight, the cursor, the
  scissors' cut line and the click paths (the M2.1 rule: they cannot disagree).
- **What is kit-free.** `src/ui/` does not compile on the host (Haiku kits).
  Policy that can be a pure function goes in `src/model/` (`SnapGrid.h`,
  `RangeEdit.h`, `Fade.h` are the precedents) so `ctest --test-dir build-host`
  can pin it. This package extracts four rules that way: the draw-order hit
  rule, the loop-range rule, the scrollbar step maths, and the marker move.
- **Edits go through `CommandStack`.** No model write without a command — the
  marker move is the one new command this package needs
  (`MoveMarkerCommand`, `src/model/Commands.h`).
- **Two other agents are editing `src/ui/MainWindow.cpp` concurrently.** This
  package's only MainWindow change is one `MessageReceived` case; the Edit menu
  is not touched at all (item C is solved inside the timeline).
- **`docs/USER_GUIDE.md` may be edited for items A and H only.**
- Never `git push`; commits carry no AI-attribution trailer.

## The items

**A. A bare ruler click silently turns the loop OFF (and a bare Ctrl-click the
punch).** `MouseUp`:3566-3591 clears `transport.loopEnabled` /
`punchEnabled` after a plain click. Contradiction: `loopEnabled` has its own
affordances (the transport's Loop button, `L`, Shift-L). Fix: a bare ruler
click is a **seek and nothing else** (both the loop and the punch drag paths);
the loop and the punch get **`Clear Loop` / `Clear Punch` in the ruler's
context menu** (`HandleRulerMenu`, beside the tempo/meter/marker entries),
each disabled when it has nothing to clear. Drag-to-set is unchanged.
`docs/USER_GUIDE.md` line ~57 updated.

**B. In a crossfade overlap the click picks the clip UNDERNEATH.** `HitTest`
(:722-727, MIDI :715-720), the MIDI and audio `MouseDown` paths (:2530-2537,
:2673-2676) and the menu's "clip under the cursor" helper (:1483-1493) all take
the FIRST clip in vector order, but clips are drawn in startFrame order
(:4225-4233), so the later one is on top. Fix: resolve **top-down** — among the
clips containing the point, the one drawn last (greatest `startFrame`; equal
starts, the later index). One kit-free helper
(`TopmostClipIndexAt`), used by every one of those paths, so the hover
highlight, the cursor and the scissors' cut line follow for free.

**C. Delete prefers a stale time range over the clip the user just selected.**
`RangeDelete` (:1807-1816) takes the range whenever `HasTimeRange()`; a clip
click never cleared the range (:2767-2770, :2616-2617). **Decision: the most
recent gesture wins**, enforced as an invariant — a clip-selection gesture
drops the range, a range that gets set drops the clip selection — so the two
selections are never both live and Delete can only act on the thing the user
touched last. (Chosen over "clip selection first + a separate Delete Range
item": the invariant is what makes the behaviour predictable, it keeps the Edit
menu — which another agent is editing — untouched, and it matches the click the
user just made.) The rule itself is a kit-free function
(`SelectionTargetFor`), tested with both paths.

**D. The Loop button lights with nothing to loop.** `ToggleLoop` (:558-559)
sets `loopEnabled = true` after *trying* to adopt marker brackets
(:4096-4104), which may set nothing; the lamp (TransportBar) and the status
line then claim looping while the engine walks a zero-length loop.
**Decision: adopt a default range — the bar around the playhead** (markers
first, as today; then the bar) and say so in the status line, rather than
refusing. With no bar to adopt at all, refuse: leave the lamp off and say
"Set a loop range on the ruler first". The bar maths is kit-free
(`DefaultLoopRange`).

**E. The scrollbar's arrows and trough do nothing visible.** `SetSteps(12,120)`
(:75) but the range is in **frames** (`SetRange(0, maxScroll)`, :1362-1365), so
an arrow click moves ~0.02 px at the default zoom. Fix: set the steps whenever
the range is set, scaled with the zoom — small = 16 px of frames, large = the
visible width in frames (kit-free `ScrollStepsFor`).

**F. Empty-lane right-click and clicks below the last lane are silent.** The
paste menu only appears with a non-empty clipboard (:2819-2823); a click in the
1 px inter-lane gap or below the last lane falls out at :2376-2378 without even
clearing the selection. Fix: **one lane-area context menu** — Paste (disabled
when the clipboard is empty), Select All, Insert Silence (disabled without a
range), Zoom to Fit, New Audio Track, New MIDI Track — and those "empty space"
clicks clear the clip selection and the time range like a click inside a lane
does.

**G. An empty project is a blank void.** `empty.dawproj` has no tracks and
`DrawLanes` (:4187-4286) has no empty-state branch. Draw a centred hint in the
lane area ("Drag audio or MIDI files here" / "or Track ▸ New Audio Track"),
the same idea `MarkersWindow.cpp:143-157` uses (`UI_GUIDELINES.md` §3).

**H. Slip edit is unreachable with Alt on a PC keyboard.** The slip zone wants
`B_OPTION_KEY` only (:706, cursor :774) while every other Alt gesture accepts
`B_OPTION_KEY | B_COMMAND_KEY` because Haiku maps a PC's Alt to Command
(:2340-2343, :2794-2799). Accept both; add the missing slip line to the user
guide's Clips paragraph.

**I. Markers: no drag; a right-click on a flag deletes with no confirmation.**
Add: left-drag a flag to move it (snapped, **one** undo step, via a new
`MoveMarkerCommand`), and right-click on a flag opens a small menu (Rename…,
Delete) instead of deleting outright. A plain click still seeks.

**J. Small ones, same file.**
1. The ruler draws marker names untruncated (:3950-3958) while the lane below
   truncates the same string (:4070-4073) — truncate the ruler's to the next
   flag or the view's right edge.
2. A MIDI region's context menu is a 3-item stub (:1455-1468) while an audio
   clip gets the full menu (:1470-1514). **One builder** for both, with the
   entries the model cannot serve dropped for a region: Rename Clip (no `name`
   field), Next Colour (`SetClipColorCommand` is audio-only), Fade Shape (no
   shape field), and the three decode-based region ops.
3. `SetProject` (:277-291) clears the selection and the range but not
   `fClipboard` — copy in project A, open B, paste, and B gets a clip pointing
   at A's media. Clear it too.
4. Double-clicking an audio clip opens `RenameWindow` at the raw click point
   (:2750-2756), which is never clamped to the screen. **Decision: rename the
   clip in place in its name strip** — the machinery exists for track names
   (`BeginTrackRename`/`HandleRenameKey`/`DrawNameField`) and the strip is
   already drawn (`DrawClip`); a clip whose strip is too narrow for a field
   falls back to the window, and that window (and every other caller's) is
   clamped to `BScreen` in `RenameWindow`'s constructor.

## Tests

Kit-free, host (`tests/arrange_logic_tests.cpp`, registered in CMakeLists):

- the draw-order hit rule: overlaps both ways, equal starts, inactive takes
  skipped, points outside every span, spans that touch but do not overlap;
- the loop-range rule: the bar around the playhead in 4/4 and in a
  `3/4`-then-`7/8` map, at a bar line, and the refusal case;
- the scrollbar steps: the 16 px small step and the visible-width large step at
  two zooms (the defect: a step smaller than a pixel of content);
- the Delete-target rule: range only, clips only, both, neither;
- `MoveMarkerCommand` through a `CommandStack`: one step, redo, undo restores
  the frame AND the order for a move across another marker.

On the VM (`tests/ui_functional_tests.cpp`, extended): the marker drag (one
undo step, "Move Marker"), the marker flag's menu actions through the same
methods the menu calls (a popup cannot be driven by the suite — the established
pattern), Delete acting on the clip a click just selected, and the loop key
adopting a bar.

Every new behaviour is mutation-checked (break it, watch the right test fail,
restore) and the mutations are named in the record.

## Definition of done

- `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build
  build-host -j2` exit 0; `ctest --test-dir build-host` green with the count;
  `sh scripts/haiku_syntax_check.sh` — no new FAILs (the four FLAC/Vorbis
  FAILs are pre-existing).
- `docs/agent-prompts/50-ux-timeline-PR.md`: what changed, the choices made in
  C/D/J4 and why, the three pasted results, the mutations by name, what is
  unverified here (the screenshot pass and the VM suite are the orchestrator's),
  and any finding that turned out to be wrong.
- No `Co-Authored-By:` or other AI attribution, anywhere.
