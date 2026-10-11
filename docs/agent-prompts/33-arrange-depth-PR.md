# Record — M2 "Arrange window feel" (time-range selection, clip depth)

Branch `feature/arrange-depth` off `feature/arrange-feel`. Spec:
`33-arrange-depth.md`. Scope: `docs/PLAN_1.0.md` §M2 items **2.4** and **2.5**.
Items 2.6 (track depth) and 2.7 (markers) are the follow-up branch.

## What landed

**Model (kit-free, host-tested).**

- `src/model/Fade.h` — `FadeShape` (Linear / EqualPower / SCurve) and
  `FadeGainAt(shape, t)`, where `t` is the fraction through the fade from its
  silent edge. `ClipFadeEnvelope` (in `Crossfade.h`) is the envelope function
  the engine, the exporter and the timeline's waveform all call, so a curve
  cannot drift between what sounds and what is drawn.
- `ComputeCrossfades` now reports **the shape each effective fade renders
  with**: the clip's own `fadeShape` when the clip's own fade is the effective
  one, and **EqualPower when the effective fade is an auto-crossfade** (the
  plan's "equal-power by default for crossfades"; the two mirrored ramps sum to
  constant power). A **muted** clip is not a crossfade partner.
- `Clip` gains `name`, `colorIndex` (0 = inherit the track's, the rule
  `MidiClip` already used), `muted` and `fadeShape`; `MidiClip` gains `muted`
  (a deliberate extension past the plan's letter: the Mute tool needs ONE
  answer for the block it is pointed at, and a region is as muteable as a
  clip). `CollectNotes`/`CollectEvents` skip a muted region; the engine skips a
  muted clip's stream and the exporter skips it too.
- `src/model/RangeEdit.{h,cpp}` — `SliceClipToRange`, `SliceRegionToRange`,
  `ExtractRange`: the content of `[a,b)` with starts **relative to `a`**. Fades
  trim by the amount the edit removes from their edge; a region's notes/events
  are rebased to the range, dropped when they start outside it and clipped at
  its end, so a range copy is what the range sounds like.
- `RangeDeleteCommand`, `RangeInsertSilenceCommand` — clear a range (remove,
  trim or split) and ripple a range's length of silence from its start. Both
  address every track (no track selection exists until M2.6) and every clip
  including hidden loop-record takes (a range edit that skipped a take would
  desync the group), and both undo exactly from a per-track clip-list snapshot.
- `SetClipNameCommand`, `SetClipColorCommand`, `SetClipMuteCommand` (audio or
  region, one command), `SetClipFadeShapeCommand`.
- `SnapGrid.h` — `SnapBarBeats` (shared by `SnapFrame` and the new `SnapStep`)
  and `SnapStep(tm, at, grid)`, the frame distance between adjacent grid lines
  near a frame. Grid Off nudges by a 1/16 — the editor's historical step, so a
  nudge always moves by something visible.
- `ProjectIO` — two new optional lines: `clipinfo <id> <color> <fadeShape>
  "<name>"` and `clipmute <id> <0|1>` (ids are project-unique, so one mute line
  serves clips and regions). They are **lines, never tail fields**: the clip
  line's optional tail is parsed from its last quote, so a quoted name appended
  there would silently drop gain/takeGroup/takeActive for every reader, old and
  new. A project that never touches the new fields writes the legacy bytes
  exactly.

**Engine / exporter (Haiku-only + kit-free exporter).**

- `TrackStream` takes the two fade shapes and its RT mix uses
  `ClipFadeEnvelope`; `Engine::Load` skips muted clips (no stream, no disk
  thread — clip mute is a structural edit that posts `kMsgReloadEngine`, like a
  move).
- `Exporter::PlaceClip` uses the same envelope (it used to multiply the two
  ramps where the engine takes the min; one definition now, so a bounce and
  playback agree on the curve as well as the length) and skips muted clips.

**TimelineView (Haiku-side).**

- **Time range** (M2.4): a drag on empty lane content (snapped, with the loop
  drag's click-vs-drag threshold) or an Alt-drag on the ruler sets it; Shift
  keeps the rubber-band clip select the plain drag used to be; a bare click
  clears it; Esc drops the selection and then the range. It draws as a
  translucent band over every lane plus a bar and edges on the ruler. It is
  **view state** — nothing about a selection belongs in the project file.
- **Range operations**: Cut (copy + delete), Copy, Delete, Insert Silence,
  Select All, Duplicate. The clipboard holds **many** clips with starts
  relative to the earliest copy, so a range keeps its internal spacing; Paste
  lands at the playhead on the **selected track** (a click anywhere in a lane
  now selects that track, which is what makes that phrase mean anything) with
  the first track of the item's kind as the fallback. Each operation is one
  undo step.
- **Clip depth** (M2.5): the **Mute tool mutes the block** under the pointer
  (`Clip.muted` / `MidiClip.muted`) — the boundary `30-arrange-feel-PR.md`
  recorded as this package's to resolve; the header's M button stays the track
  mute. A muted block draws desaturated with an M badge. The clip context menu
  gains Rename Clip…, Next Colour, Mute/Unmute Clip and a Fade Shape submenu,
  and a double-click renames a clip (`RenameWindow` posts the clip id back in
  its `aux` field). The clip-depth actions are methods, so the menu and the
  functional test run the same code.
- **Drawing**: the waveform is drawn after clip gain **and** the effective fade
  envelope (the same `ClipFadeEnvelope` the renderers use), the fade ramps draw
  as their actual curve, the name comes from `Clip.name` or the file's base
  name and truncates to the strip, and `colorIndex` tints the clip.
- **Nudge**: `,` and `.` move the selection one grid step as one undo step.
- **Slip** (Alt-drag) shipped in 2.1; this package adds the missing end-to-end
  gesture check — the previous package tested only the cursor.

## Decisions (recorded, not discovered)

- **The range is project-wide.** Every track is affected. There is no per-track
  selection until M2.6, and inventing a second selection model here would be
  something for 2.6 to collide with.
- **A muted clip is not a crossfade partner** and contributes nothing to what
  the range "sounds like" (it is copied with its mute).
- **An inactive loop-record take is not copied** (you copy what you can see),
  but range Delete/Insert Silence do touch it (the structure has to stay
  consistent when takes are cycled). A pasted range is an ordinary clip, not a
  new take group.
- **Region deletes keep content** (the non-destructive-window rule a manual
  trim already follows): a delete over a region narrows the window and rebases
  the content; notes the delete covered are silent until the window widens
  again.
- **Insert Silence ripples clips only** — the playhead, loop and markers stay
  put (markers are M2.7's).
- **Fade shapes are audio-clip only** (a MIDI region's "fade" is a velocity
  ramp; equal-power for a velocity ramp has no meaning). The auto-crossfade
  default is equal power, exactly as the plan asks.
- **Nudge with the grid Off moves a 1/16**, not one frame.
- **Clips (not just tracks) can be muted**, including MIDI regions — see above.

## Boundaries the follow-up owns

- Track depth (2.6): rename in place, lane-edge resize, colour picker,
  Duplicate Track, exclusive solo, input selector, collapse/hide. Nothing here
  changes those paths.
- Markers (2.7): the marker lane and its window; a ripple does not move markers
  (recorded above).
- Zoom/scroll state, the tool and the snap grid stay per-session view state.

## Verification

### Host

| Check | Result |
|---|---|
| `cmake --build build-host` | exit 0 |
| `ctest --test-dir build-host` | 59/59 (baseline 58/58 + `range_edit_tests`) |
| ASan build + ctest | 59/59 |
| `sh scripts/haiku_syntax_check.sh` | 68 files, 0 FAIL |

New host tests: `tests/range_edit_tests.cpp` (147 checks: the fade curve,
`ClipFadeEnvelope`'s ramp values and its min-combine of overlapping ramps,
equal-power pairing, the shape rules, `SliceClipToRange`/`SliceRegionToRange`/
`ExtractRange`, both range commands with exact Undo, the four clip commands,
a muted region contributing no notes), `tests/snapgrid_tests.cpp` +25 (SnapStep
per division, triplets, Off, meter and tempo changes), `tests/projectio_tests.cpp`
+133 (round-trip of the new fields, the clip line byte-identical to the legacy
spelling, an old file loading with defaults, a truncated `clipinfo` skipped).

### Host mutation checks (applied, watched to fail, reverted)

| mutation | failures |
|---|---|
| `Fade.h` EqualPower returns `t` | 9 |
| `FadeShapeFromInt` forgets SCurve | 1 |
| auto-crossfade keeps the clip's shape, not equal power | 1 |
| a muted clip still crossfades | 2 |
| `SliceClipToRange` drops the source-offset advance | 2 |
| `SliceRegionToRange` stops rebasing notes | 3 |
| RangeDelete deletes a covering clip whole instead of splitting | 3 |
| RangeDelete keeps the fade-out untrimmed | 1 |
| RangeInsertSilence does not shift clips at/after the point | 1 |
| RangeInsertSilence leaves the range length out of the split shift | 2 |
| `SetClipMuteCommand` ignores the midi flag | 4 |
| `CollectNotes` stops honouring a muted region | 1 |
| `clipinfo` moved onto the clip line as tail fields | 6 |
| ProjectIO ignores `clipmute` for a MIDI region | 1 |
| `SnapStep` ignores the triplet flag | 1 |
| `SnapStep` Off does not fall back to a 1/16 | 1 |

### VM

(filled in by the run below)

## Screenshots

(filled in by the run below)

## What could not be verified

(filled in by the run below)
