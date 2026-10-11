# Task: M2 "Arrange window feel" — time-range selection (2.4) and clip depth (2.5)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/arrange-depth` off `feature/arrange-feel` (the chain is master →
feature/theme-modes → feature/dock-browsers → feature/lv2-state →
feature/arrange-feel → yours; T1's two theme modes are in it, so **every UI
change is reviewed in both modes**). Same rules as every package: spec first,
one verification bar per branch, a record (`33-arrange-depth-PR.md`) kept as you
go, no `Co-Authored-By:` trailer.

This is the second slice of `docs/PLAN_1.0.md` §M2: items **2.4 (time-range
selection and its edit operations) and 2.5 (clip depth)**. Items 2.6 (track
depth) and 2.7 (markers) are a later branch and are **not** in scope. The
previous slice (`30-arrange-feel.md`) built 2.1–2.3 on the same files: the tool
palette, the `HitTest`/cursor/hover triple, navigation, and the `S` split; this
package extends that work and must not regress it.

## Codebase orientation (read before coding)

- Layering. `src/model/` is kit-free and builds + tests on the Linux host
  (`ctest --test-dir build-host`); `src/ui/` and `src/engine/` are Haiku-only and
  verified on the VM and by `sh scripts/haiku_syntax_check.sh`. Logic that can
  live in the model belongs there — it is the only place a behaviour can be
  host-tested and mutation-checked cheaply. `src/engine/Exporter.cpp` is
  kit-free: it is the executable spec for anything the live engine renders.
- `TimelineView` (`src/ui/TimelineView.{h,cpp}`) owns the arrangement: tool
  strip, ruler, lanes, clips, gestures. `HitTest(where, mods)` is the ONE answer
  the click path, the hover highlight and the cursor share. Drags preview by
  writing the model and `MouseUp` restores the pre-drag value and pushes ONE
  `CommandStack` command (`24-…-ENTRY.md` §7). `Snapped()` snaps through
  `src/model/SnapGrid.h`; `SnapFrame(tm, f, grid)` is the mathematics the menu,
  the indicator and the gestures share.
- The clipboard today is three scalars on the view (`fClipClip`/`fClipMidi`
  + flags) filled by the right-click menus and consumed by `PasteAtPlayhead`
  and `MSG_PASTE` — one clip at a time.
- The Edit menu is `MainWindow.cpp:333-338`: Undo, Redo, Paste. `CommandStack`
  already carries `UndoName()`/`RedoName()` (`src/model/Command.h`) and every
  command names itself (`Command::Name()`); nothing reads them for the menu.
- Child windows forward Space to the transport with one shared inline helper
  (`ForwardSpaceToTransport`, `src/ui/UiMetrics.h:76`) called from the
  `DispatchMessage` override of each window that has one. Cmd-Z is not
  forwarded anywhere: a child window with focus eats it.
- Fades. `ComputeCrossfades` (`src/model/Crossfade.h`) folds each clip's own
  fades together with any overlap-derived auto-crossfade and is used by the
  draw path, the engine (`Engine.cpp:275`) and the exporter
  (`Exporter.cpp:467`), so drawing and rendering cannot drift. The envelope is
  linear in `TrackStream::Mix` (`Engine.cpp:209-216`) and in `PlaceClip`
  (`Exporter.cpp:117-134`).
- Clip model: `Clip` and `MidiClip` (`src/model/Project.h`) — `MidiClip` already
  has `colorIndex` (0 = inherit the track's). `Track::CollectNotes/CollectEvents`
  flatten MIDI for the synth and are the ONE place a region's audibility is
  decided.
- IO is append-only line text (`src/model/ProjectIO.cpp`). A clip's optional
  fields ride after the closing quote of its path, parsed from `rfind('"')` —
  which is exactly why **new clip fields must be new LINES, not new tail
  fields**: a quoted name appended to the clip line would move the last quote
  and silently drop `gain`/`takeGroup`/`takeActive` for every reader, old and
  new. Compat tests for both directions live in `tests/projectio_tests.cpp`.
- The `Mute` tool (`TimelineView::MuteAt`) mutes the clip's TRACK — the
  boundary `30-arrange-feel-PR.md` recorded for this package to resolve.
- Tests: `tests/ui_functional_tests.cpp` — free `TestXxx` functions called in
  order from `TestThread`, `CHECK(cond)`, `Shot("name")` under `DAW_UI_SHOTS`,
  synthetic mouse messages carrying **both** `where` and `screen_where`, and
  **every view call under the window lock** (an unlocked `BView` call debuggers
  on the target — a crash dialog, not a failed check). Kit-free behaviour goes
  in the host suites (`tests/arrange_cmd_tests.cpp`, `projectio_tests.cpp`,
  `crossfade_tests.cpp` are the neighbours to match).

## Goal

A time range that behaves like a DAW's: drag it out of empty lane space or the
ruler, see it, and cut/copy/delete/insert-silence it across the tracks; an Edit
menu that says what it will undo and works from every window. And clips that
carry a name, a colour and a mute, whose waveforms and fades are drawn as they
sound, with a fade shape per clip and keyboard nudging.

## Work items

1. **Fade shapes** (`src/model/Fade.h`, kit-free header, host-tested).
   `enum class FadeShape { Linear, EqualPower, SCurve }`, `FadeShapeName`,
   `FadeGainAt(shape, t)` where `t` is the fraction through the fade (0 at the
   silent edge, 1 at full level): `t`, `sin(t*pi/2)`, `t*t*(3-2t)`. Equal-power
   ramps crossfade at constant power (in² + out² = 1). `Clip` gains
   `fadeShape` (default Linear, so existing projects keep their sound).
   `ClipFades` (`src/model/Crossfade.h`) gains `inShape`/`outShape` — the shape
   each effective fade must render with: the clip's own when the clip's own fade
   is the effective one, **EqualPower when the effective fade comes from an
   auto-crossfade** (the plan's "equal-power by default for crossfades").
   `ComputeCrossfades` decides it and treats a muted clip as absent (a muted
   neighbour is not a crossfade partner). Engine `TrackStream::Mix` and
   Exporter `PlaceClip` apply the shape; the timeline draws the ramps as the
   same curve, so what you see is what sounds.
2. **Clip depth model** (`src/model/Project.h`). `Clip` gains `name`,
   `colorIndex` (0 = inherit the track's, like `MidiClip`), `muted`.
   `MidiClip` gains `muted` too: the Mute tool needs ONE answer for the block
   it is pointed at, and a region is as muteable as a clip (the plan names only
   audio clips; this is the deliberate extension, recorded). `Track::
   CollectNotes/CollectEvents` skip muted regions; the engine and the exporter
   skip muted clips.
3. **Clip commands** (`src/model/Commands.{h,cpp}`, host-tested):
   `SetClipNameCommand`, `SetClipColorCommand`, `SetClipMuteCommand` (audio or
   region, one command), `SetClipFadeShapeCommand`.
4. **Time-range selection** (in `TimelineView`; the range is view state, not
   project state — nothing about a selection belongs in the file).
   - Gestures: a drag on empty lane content (>= 4 px, snapped) sets the range;
     Shift-drag keeps the previous rubber-band clip select (additive); an
     Alt-drag on the ruler sets it there (Alt = `B_OPTION_KEY`, the modifier
     the slip gesture already uses — careful: `Snapped()` reads Shift from the
     live event, so a Snapped() call inside the ruler path must not see Shift
     it did not ask for). A click without movement clears the range (the loop
     drag's own click-vs-drag rule). Esc clears it after the clip selection.
   - Scope: **the range is project-wide — every track** (there is no track
     selection until M2.6; inventing one here would be a second selection model
     for 2.6 to collide with. Recorded).
   - The highlight: a translucent band over the lane area between the edges,
     with solid edges in the lanes and a bar over the ruler, drawn in both
     themes from the tokens.
5. **Range operations** (`src/model/RangeEdit.{h,cpp}` + `Commands.{h,cpp}`,
   kit-free, host-tested; the UI only wires them):
   - `SliceClipToRange` / `SliceRegionToRange` / `ExtractRange`: the content of
     `[a,b)` as clips whose starts are **relative to `a`** — audio clips and
     MIDI regions cut at both edges (source offset advanced, fades and note
     lengths trimmed by the same amount, notes outside the window dropped so
     the copy is what the range sounds like). Inactive takes are skipped.
   - `RangeDeleteCommand(a,b)`: clears the range — clips wholly inside go,
     straddling ones are trimmed or split so no material sounds inside `[a,b)`
     and everything outside is untouched. Snapshots the affected tracks' clip
     lists for an exact Undo.
   - `RangeInsertSilenceCommand(a,b)`: ripples — everything that starts at or
     after `a` moves right by `b-a`; a clip straddling `a` is split at `a` and
     its right half moves with the rest, leaving exactly `b-a` of silence.
     One undo step, exact Undo.
   - `Cut` = extract + `RangeDeleteCommand` (one `MacroCommand`); `Copy` =
     extract only; `Paste` = one `MacroCommand` of `AddClipCommand` /
     `AddMidiClipCommand` at `playhead + item.startFrame`; `Duplicate` =
     paste at the range end (or the selection's own duplicate when clips are
     selected); `Select All` selects every clip and region.
   - Target track for a paste: the selected track when it exists and matches
     the item's kind, else the first track of that kind (the rule
     `PasteAtPlayhead` already uses).
6. **The Edit menu** (`MainWindow.{h,cpp}`): Undo, Redo (labels AND enabled
   state from `UndoName()`/`RedoName()`, refreshed on the 2 Hz title poll and
   after every menu-driven edit), separator, Cut, Copy, Paste, Delete,
   Select All, Duplicate, separator, Insert Silence. Cmd-X/C/V/A/D and Del;
   the existing bare `Del`, Ctrl-D and Cmd-V paths keep working. The ids live
   in `MainWindow.h` so the functional tests post exactly what the menu posts.
   **Child windows forward Cmd-Z / Cmd-Shift-Z** through one shared helper
   beside `ForwardSpaceToTransport` (`src/ui/UiMetrics.h`), called from every
   window that already forwards Space.
7. **The Mute tool means the CLIP** (`TimelineView::MuteAt`): toggles
   `Clip.muted` / `MidiClip.muted` of the block clicked (the resolved boundary
   from `30-arrange-feel.md`). A muted clip draws dimmed with an "M" badge, and
   a mute reads through to the engine, the exporter and the crossfade partner
   rule. The clip context menu gains Rename Clip…, Next Colour, Mute Clip and a
   Fade Shape submenu (Linear / Equal Power / S-Curve) so the new fields are
   reachable without a tool (an audio clip double-click opens Rename Clip too).
8. **Clip drawing** (`TimelineView`): the name strip shows `Clip.name` when
   set, else the file's base name, truncated to the strip (`TruncateString`,
   the existing rule); `colorIndex` tints the clip (0 = the track colour); the
   waveform is drawn **after** clip gain and the effective fade envelope (the
   same shape functions the engine uses), so the envelope the user sees is the
   envelope that sounds; the fade ramps follow the clip's shape.
9. **Slip and nudge.** Slip (Alt-drag, `SlipClipCommand`) already ships from
   2.1 — this package adds the missing end-to-end gesture check (the previous
   package tested only the cursor) and nothing else. **Nudge** with `,` and `.`
   moves the selected clips/regions by one grid step (the snap grid's step at
   each clip's position through the tempo map; grid Off nudges by a 1/16, the
   historical default), one `MacroCommand` per keypress.
10. **IO** (`src/model/ProjectIO.cpp`): two new optional lines,
    `clipinfo <clipId> <colorIndex> <fadeShape> "<name>"` and
    `clipmute <clipId> <0|1>` (ids are project-unique across audio clips and
    MIDI regions, so one mute line serves both), written only when a field
    differs from its default and right after the clip they belong to. The clip
    and midiclip lines themselves are byte-identical to the previous format, so
    an OLD reader sees exactly the fields it knows — compat tests both
    directions.

## Definition of done

- Host: `cmake --build build-host` exit 0, `ctest --test-dir build-host` green
  (baseline on this branch: 58/58), ASan build + ctest green, and
  `sh scripts/haiku_syntax_check.sh` 0 FAIL.
- VM: `build` and `build-off` ctest green (baseline 60/60 and 55/55 + this
  package's checks).
- New host tests: `FadeGainAt` (endpoints, monotonicity, equal-power constant
  power); `ComputeCrossfades` shapes (auto fade → EqualPower, own fade → clip
  shape, muted neighbour → no auto fade); the four clip commands (incl. Undo);
  `SliceClipToRange`/`SliceRegionToRange`/`ExtractRange` (both edges, fades,
  source offsets, notes, inactive takes); `RangeDeleteCommand` (inside /
  straddling / covering / gap, exact Undo); `RangeInsertSilenceCommand`
  (ripple length, split-at-a, exact Undo); ProjectIO round-trip of the new
  fields plus a compat test both ways (old-format file loads with defaults;
  new-format clip line is byte-identical to the old spelling).
- New `ui_functional_tests` checks, each **mutation-checked** (break it, watch
  it fail, restore) and the mutation named in the commit body: the range drag
  and its highlight, range cut/copy/paste/delete/insert-silence, the Edit
  menu's labels from `UndoName()`, `Cmd-Z` forwarded from a child window, clip
  mute / name / colour, the fade shape, nudge, and the slip gesture — **driven
  through the real widgets/paths**, never a hand-made message that skips them.
- Screenshots: the `DAW_UI_SHOTS` pass run twice (`DAW_UI_THEME=light` and
  `DAW_UI_THEME=dark`, screen unlocked), every shot showing the range
  highlight, the clip name/colour/mute, the shaped fades or the edited Edit
  menu opened at full size, and the 150 % shots too where geometry moved; what
  was looked at and what was fixed named in the record. Green tests are not
  done.
- The record names what could not be verified and any decision Marc owes.
