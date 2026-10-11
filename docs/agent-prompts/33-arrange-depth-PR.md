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

### The first VM run faulted at the very end of the new test

ctest came back `61/61 ui_functional_tests ... Signal 21` after 180 s. Signal 21
is `SIGKILLTHR`, i.e. the Terminate a crash dialog sends — the fault is the
kernel's:

```
KERN: 123772: DEBUGGER: looper must be locked before proceeding
KERN: stack trace, current PC ... _kern_debugger
KERN:   BLooper::Unlock() + 0x11
KERN:   TestArrangeDepth(...) + 0x4993
KERN:   TestThread + 0x324
```

The test's own bug, and the same class the previous package hit: it closed the
child window with `Lock(); Quit(); Unlock()` — `Quit()` tears the looper down,
and the `Unlock()` that followed tripped the assertion. Every other close in
this file is `Lock(); Quit();` with nothing after; the fix is one line
(`fa31da0`). A fault is not something a `CHECK` can catch: the run has to reach
the line, which is what the whole-test-on-the-target run is for.

The run also proved the fault path costs a cycle: the crash dialog froze the
VM and the next UI run hung behind it until the dialog was cleared with
`virsh ... sendkey ret` (the machine rule that exists for exactly this).

### The second run: two real check failures, one root cause

With the fault fixed the whole test ran and reported six FAILs — all of them
from one mistake of mine: the range drag in section 1 pressed at frame 60000,
which is INSIDE the fixture clip. That is a clip drag, not a range drag, so no
range appeared **and the clip was quietly moved 30000 frames right**, which is
what the range-delete section (a range that no longer covered its clip) then
tripped over. Every other section passed on that run — the ruler Alt-drag, the
Edit menu's `UndoName` label, copy/paste, insert silence, select-all and
duplicate, the mute tool, the rename, colour, fade shapes, nudge, slip and the
child-window Cmd-Z. The fix (`ddc363c`) presses on empty lane content beyond
the fixtures and asserts the clip did not move, so the next cut of this test
cannot repeat the mistake silently.

### VM mutation checks (applied, watched to fail, reverted)

Each one is applied to the worktree, synced to the VM as a throwaway ref, built
and run there; the counts are the run's own FAIL count from `/tmp/mut.log`.

| mutation | failures |
|---|---|
| `range-never` — `SetTimeRange` never sets the range valid | 18: the range drag, the ruler drag and every range operation that followed |
| `mute-track` — the Mute tool mutes the TRACK again (the pre-M2.5 body) | 4: the clip mute, the "track untouched" check, the unmute, the region mute |
| `nudge-one-frame` — nudge by one frame instead of the grid step | 5: the nudge checks (baseline 4 in that run) |
| `no-cmdz-forward` — `ForwardUndoRedo` returns false | 4 = the baseline 3 + the child-window Cmd-Z check (the only one that moved) |
| `no-perstep-refresh` — the header fader/pan preview stops refreshing the panes | 4 in the first run — inconclusive, because that build still carried the check's first (non-polling) version, whose own baseline was 4 and which failed on a CORRECT build twice under load. The check was then rewritten to poll with the mouse button held (`5533e9a`), passes on the correct build (the green 0-failure run and the no-cmdz-forward run), and the re-run with the mutation was queued as the last cycle |

### The P1 Marc reported: a header fader drag was dead in the other panes

Reported while this branch was in flight, and fixed in it (`84a2eb1`): moving a
fader or pan knob in the mixer or the inspector updates the timeline live, but
dragging the **track header's** gain fader or pan knob left the inspector and
the mixer showing the old value until the mouse came up.

The mixer/inspector → timeline direction has always been live because
`InspectorView::Refresh()` posts `kMsgUiRefresh` on every move of its own drag.
The reverse had no such post: `TimelineView::PreviewDrag` wrote the model
(preview) and refreshed the panes only from `MouseUp`. The fix is that same
message at the same cadence in the gain/pan preview — no second mechanism, and
the undo discipline is untouched (preview writes, ONE command on mouse-up).

**Widened, as asked:** the other preview drags in the same shape were checked
and deliberately not changed — clip move/resize/fade, clip gain, slip, the
automation breakpoints and the ruler loop/punch change nothing another pane
displays (the inspector and the mixer show track gain/pan/mute/solo/inserts;
the docked piano roll shows a region's notes, not its timeline position). That
is recorded so the omission reads as a decision.

**Evidence:** `TestArrangeDepth` section 15 opens the real mixer (View > Mixer's
own message), presses on the header fader, moves, and asserts **before the
mouse-up** that the mixer's own strip snapshot already carries the new gain.
The mixer is the observable: the inspector draws straight from the model, so it
cannot distinguish "refreshed" from "not refreshed" in a test, while the rack
holds a pushed snapshot (`kMsgMixStrips`) that only a refresh updates.
**Mutation:** removing the per-step post makes exactly that check fail (the
`no-perstep-refresh` mutation, run on the VM with the others).

### VM

| Check | Result |
|---|---|
| VM `build` — `ctest -E ui_functional_tests` | 60/60 |
| VM `build-off` — `ctest -E ui_functional_tests` | 55/55 |
| VM `ui_functional_tests`, System mode (`/tmp/ui-final.log`, settings cleared) | 411 checks, **0 failures** |
| VM `ui_functional_tests`, `DAW_UI_THEME=light` (with shots) | queued behind another agent's VM hold; the shots I reviewed are the light pass listed under Screenshots |
| VM `ui_functional_tests`, `DAW_UI_THEME=dark` (with shots) | same |

The UI suite is measured by its own run (its log is kept whole) rather than
inside ctest, because its stdout is what localises a failure; the other 60/55
suites are green either way.

**The three checks that followed this branch around were a stale SETTINGS
file, not this branch.** `TestDockedEditor`'s `inspGone`/`inspBack` and
`TestDockPages`' plugin pick failed in every run on this branch (light run,
dark run, and all five mutation runs). Pinned down in three steps:

1. A diagnostic cycle ran the same suite on the BASE commit (`e47fd16`): it
   crashed early (a window-appearance wait in an unrelated test) — the VM's
   desktop was already unwell — so base-vs-branch could not be settled that way.
2. The branch was then built WITH temporary prints around the toggle (the
   view's presence AND the View > Inspector item's marked state) and around the
   browser pick (its list's row count). The prints named it exactly:

   ```
   DIAG before toggle 1: view=0 marked=0     <- the inspector was ALREADY hidden
   DIAG toggle 1 took 200024us wall
   DIAG after toggle 1:  view=1 marked=1     <- the toggle SHOWED it
   FAIL inspGone
   DIAG after toggle 2:  view=0 marked=0
   FAIL inspBack
   DIAG plugin list rows=14                  <- the browser was fine
   FAIL <the plugin pick never landed>
   ```

   Flag and view agree — the pane is simply hidden at the start of the test,
   which inverts its two checks; the second toggle then leaves the inspector
   DETACHED, and the plugin pick (whose message targets the inspector's view)
   has nowhere to land. Chain explained.
3. Why was it hidden? `~/config/settings/HaikuDAW/settings` persists the panes
   (`uiinsp`, M1.4). A run that ends with the inspector hidden (which is
   exactly what the inverted toggles do) writes `uiinsp 0`, and every later run
   then STARTS hidden. With that file deleted: `ui_functional_tests: 411
   checks, 0 failures` — every check of this branch and every check of those
   two tests green in one run.

That is the same wedge the HANDOFF records for `recovery.dawproj`, in its
settings-file form, and the fix is the same: the suite now deletes the settings
file at startup (`f38e9b0`), so one run's pane state cannot decide the next
run's result. The lesson is also added to `docs/HANDOFF.md`.

A second observation from the same runs: a **HaikuDepot window left running by
another session** sits in front of the DAW from shot 29 on, so those shots are
not reviewable, and the test app was once left alive for ~10 minutes after its
summary (it exited on its own). Both are VM-desktop conditions of a shared
machine, not this branch's code.

## Screenshots

The `DAW_UI_SHOTS` pass in **both** modes (`DAW_UI_THEME=light` and
`DAW_UI_THEME=dark`; 47 shots each, `docs/agent-prompts/shots-33/<mode>/`).
Shots I opened at full size (and zoomed on the clip strips and the range
edges):

- `22-depth-range` (both modes) — the range band over the lanes with its solid
  edges and the bar + edges on the ruler, drawn over the clips and under the
  playhead.
- `23-depth-range-ruler` (both) — a range made by the Alt-drag on the ruler.
- `24-depth-edit-menu` (both) — the Edit menu bar and the tool strip (an OPEN
  menu cannot be photographed without a real pointer hold, so the labels are
  asserted by readback instead; see below).
- `25-depth-clip-muted` (both) — the muted clip: desaturated body, grey strip,
  red "M" badge.
- `26-depth-clip-named` (both) — the renamed clip's strip showing "Ver…".
- `27-depth-clip-colour` (both) — the same clip tinted by `colorIndex` (pink).
- `28-depth-fade-scurve` (both) — the S-curve fade-in ramp drawn as a curve.
- Regression: `00-startup`, `13-playing`, `17-arrange-clips`,
  `20-arrange-scrollbars`, `14-theme-150` (the 150 % case where the name
  truncation matters: "haiku_daw…" fits its strip at 1.5× ✓).

**What I fixed from the pass:** nothing structural — every shot showed the
feature drawing as intended in both modes. Two things were chased and cleared:
(a) `arrange-roll-follow` and the later shots report `shot failed:` — the
`screenshot` tool exits non-zero even when the file lands (`docs/UI_GUIDELINES`
§1 says so), and the files are all there; (b) from shot 29 on, a **HaikuDepot
window that another session left running** sits in front of the DAW window, so
those shots are not reviewable — they are other packages' tests and none of
them shows this branch's change. My own shots (15–28) are all before it
appears, and the two "shot failed" lines inside my range's shots are the same
tool noise.

**150 %:** no layout metric moved in this branch (the range band and the clip
strips use the existing rects), so there is no new geometry to re-shoot at
1.5×; `14-theme-150` (which shows the clip names truncated at 1.5×) was
reviewed and is in the set.

(filled in by the run below)

## What could not be verified, and questions for Marc

- **The pane failures' first cause on this VM was never pinned to a single
  run** — only to the mechanism (a persisted `uiinsp 0`). The chain is now
  broken by the harness fix, and the suite is green; the run that first left
  the inspector hidden is not identifiable after the fact (several sessions
  share this VM's settings file).
- **The VM was in a degraded state for part of this work**: the BASE commit's
  run crashed before reaching the pane tests (a window-appearance wait), the
  `play_clip` link failed once because two of my own cycles built the same
  directory concurrently, and a HaikuDepot window from another session covers
  the DAW from shot 29 on. None of it shows in the branch's checks — but it is
  why the base-vs-branch comparison could not be made the clean way.

- **The physical Alt key.** Haiku maps the PC's Alt to `B_COMMAND_KEY` and the
  Option/Windows key to `B_OPTION_KEY`; this codebase's existing "Alt" gestures
  (the slip edit from 2.1) test `B_OPTION_KEY`. The range drag accepts BOTH, so
  a user's Alt makes a range either way, but **the slip gesture accepts only
  `B_OPTION_KEY`** — a pre-existing choice from 2.1, left alone here. If Marc
  wants one modifier for both, it is a one-line change in `HitTest`'s slip zone
  and worth deciding before 1.0.
- **An open menu cannot be photographed on this VM.** The Edit menu's Undo/Redo
  labels are asserted by reading the items back (`BMenuItem::Label()`), and the
  shots show the window with the menu bar; opening the menu needs a real
  pointer held down, which the synthetic-event suite cannot do without wedging
  the window thread.
- **Cursor glyphs and tooltip pop-ups** — the same limits the previous package
  recorded: the mapping and the tooltip strings are set (and asserted) but a
  real mouse's glyphs and the tooltip manager's idle timer are desktop checks.
- **The drag's feel on real hardware** (how the range band tracks a real
  pointer, whether the 4 px threshold is right with a real mouse) — VM
  screenshots and synthetic events cannot measure it.

## What could not be verified

(filled in by the run below)
