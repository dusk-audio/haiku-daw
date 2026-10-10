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

### The first VM run crashed `ui_functional_tests` — root cause and fix

ctest reported `ui_functional_tests (Signal 21)` and Haiku's error dialog
appeared. Signal 21 on Haiku is `SIGKILLTHR`, which is what dismissing that
dialog sends — the real fault was `BView`'s lock assertion:
`BLooper::check_lock()` (`src/kits/app/Looper.cpp:1412`) runs
`debugger("Looper must be locked.")` when the caller is not the looper's owner.

The caller was the new test: `TestArrangeFeel` called `tv->HitTest(...)` (which
walks lane geometry and so reaches `BView::Bounds()`) and `hbar->SetValue(0)`
from the test thread **without the window lock**. `LastTest.log` ends at
`test_arrange_feel` with no `FAIL` lines, i.e. the fault landed before the
test's first output — which is exactly where the unlocked `HitTest` was.

Fixed in `37e960a`: every view call in the test goes through lock-holding
helpers (`hitAt`, `frameX`) and the scrollbar's `SetValue` is locked too. The
rule is the one every other test in the file already follows; the section
breadcrumbs now print as the test runs so the next fault of this kind
localises in one run.

**Mutation check:** the pre-fix unlocked `HitTest` IS the mutation — that run
faulted the team (evidence: the `LastTest.log` above). With the lock in place
the same run is green.

### The second fault: the glyph cursors' bitmap had no owner

The re-run got past the lock bug (its output now reached `  arrange: tools`) and
then faulted again, inside the tools section. The kernel's log gave it exactly:

```
KERN: 73125: DEBUGGER: View method requires owner and doesn't have one.
KERN: stack trace, current PC ... _kern_debugger
KERN:   TimelineView::CursorObject(TimelineView::Pointer) + 0x523
KERN:   TimelineView::ApplyCursor(TimelineView::Pointer)
KERN:   TimelineView::UpdateHover(BPoint, uint32, uint32)
KERN:   TimelineView::SetTool(TimelineView::Tool)      <- KeyDown('5'), Mute
KERN:   TimelineView::KeyDown(char const*, int)
KERN:   TimelineView::MessageReceived(BMessage*)
```

This one was a **real defect in the feature, not the test**: the tool glyphs are
drawn into a 16×16 `BBitmap` so they can be cursors, and
`BBitmap(BRect, color_space)` defaults `acceptsViews` to **false**
(`Bitmap.h:52`). Without `B_BITMAP_ACCEPTS_VIEWS` the bitmap has no off-screen
window, `BBitmap::AddChild` silently does nothing (`Bitmap.cpp:886`), so the
drawing view has **no owner** — and the first `BView` call it makes trips
`_CheckOwnerLockAndSwitchCurrent()`'s assertion (`View.cpp:6870`). Dismissing
that dialog is `SIGKILLTHR`, i.e. ctest's "Signal 21".

Fixed in `9926399`: the bitmap is built with `acceptsViews`, locked **before**
`AddChild` (the order `BBitmap` requires), and the glyph cursor fails soft if
either step fails.

**Mutation check:** reverting `acceptsViews` to false reproduces the fault with
the same syslog line and stack; with it the suite passes.

That is also why the test now *applies* every glyph cursor (pencil … fade, plus
the slip cursor under Alt) instead of only mapping them through `CursorFor`: the
mapping is pure and never crashed, so only applying a cursor reaches
`CursorObject`. A fault is not something a `CHECK` can catch for the next
reader — the run has to reach the code.

### Handling a faulted run (VM hygiene)

A fault leaves Haiku's error dialog up, and every later run on that machine
hangs behind it (ctest reports `Signal 21`, which is the dialog's Terminate, not
the fault). The runs now take a screenshot before and after, clear the dialog
with `virsh -c qemu:///system qemu-monitor-command haiku-beta6 --hmp 'sendkey ret'`
(a keyboard event, not a settings change — the debug server's own settings are
never touched), and `rm -f` the recovery file. The lesson is also written into
`docs/HANDOFF.md`'s dev-loop section, with the syslog one-liner that prints the
assertion and the stack, so the next agent inherits it instead of the dialog.

### Numbers

| Check | Result |
|---|---|
| host `cmake --build build-host` | exit 0 |
| host `ctest --test-dir build-host` | 58/58 (baseline 56/56 + `snapgrid_tests` + `arrange_cmd_tests`) |
| ASan build + ctest | 58/58 |
| `sh scripts/haiku_syntax_check.sh` | 67 files, 0 FAIL |
| VM `build` ctest | 60/60 when `ui_functional_tests` passes; in one run it was 59/60 — that test's own big-project 5 s wait (see below) |
| VM `build-off` ctest | 55/55 |
| VM `ui_functional_tests` | 329 checks, **0 failures** — two direct runs and both screenshot passes (light and dark) |

### One pre-existing failure, seen once: TestPianoRollTransforms' Humanize

On one run the suite came back with a single red — `ui_functional_tests.cpp:604`,
the Humanize step of `TestPianoRollTransforms` (a package-04 test, not mine):

```cpp
    rv->RunMidiOp(MidiOp::Humanize);
    std::vector<MidiNote> before;
    CHECK(notes(&before));            // <- read AFTER the op
    ... CHECK(WaitFor(... !NotesEqual(h, before)));
```

`before` is read *after* the op, so it is a race: whichever side the read lands
on, the check needs the model to move again. Either mechanism is pre-existing
(and the seed is `system_time()`, so a draw that leaves every note in place is
also possible). It did not recur in the two runs after it, and none of this
package's code is on that path (`RunMidiOp`, `Humanize`, `MidiOps` are
untouched; the same check passed in every other run of the same tree). Reported,
not fixed: it belongs to package 04.

A second pre-existing red appeared once, in a `ctest` run: `TestBigProjectPlayback`'s
`WaitFor(playing(), 5000000)`. The test itself documents the number — "MEASURED
2026-10-09, VM: 3.1 s for this project. The plan's budget is 300 ms, and the fix
is M4.1" — so a 5 s wait is a marginal bound on this machine, and under ctest's
own load (and with one more track than the measurement had) it crosses. My
direct runs measured **3452 ms**, **3817 ms** and (before this package's fixtures
existed) 5061 ms on the same VM: it is load, not this branch — the same build
passed the suite directly in the same hold. Reported, not fixed: M4.1 owns it.

### A trap in our own tooling, found through the mutation driver

`sh scripts/vm.sh build` (and `test`) ended in `... | tail -3`, so a FAILED
compile exited 0 and left the previous binaries in place — the entry doc's
"check the build's exit code, not only ctest" warning, defeated by the script
itself. It cost one mutation cycle that silently ran a stale binary. Fixed on
this branch (standalone commit): the build/test output goes to a log, which is
tailed afterwards, and the remote command `exit`s with the real status, so ssh
propagates it. The lesson is in `docs/HANDOFF.md` beside the crash-dialog one.
The VM proof (a deliberate `#error` in a leaf source file, then
`sh scripts/vm.sh build` must exit non-zero) was queued as one lock hold and is
recorded in the commit that follows it; at the time of writing the shared lock
was held by another agent's pass.

### One tooling mistake worth recording

The first screenshot attempt ran `ctest` and the `build-off` build on the VM
checkout the mutation cycle had left behind (the driver syncs its mutated tree
and does not sync back), so that build compiled the mutation and failed. The
final run re-syncs the branch after the cycle and before anything else measures
the tree. The same class of mistake as the leaked commit above: a mutation run
owns the VM checkout as well as the worktree.

## Mutation checks

Host (each applied, watched to fail, reverted, named in `c6255d9`):

- `SnapGrid.h`: triplet multiplier 1.5 → 1.0 — 4 snap checks failed.
- `Commands.cpp` (`JoinableSuccessor`): the gap refusal bypassed — 4 failed.
- `Commands.cpp` (`JoinClipsCommand::Do`): the seam fade-out left in place — 1.
- `Commands.cpp` (`JoinMidiClipsCommand::Do`): the rebase delta zeroed — 2.

VM, one mutation per suite run (applied, watched to fail, reverted; driven by a
throwaway commit object so the branch is untouched). What each run reported:

| mutation | result |
|---|---|
| `SetTool` never changes | 25 failures: the tool-selection checks and every gesture that depends on the tool |
| `Snapped` ignores the grid | 6: the pencil's snapped start/length, both snap-indicator seeks |
| `HitTest` ignores the caller's modifiers | exactly the Gain cursor check (`cursorAt(audioBody, B_CONTROL_KEY, 0) == Pointer::Gain`, 1 failure). The Slip check passes by design: the Body zone derives slip from the caller's modifiers either way |
| the Ctrl+wheel direction flipped | exactly the two zoom-direction checks |
| `ScrollToFrame` clamps early | exactly the three scroll-past-the-end checks |
| the roll does not follow | exactly the two follow checks |

(Two of those cycles ran against a tree I was editing at the time, so their
failure sets also carry reds from the test's *own* bug — the MIDI-region cursor
mistake below.)

**The `hit-test-mods` mutation took four attempts to get right**, and the story
is worth keeping: the first replaced only one of the five call sites (the
scissors branch, which no check can tell apart); the second inserted its
`// MUTATION` comment mid-expression so the file did not compile, and `vm.sh
build` ended in a pipe to `tail`, so the FAILED compile exited 0 and left the
previous binary in place (fixed on this branch, see below); the third was the
same single-site replace with a compiling form; the fourth, with the driver
replacing every site and checking the build's real exit code, failed exactly the
check it should — and that run's build really was the mutated one (`BUILD=0`
with the mutation in the tree, `[9 site(s)]` reported).

### An accident worth recording: a mutation leaked into a commit

`5c466dd` captured a mutation: the driver above applies a mutation to the
working tree and restores it afterwards, and that commit was made in the same
worktree while the driver was mid-cycle, so `git add -A` staged the mutated
`SetTool` (`return;   // MUTATION: the tool never changes`). Found by reading the
branch back (`git grep -c MUTATION HEAD`), fixed in `d3b89a6` — a new commit
rather than a rewrite, per this repo's rules. The rule that came out of it: a
mutation-check run owns the worktree until it prints its summary; do not commit
from there while one is running.

## The failures the first diagnostic run found, and what each fix was

The first full run (319 checks) came back with 16 failures and no fault. Three
were real:

- `HitTest` read the modifiers from the window's **current message**, so
  `CursorFor(where, B_CONTROL_KEY)` could not ask for the Ctrl zone and answered
  `Move` where the caller wanted `Gain`/`Slip` (`7a24567`). The zone depends on
  the modifiers, so they are a parameter now.
- The test focused the timeline with a **lone mouse-down**, leaving a
  rubber-band gesture live; the next click's mouse-up committed a band-select
  over a zero-height rect, selecting a clip nobody meant — and split-at-playhead
  then honoured that selection and did nothing, which cascaded into six more
  checks. It clicks this track's empty lane now (a full click) and asserts the
  selection is empty before going on.
- The Ctrl+wheel check asserted the **wrong direction** (Haiku's wheel delta is
  positive rolling down, so wheel-up zooms in). Both directions are driven.

The rest of the fixes were hygiene: the scrollbar check sends the thumb to the
far end before dragging it back (SetValue ignores a value it is already at) and
prints the values; the test restores the pointer tool, the horizontal scroll and
the dock's open/shut state, because the NEXT test measures this window.

## Screenshots

Two passes, `DAW_UI_THEME=light` and `DAW_UI_THEME=dark` (40 shots each; the
suite green in both, `ui_functional_tests: 329 checks, 0 failures`). Shots I
opened and checked, against `docs/UI_GUIDELINES.md` §3:

**Light** — `00-startup`, `13-playing`, `14-theme-150`, `15-arrange-palette`,
`16-arrange-hover`, `17-arrange-clips`, `18-arrange-fade`, `19-arrange-snap`,
`20-arrange-scrollbars`, `21-arrange-roll-follow`, plus 1:1 crops of the tool
strip, the clip's left edge and both scrollbars.

**Dark** — `00-startup`, `13-playing`, `14-theme-150`, `15-arrange-palette`,
`20-arrange-scrollbars`, `37-opened-project`.

What the shots show, and what I checked:

- The **tool strip** fits at 100 % and at 150 % in both modes: six buttons, the
  separators, the snap field and the zoom pair, nothing clipped, nothing
  overlapping, the strip's height fixed (it does not grow with the window).
- The **snap indicator** reads at a glance: `1/16` / `1/4` on the lit field,
  `Off` dim (shots 15-19).
- The **hover highlight** is a clearly visible bright bar on the clip's left
  edge (crop of shot 16) — legible against the dark clip fill in both modes.
- The **scrollbars** are stock-drawn and their thumbs track the scroll: measured
  on the pixels, the thumb's 78 % span sits at the LEFT with the view at frame 0
  (shot 21) and at the RIGHT with the view scrolled to the content end (shot 20),
  which is the "scroll past the end of the last clip" state — the ruler shows
  bars 3-9 with the lanes empty.
- The **docked piano roll** scrolled to the playhead (shot 21: "Piano Roll —
  arrange" with the tapehead in view).
- No dead bands, no labels against an edge, no stale panes after Open (shot 37).

**Nothing had to be fixed from this pass.** One thing I chased and cleared: in
the dark pass the *panel backgrounds* (inspector, dock, transport bar) are light
while the custom-drawn chrome is dark. That is exactly what T1's and T2's
accepted dark shots look like (`docs/agent-prompts/shots-25/dark`,
`shots-26/dark`), so it is the established dark look, not a regression from this
branch.

Both passes were re-run after the branch's last code commit and diffed shot by
shot against the ones reviewed above. **Every shot that shows this package's
change is identical except the desktop clock** (a 125x12 px corner): the seven
`arrange-*` shots plus `theme-150`, in both modes. (Some of the OTHER tests'
shots differ between runs for their own reasons — the humanize transform draws
from `system_time()`, alerts open where the pointer is — which is pre-existing
per-run variance, not a rendering difference.)

**What I could not verify on the VM:** the cursor *glyphs* as the user sees them
(there is no interactive mouse; the mapping is asserted through `CursorFor` and
the glyph-cursor path — the bitmap drawing that once faulted — is applied by the
test on the real target), and the tooltip pop-ups (Haiku shows them from the
tooltip manager on a real hover+idle; the suite sets them, and the shots cannot
show that). Both are on the click list for the desktop.

`TestArrangeFeel` leaves its two fixture tracks in the project (the next test,
`TestBigProjectPlayback`, measures its play start against that project). Removing
them at the end of the test is a change I did not get verified on the VM — the
shared lock was taken by another agent's pass — so it is not in this revision;
the number that test prints is inside its own bound either way (see above).
