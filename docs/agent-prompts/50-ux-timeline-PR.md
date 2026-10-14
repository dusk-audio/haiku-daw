# Record — package 50: the arrange timeline's UX defects

Branch `fix/ux-timeline` off `master`. Spec: `50-ux-timeline.md`. Source: the
UX review's ranked findings list — that list is the justification, so the items
below are implemented as written; the three places where the brief left a
choice (C, D, J4) are decided here and say why.

Everything lives in `src/ui/TimelineView.cpp` unless a file is named.

## What landed

**A. A bare ruler click is a seek and nothing else.** `MouseUp` no longer
clears `transport.loopEnabled` (the loop drag path) or `transport.punchEnabled`
(the Ctrl-drag path) after a click that never moved. Clearing is an explicit
affordance now: **"Clear Loop"** and **"Clear Punch"** in the ruler's
right-click menu (`HandleRulerMenu`), each disabled while there is nothing to
clear; Clear Loop also drops the range so the next `L` adopts a fresh one.
Drag-to-set is untouched, and a bare Ctrl-click seeks like any other.
`docs/USER_GUIDE.md` updated.

**B. The draw-order hit rule.** All four places that decided "which block is
under this point" now call one kit-free function, `TopmostClipIndexAt`
(`src/model/ArrangeLogic.h`): `HitTest` (audio and MIDI), the MIDI `MouseDown`
path, the audio `MouseDown` path, and the block menu's state-dependent labels
helper. Among the blocks containing the point it returns the one drawn LAST —
greatest `startFrame`, ties to the later index — which is the one on top of a
crossfade. The hover highlight, the cursor and the scissors' cut line read
`HitTest`, so they follow for free.

**C. Delete acts on the selection the user touched last (the decision).**
Chosen: **the newest gesture wins**, enforced as an invariant — a clip
selection gesture (a click, a Shift-click, a band select, Select All) clears
the time range, and a range that gets SET clears the clip selection (one line
in `SetTimeRange`, which is where every range finally lands). The two are never
both live, so `SelectionTargetFor(rangeValid, hasClips)` — the rule
`RangeDelete` / `RangeCut` / `RangeCopy` / the Delete key all read — is
unambiguous. Why this over "clip selection first + its own Delete Range item":
it is the behaviour the user's own gesture predicts, it needs no Edit-menu
change (the menu is being edited by two other agents this week), and it keeps
one rule instead of two special cases. Cost, stated plainly: `Cmd-A` now drops
a range you made a moment ago (the band visibly disappears, so nothing lies)
and Insert Silence needs the range re-made — which is the same "newest wins"
rule read the other way.

**D. The Loop button lights only for a loop the engine can walk (the
decision).** Chosen: **adopt a default range — the bar around the playhead**,
rather than refusing. `ToggleLoop` now tries, in order: the range the model
already has, the markers bracketing the playhead (as before), then
`DefaultLoopRange(tempoMap, playhead)` (kit-free). It posts the adopted range
to the status line ("Loop: bar 3") so a loop the user did not draw names
itself. If NO bar can be built at all (a tempo map whose bar rounds to zero
frames), the toggle refuses: the lamp stays off and the status line says "Set a
loop range on the ruler first". `MainWindow` gained the one
`kMsgTransportStatus` case that makes a view able to say that.

**E. The scrollbar's steps follow the zoom.** The constructor no longer sets
the horizontal bar's steps (the range is in frames, and the stock 12/120 was a
fiftieth of a pixel); `SyncScrollBars` sets them through the kit-free
`ScrollStepsFor` — small = 16 px of frames, large = one screenful — cached like
the range and proportion are. The vertical bar (whose range IS pixels) keeps
the stock steps.

**F. One lane-area context menu.** `LaneAreaMenu` serves the empty part of a
lane, the 1 px inter-lane gap and everything below the last lane: **Paste**
(disabled when the clipboard is empty; onto the clicked track at the clicked
frame, else at the playhead), **Select All**, **Insert Silence** (disabled
without a range), **Zoom to Fit**, **New Audio Track**, **New MIDI Track** (the
last two posting MainWindow's existing `MSG_NEW_AUDIO` / `MSG_NEW_MIDI` — no
new message ids). Those "empty space" clicks now clear the clip selection AND
the time range, like a click inside a lane does.

**G. An empty project says how to fill itself.** `DrawLanes` draws a centred
two-line hint in the lane area when the project has no tracks ("Drag audio or
MIDI files here" / "or use Track ▸ New Audio Track"), the small second line's
font put back afterwards — the view keeps one font between draws.

**H. Slip edit on a PC keyboard.** The slip zone and its cursor accept
`B_OPTION_KEY | B_COMMAND_KEY`, like every other Alt gesture in the file. The
user guide's Clips paragraph gained the slip line.

**I. Markers: drag and a menu.** Left-drag a flag to move it — the release
lands on the snapped grid and commits ONE `MoveMarkerCommand` (new, in
`src/model/Commands.h`, re-inserting at the sorted position so a move across
another marker keeps `markers` ordered); a press released where it was grabbed
is still click-to-seek. Right-click a flag opens **Rename Marker… / Delete
Marker** instead of deleting outright; empty lane space still adds a marker.
The tooltips and the drag's ghost line follow.

**J. The small ones.**
1. The ruler's marker names are truncated to the next flag (or the view's right
   edge) — the lane below already truncated the same string.
2. `ContextMenu` and `AudioClipMenu` became ONE `ClipMenu(where, midi,
   withTake)`; for a region it drops what the model cannot serve: Rename Clip
   (no `name` field), Next Colour (`SetClipColorCommand` is audio-only),
   Fade Shape (a region's fades are velocity ramps, shape-less) and the three
   decode-based ops (Normalize/Reverse/Strip Silence). A region keeps Copy,
   Delete, Split here, Next Take, Mute/Unmute (the model has `muted`) and Clear
   Fades (`SetMidiClipFadeCommand`). Colour for regions was left out rather
   than extending the command — a model change the brief did not ask for.
3. `SetProject` clears `fClipboard` too — a clip copied in project A no longer
   pastes into B pointing at A's relative media path.
4. **Clip rename happens in place, in the clip's name strip** (the decision):
   `BeginClipRename` reuses the track rename's field, key handling and caret
   (`DrawRenameFieldIn(rect)` is the one drawing; `CommitRename` branches on
   which name is open). Chosen because the machinery was there and the strip is
   already drawn — a window floating over the arrangement was the wrong object.
   A clip whose strip is narrower than 48 px falls back to `RenameWindow`, and
   **that window is now clamped to `BScreen`** in its constructor, which also
   fixes the track and marker prompts (they anchor at a click point too).
   Folding the lane that owns the open field commits it first, so the field can
   never hold the keys while showing nothing.

## Kit-free rules (host-tested)

`src/model/ArrangeLogic.h`: `TopmostClipIndexAt`, `DefaultLoopRange`,
`ScrollStepsFor`, `SelectionTargetFor` + `SelectTarget`. `MoveMarkerCommand`
in `src/model/Commands.{h,cpp}`. Tests: `tests/arrange_logic_tests.cpp`
(76 checks), registered in `CMakeLists.txt`.

## Verification

- `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build
  build-host -j2` — exit 0.
- `ctest --test-dir build-host` — **76/76 passed** (was 75/75; the new suite is
  `arrange_logic_tests`, 76 checks).
- `sh scripts/haiku_syntax_check.sh` — **83 OK, 4 FAIL**, and the four are the
  pre-existing FLAC/Vorbis files (`FlacSink`, `FlacSource`, `VorbisSink`,
  `VorbisSource`), unchanged by this branch.

### Mutations (broken, watched fail, restored — all in `arrange_logic_tests`)

| # | Mutation | Caught by |
|---|---|---|
| 1 | `TopmostClipIndexAt`: first match wins (the old rule) | "topmost clip" — 6 checks (the crossfade, the equal starts, the shown/hidden takes, the region overlap) |
| 2 | `DefaultLoopRange`: the bar AFTER the playhead (`bars + 1`) | "default loop range" — 16 checks (every bar position in both maps) |
| 3 | `ScrollStepsFor`: small step back to the stock 12 frames | "scrollbar steps" — 3 checks (the 16 px step and the >1 px "not a no-op" check) |
| 4 | `SelectionTargetFor`: clip selection first | "selection target" — 1 check (the both-set order) |
| 5 | `MoveMarkerCommand`: append instead of inserting in frame order | "move marker" — 4 checks (the reorder, its undo, the redo, the two-on-one-frame case) |

## Reopened: the first VM run of the merged branch (`feature/ux-pass`)

The integration run found two failures in this package. Both are dealt with
here, and the user guide's slip line (item H) is corrected with them.

**`TestClipStretch` (eight checks) — a real regression, and the code was
wrong.** The block Cmd-drags the clip's right edge to stretch it. Item H made
the slip zone accept `B_COMMAND_KEY` as well as `B_OPTION_KEY` (right — a PC's
Alt IS Command here), but the slip test sat BEFORE the corner/edge zones in
`HitTest`'s `zoneFor`, so a Cmd-press on the right edge resolved to
`Zone::Slip`: the stretch drag became a SLIP — every assertion failed (length,
ratio, source offset, the undo name). Master's order was only safe because
Option was the sole slip bit and Cmd fell through to the geometry.
Fixed by resolving the GRIPS first — corners (fade), then edges (trim/stretch),
then the body (slip with both Alt bits) — with a comment naming the trap, so
`Cmd`+right edge stretches again, `Cmd`+corner fades, and Alt in the body
still slips (the M2.1 cursor checks and the section-13 slip gesture cover that
side). The user guide's slip line now says "through a clip's middle", which is
what the rule is. This is the regression test: the VM's `TestClipStretch` and
the M2.1 cursor checks are the ones that catch it, and neither can be run here.

**`TestArrangeDepth:3302` (`!range()` before the range-drag section) — a
precondition that was racing the app_server.** Only one code path in the tree
sets a time range: `PreviewDrag`'s `RangeSelect` branch, i.e. a `B_MOUSE_MOVED`
while a range drag is live. The failing line ran after the section's focusing
click (`click(frameX(200000), laneMidA)`), which posts a DOWN and an UP with
nothing in between — so the move had to come from the app_server: the press
calls `SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS)`, so the server
also reports the REAL pointer to the view, and a report landing between the
press and the release is, to the view, an ordinary drag (it moved more than the
4 px threshold). That sets a range the click then legitimately keeps. The same
click cleared a range fine later in the same run (section 2's check passed),
which is what rules out the click's clearing behaviour. Two things were done:

1. The regression above left a range behind in the first place (its second
   gesture's press lands left of the clip once the clip is not stretched, which
   starts a range drag) — fixed with the rest.
2. The precondition is now a lock-guarded `ClearTimeRange()` with the failure
   mode written down, instead of an assertion about the click: section 2 checks
   "a bare click clears the range" on purpose, with a range set first, so no
   behaviour check is lost — the line was a duplicate that could only fail for a
   reason that has nothing to do with the section it guards. Section 2 is
   untouched.

**Considered and not done:** making the view ignore pointer reports whose
message does not say a button is down (which would kill the whole class). The
timeline's drag previews and the CLIP COMMITS depend on those reports, so a
wrong reading of the field — e.g. a `B_MOUSE_MOVED` that omits `buttons` — would
make every real drag silently do nothing, and this package has no VM to try it
on. If the race shows up again in a check that is about behaviour (section 2,
or a clip drag landing nowhere), that guard is the follow-up, with a VM run to
settle the field's presence.

**The suite's runtime**: this package's new checks add no long waits on a green
run — every `WaitFor` watches state a gesture in the same block sets within one
posted message (the marker drag's frame, the Delete, the rename commit, the
loop), so they return on the first poll; the only added sleeps are 0.4 s of
`snooze` around the two new clip sections' zoom change, and two `Shot()`s. The
300 s overrun is suite-wide (four packages, ~1900 lines, 19 shots); the failing
probes' timeouts are what inflate it, and this package's two failures are fixed
above rather than papered over.

## Not verified here

- **The VM half.** `tests/ui_functional_tests.cpp` was extended but could not
  be run on this machine (no VM access in this package — the orchestrator owns
  the VM run): the marker-flag drag (one undo step, "Move Marker"; the old
  right-click-deletes block became a direct `RemoveMarkerAt` call, precisely
  because posting the click would sit in the popup's `Go()`), the Delete rule
  with both paths, the clip rename field, and `L` with nothing to adopt
  adopting the playhead's bar. `scripts/haiku_syntax_check.sh` compiles the
  suite, so it build-checks; only its behaviour is unverified.
- **The screenshot pass** (UI_GUIDELINES §1): the empty-project hint, the ruler
  marker truncation, the in-place clip rename field, the marker drag line and
  the two new menus have not been looked at. They are the orchestrator's pass.
- The lane menu's Paste/Clear entries and every popup are eyes-only by nature:
  a popup cannot be driven by the suite.

## Findings that were wrong or moved

- **Item D's "no usable range" case is nearly unreachable**: `SnapBarBeats`
  defends a nonsense meter (`num <= 0` falls back to 4 beats), so only a tempo
  so fast that a bar rounds to zero frames hits the refusal — the test says so
  explicitly rather than pretending a broken meter reaches it. The refusal
  branch is still the honest one for the status line, and the ADOPTED bar is
  the case users will actually meet.
- **Item C's premise is right but its line numbers had drifted**: clicking a
  clip did not clear the range, and the range won — confirmed at the code
  (`RangeDelete` in `TimelineView.cpp`, `MSG_EDIT_DELETE` in `MainWindow.cpp`).
- **Item I's "MoveMarkerCommand if it exists"**: it did not exist; the new
  command is in `src/model/Commands.h`.
- **Item F's "New Audio/MIDI Track" needed no new message ids** —
  `MSG_NEW_AUDIO` / `MSG_NEW_MIDI` are already public in `MainWindow.h`, so
  `TimelineView.cpp` includes that header and posts them; the Edit menu was not
  touched at all.
- **The functional test's zoom is a trap** (found while adding the clip-rename
  check): TestArrangeFeel ends with a zoom-out, so at TestArrangeDepth the
  fixture clip is ~47 px wide and a click 12000 frames in lands on the 9 px
  trim grab. The new sections set a known zoom and put it back
  (`zoomForBodyClick` / `restoreZoom`).

## Commits

- `2180fac` feat(model): the arrangement's kit-free rules, and the marker move command
- `1758238` fix(ui): the arrange timeline's ranked UX defects (package 50)
- `98c990f` test(ui): drive the marker drag, both Delete paths and the in-place clip rename
- the docs commit that carries this record, the spec and the user guide's two fixes

Each is conventional, authored `Marc Korte <marc@duskaudio.com>`, and carries
no AI-attribution trailer.
