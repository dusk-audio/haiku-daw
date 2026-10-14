# Record — package 51: the piano roll's UX defects (A–J)

Branch `fix/ux-pianoroll`, off `master` (0783e5e). Spec:
`docs/agent-prompts/51-ux-pianoroll.md`. Scope: the ranked UX-review findings
A–J, A–C first (they are integrity bugs, not cosmetics).

Commits: see the branch log; every one is `marc@duskaudio.com`, conventional,
no attribution trailer.

## What landed

### A/B — the roll no longer writes what it did not read

The bug, twice over. The roll edits a snapshot and posts its WHOLE note list;
nothing ever pushed the model back, so an undo left the roll showing the move
the undo had just reverted and the next gesture committed that stale list as a
fresh edit. And a region dragged in the arrangement
(`MoveMidiClipCommand` keeps the id and rewrites only `startFrame`, posting
`kMsgReloadEngine`) or split (`S`) left the roll holding the open-time
`fClipStart`/`fClipLen`, so the next gesture wrote the whole part shifted and a
split grew the half back to the old length.

- **One push, one shape.** `kMsgRollRegion` (PianoRoll.h) carries the region's
  current state — track, clip, start, length, the note and event arrays in the
  same layout the apply messages use, `serial`, `gone` — to every open roll:
  the docked view through a direct `SyncRegion` call (same looper) and the
  popped-out window through its messenger. Sent by `RefreshRolls()` from
  `MSG_EDIT_UNDO`, `MSG_EDIT_REDO`, every `kMsgApply*`, `kMsgUiRefresh`,
  `kMsgReloadEngine` (the region drag's only message), a roll's own open, and
  the 2 Hz `MSG_TITLE` poll as the backstop for edits the window never hears
  about (the timeline commits straight from a gesture).
- **The view decides with the kit-free rule** (`RollEdit.h: DecideRollSync`):
  content or length changed → Reload (replace the snapshot, clear the
  selection, cancel an in-flight drag — an uncommitted gesture loses to the
  model); only the start moved → Repoint (notes are clip-relative, so the
  selection survives a move made in the arrangement); the region is gone →
  Close (the dock empties, the popped-out window quits); otherwise Keep.
- **The commit gate.** Every commit names the stack serial it was based on
  (`base`), and a commit is applied only when `RollCommitAllowed` says the
  model is still the state the roll saw. Three ways that can be true:
  1. the base IS the stack's serial;
  2. the stack is still exactly where the roll's own last accepted commit left
     it — two gestures posted inside one turn of the window's loop both name
     the same base (the push that would raise it is sent while the first is
     handled), and refusing the second would drop the user's gesture;
  3. the region's own fingerprint (`RollRegionSignature`, FNV-1a over start,
     length, notes and events) still matches what the window last pushed to
     that roll. An edit ELSEWHERE (a fader drag, a track rename, a freeze job,
     a region edit the timeline commits without posting anything) moves the
     serial and leaves this region untouched; refusing the user's note for that
     would be a false refusal, and this is exactly the case the 2 Hz poll
     cannot close in time.
  Anything else — the region really did change under the roll — is refused, and
  the refusal's push is what re-reads it. A roll that was never pushed (only
  tests build one) carries no `base` and keeps the pre-51 behaviour.
### C — follow-playhead

`PianoRollView::SetFollow` replaces the hard-coded `fFollow = true`;
`MainWindow::PushRollFollow` fans the View menu's one mark out to the docked
view and the registered window, at toggle time and at open time. The menu's
mark, the timeline and every roll read one policy.

### D — audition

- `Engine::AuditionNote` — a small lock-free `MidiEventRing` in the
  Engine, drained on the RT thread into the existing voice pool
  (`Graph::TakeVoice`/`ReleaseVoice`/`DrainAudition`), rendered through the
  target track's bus, so a click sounds through that region's instrument, fader
  and FX — not a bare synth blip. An audition voice is marked, so switching
  keyboard monitoring off does not release it, and its bus exists even when the
  region has no notes (`BuildRequest::auditionTrack`, snapshotted into the Graph
  at build so the bus set and the RT routing cannot disagree).
- `Engine::AuditionNotesRendered()` counts what the callback actually rendered:
  the path's end-to-end proof, and the regression handle for the bug the review
  found (see the review section).
- The roll asks (`kMsgRollAudition`) on a keyboard-column press/release, a
  drawn note (pencil, brush) and a velocity-lollipop click; `MainWindow`
  decides. When the transport is idle and nothing is armed, the window raises
  the monitor-only engine for the clicks and stops it 3 s after the last one.
- **The mute switch is in Preferences** (`previewNotes`, "Preview notes when
  editing MIDI", persisted as `audprev`), honoured in `EnsureAuditionEngine`,
  so the roll always asks and the window answers.

### E — the snap grid

The roll carries a `SnapGrid` (default 1/16 — exactly what the welded
`kSnapDiv = 4` did) and snaps through `RollEdit.h: SnapClipRelative` (absolute
frames, the song's grid, back to clip-relative). The toolbar has the same field
the arrangement has — its label IS the active grid, lit while snapping is on —
opening the same menu (bar, 1/2 … 1/32, Triplets, Off) from the same kit-free
table, on the same key (`g`). Shift stays the free-placement override, and the
nudge/draw steps follow the selected grid (a triplet grid nudges a triplet
step).

### F — CC points drag instead of painting

Mouse-down in the lane resolves the point under the cursor in BOTH axes
(`CcPointNearFrame` with the 7 px grab radius in frames) and drags THAT point
(`CcPointMove`, the index is the drag's identity); only empty space paints a
ramp. A point moved onto a frame that already holds one of the same stream
absorbs it. The lane menu gained **Clear lane** (one undo step, disabled when
the lane is empty).

### G — the lane menu, and a bend lane

- `MidiControl.h: CcVoiceEffect` is the model's own table of what a controller
  does to a voice (CC1 mod-wheel vibrato, CC7 volume, CC10 pan, CC11
  expression, CC64 sustain); the lane menu's labels are built from it, so a
  controller that does nothing reads "no effect yet" from the same source the
  engine's evaluators come from.
- **The bend lane landed**: the lane address is now −1 velocity, −2 pitch bend,
  ≥0 CC (`LaneIsValid`, `LaneValueMax` 16383 for a bend, `LaneValueDefault`
  8192 centred), the menu offers "Pitch Bend" and the staircase/handles draw it
  with the same code the CC lanes use.

### H — editing feel

Cmd-C/X/V (paste at the playhead when it is inside the region, else at the
region head), Cmd-D (duplicate one grid step on), Alt-drag (duplicate the
selection and drag the copies), the left-edge resize (start moves, the note's
end stays; the right edge keeps its old behaviour), cursors (`BCursor` — grab
on a note body, east-west on either edge, cross-hair for pencil/brush and the
keyboard column), arrows nudging a selection by a grid step (scrolling stays
the fallback with nothing selected) with Up/Down transposing, `+`/`-` through
`ZoomBy` (anchored on the playhead in view, as the toolbar's buttons already
were), and `f` fits the region.

### I — the region's bounds

Drawn as two vertical edges at `FrameToX(0)`/`FrameToX(fClipLen)`, with notes
the region does not play drawn dimmed — `RollEdit.h: NoteInRegion` is now the
ONE statement of that window rule, and `MidiOps.cpp`'s transform guard reads it
too, so what is dimmed is exactly what is silent.

### J — theme scale, tokens, toolbar, title

Every metric in the view goes through `Themed()` (`KbdW/RowH/LaneH/ToolbarH`
accessors), the bottom lane's literal `Rgb(18,20,25)` became the theme's well
(`ColLcd()`), the lane's staircase/handles/lollipops take `ColMidiAccent`/
`ColAccent`/`LabelOn` (which read on both the dark well and System mode's
document-coloured one), the note selection outline is `LabelOn(fill)`, and the
velocity heat map moved into `Theme.h` beside `MeterColor` (data-to-colour, the
same in both modes). The toolbar lays itself out from the theme scale and the
view's own width (`RollEdit.h: RollToolbarFor`), shrinking the MIDI button —
and truncating its label — rather than hanging off a narrow dock's edge. The
pop-out is zoomable (`B_NOT_ZOOMABLE` dropped), sized from `Themed()`, and
titled "Piano Roll — <track>".

### MainWindow's Edit-menu routing (part of H)

The docked roll is a VIEW of the main window, so the menu bar claims Cmd-C/X/V/
D/A/F before any view sees them. With the docked roll focused, the Edit menu's
Cut/Copy/Paste/Duplicate/Select All and View ▸ Zoom to Fit are routed to it
(`FocusedRoll()`), so Cmd-C in the editor copies NOTES rather than silently
copying the arrangement's clips. (A popped-out roll is its own window with no
menu bar, so its `KeyDown` handles them itself.)

### Cmd-Z with the docked roll focused — verified, not a bug

The brief suspected it might do nothing. Haiku's `BWindow::_HandleKeyDown`
offers Cmd-modified keys to the window's own menu bar before the focus view
(`haiku/src/kits/interface/Window.cpp:3681`, read in the local source tree), and
the docked roll belongs to the window whose menu owns Cmd-Z — so it always
worked. The popped-out window keeps forwarding it through `ForwardUndoRedo`.
What did NOT work was what Cmd-Z did to the roll's snapshot: that is item A.

## Verification

1. `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build build-host -j2`
   → **exit 0**.
2. `ctest --test-dir build-host` → **100% tests passed out of 76**
   (was 75 + the new `rolledit_tests`).
3. `sh scripts/haiku_syntax_check.sh` → **83 OK, 4 FAIL**, and the four are the
   pre-existing codec files (`FlacSink/FlacSource/VorbisSink/VorbisSource`,
   missing FLAC/Ogg headers on this host). `src/ui/PianoRoll.cpp`,
   `src/ui/MainWindow.cpp`, `src/engine/Engine.cpp`,
   `tests/ui_functional_tests.cpp` and everything else compile clean under the
   Haiku headers.

`tests/rolledit_tests.cpp` — **172 checks**: the sync decision (Keep/Repoint/
Reload/Close and the ordering between them), the commit gate (fresh, batched,
region-keyed, broken by an undo, re-synced), the region fingerprint (every
field it covers), the region window rule, snap-in-region (16ths, triplets, Off,
an off-grid region head, the clamp at the head), the lane addressing (bend
included), the lane fraction round trip, the CC hit/move/absorb/clear rules
(including a drag that absorbs a point and keeps hold of its own), nudge from a
captured origin, copy/append selection, and the toolbar layout.

## Mutation checks (break it, watch it fail, restore)

| Mutation | Caught by (rolledit_tests) |
|---|---|
| `DecideRollSync` ignores the region length | 73 (2 checks) |
| `DecideRollSync` returns Keep on note differences | 67 (6) |
| `RollCommitCurrent` accepts any serial | 110 (6) |
| `RollCommitAllowed` ignores the own-transition base | 127 (1) |
| `RollCommitAccepted` records nothing | 124 (2) |
| `NoteInRegion` uses `<=` (the end frame sounds) | 142 (2) |
| `SnapClipRelative` drops the region's origin | 160 (10) |
| `CcPointMove` leaves the frame alone | 245 (5) |
| The CC hit test has an unbounded radius | 235 (2) |
| `LaneValueAtFraction` does not clamp | 224 (2) |
| The toolbar layout always keeps MIDI wide | 369 (3) |
| `EventInLane` stops matching bend events | 205 (7) |
| `CopySelection` ignores its offset | 343 (2) |
| `RollRegionSignature` ignores the notes | 151 (1) |
| `RollRegionSignature` ignores the length | 150 (1) |
| `RollCommitAllowed` drops the region identity | 134 (2) |
| `CcPointMove` does not report the shifted index | 318 (5) |


One of these was a REAL bug found while writing the tests, not a deliberate
mutation: `CcPointMove`'s first cut erased an absorbed point without adjusting
its own index, then wrote through the stale one — `rolledit_tests` aborted on
the out-of-range access before the fix (the index-tracking is now commented
where it bit).

## Haiku-only halves (the VM suite), `tests/ui_functional_tests.cpp`

`TestRollModelSync` (package 51, ~130 lines) drives the DOCKED editor through
the real messages: it opens one, drags a note, **undoes** and requires the roll
to show the model's state again; draws a note on the 1/16 grid and then on the
quarter grid at the same x and requires each to land where
`SnapClipRelative` says; drags a CC point and requires the model to still hold
exactly two points with one at neither original frame (no staircase); moves the
region with `MoveMidiClipCommand` + `kMsgReloadEngine` and requires the roll to
re-point, then splits it and requires the half's length to SURVIVE the next
gesture; toggles Follow both ways and requires the view and the menu mark to
agree; clicks the keyboard column and requires the ENGINE to render an
  audition note (`AuditionNotesRendered`); and removes the region and requires
  the editor to be gone. It also drives the clipboard keys through the real key
  path (Cmd-C must copy in the docked editor — the Edit item has to be ENABLED
  for Haiku to deliver the shortcut at all), and both Alt gestures (a click
  toggles the selection and commits nothing, a drag duplicates and one undo
  takes it back). Eleven shots: `roll-after-undo`, `roll-grid-16th`,
  `roll-grid-quarter`, `roll-cc-lane`, `roll-cc-drag`, `roll-clipboard`,
  `roll-alt-click`, `roll-alt-drag`, `roll-region-moved`, `roll-after-split`,
  `roll-audition`, `roll-region-gone`.

## Adversarial review pass (before this record was written)

Two read-only review agents went over the diff — the engine audition path and
the roll/window sync protocol — because none of this code can be run here. The
engine review found five real defects, all fixed:

1. **`Bus`'s hand-written move constructor and move-assignment did not carry the
   new `auditionNotes` member.** The graph build reserves the bus's voice share
   and then `push_back(std::move(b))` — a move-CONSTRUCT — so the bus arrived
   with capacity 0, the RT hand-off's `size() < capacity()` guard dropped every
   note, and the whole feature was silently dead (no crash, no sound). Both
   moves carry it now, and `AuditionNotesRendered` plus the new VM check exist
   so this exact class of bug cannot pass again.
2. **`StopAuditionEngine` stopped the engine unconditionally**, and a play, a
   take and the armed-track monitor are all the same engine object — unchecking
   "Preview notes" while the transport rolled would have killed playback with
   the play lamp still lit. It returns when the transport owns the engine.
3. **The "already up" test proved a timer existed, not that the player was
   running**: a click inside the 3 s window after the engine had been stopped
   elsewhere was swallowed for ever. It asks `Engine::PlayerRunning()` now.
4. **A key held on the keyboard column longer than the idle timeout** would have
   had its voice reset mid-note by the idle stop. Held notes are counted; the
   timer re-arms while one is down, and the views release them when they go.
5. **The bus set (build-time) and the routing (live atomic) could disagree**,
   dropping a click on a track whose bus only exists for the audition. The
   target is snapshotted into the Graph and the RT routes by that.

The engine review also checked and cleared: RT safety of the new ring, vectors
and helpers; the refactor's equivalence with the old `UpdateLiveVoices` body
(ordering, the CC64 path, the note-off match, the bend phase, the reap rules);
and voice lifetime in both render paths.

The sync-protocol review found eight more, all fixed:

1. **Only one popped-out editor was tracked, and a second pop-out did not close
   the first** — the orphan kept its own stale base, so every commit it made
   after the first was refused and silently dropped. Opening a pop-out quits
   the one registered before it (the fresh-messenger rule the other
   single-instance windows use).
2. **Cmd-C/V/X/D did nothing in the docked editor.** Haiku skips the shortcut
   of a *disabled* menu item but still swallows the key
   (`Window.cpp:3810-3847`), and `UpdateEditMenu` enabled the Edit items only
   from the arrangement's selection. It counts the focused roll's selection and
   clipboard now — `Cancel`/rubber bands in the arrangement are unaffected.
3. **The same keys did nothing in the popped-out window**, whose `KeyDown`
   never sees them: `BWindow::_HandleKeyDown` ends with "we always eat the event
   if the command key was pressed". `PianoRoll::DispatchMessage` intercepts
   them, the path Cmd-Z already used. (This corrects the speculation in the
   Cmd-Z paragraph above: the win's Cmd-Z works because of the forwarding, not
   because its `KeyDown` runs.)
4. **A stale base meant losing the user's gesture**, and the window's own
   `fStack->Execute` sites (the master slider's per-tick message, renames, the
   sends/instrument/fx paths, autosave's `FlushFxEditors`, the timeline's
   context-menu region Delete/Split, the region jobs) bump the serial with no
   push at all — up to the 500 ms poll of false refusals. The region
   fingerprint (above) makes the refusal *exact*: only a change to this region
   refuses a commit.
5. **A CC drag onto a neighbour's frame left the drag holding the wrong index**
   (the absorbed point shifts the vector): the next mouse-move dragged the
   neighbour, or ran off the end and froze. `CcPointMove` hands the new index
   back; the host test now calls it twice on the same drag.
6. **Alt-drag did not duplicate** (and, after the fix, a re-read of the diff
   caught the toggle itself firing on an already-cleared index — the two Alt
   gestures now have VM checks because reading it twice was not enough) — the
   Alt bits were this view's additive-
   select modifier, and on a PC keyboard Alt *is* `B_COMMAND_KEY`. Alt on a
   note now resolves by what the pointer does: a drag duplicates (the copies
   are made on the first real movement), a click toggles the selection. Both
   documented gestures, on any keyboard.
7. **A bare click on a note committed an unchanged list** — an "Undo Edit
   Notes" step that undid to the same notes, one per click. A gesture that
   changed nothing no longer commits (the pencil's new note still does, even
   when the drag sizes nothing).
8. **`ClearDockedEditor` deleted the docked view without releasing a key it was
   holding** (and zeroing the shared held-note counter could under-count a key
   the pop-out still held). The view releases it on the way out.

One design change came out of writing the gate's own tests rather than either
review: `RollCommitGate` — the first cut refused a second gesture posted inside
one turn of the window's loop.

## New keys and gestures (documented in the header and USER_GUIDE)

`1`–`7` tools · `q` quantize · `g` the grid menu · `f` fit the region ·
`+`/`-` zoom about the playhead · arrows nudge a selection (or scroll) ·
Up/Down transpose · `Del`/`Backspace` delete · Cmd-A select all ·
Cmd-C/X/V/D clipboard · Shift = free placement (no snapping) ·
Alt-click toggles a note in the selection · Alt-drag duplicates it under the
pointer · `=`/`-` (and the toolbar's field) as the arrangement uses them.

## Reopen — the integration run's 15 failures in `TestRollModelSync`

The first integrated VM run reached this test for the first time and 15 of its
checks failed. All of them were the TEST's, none the app's, and the root cause
was one line of Haiku behaviour worth writing down:

- **A posted mouse message must carry `screen_where`.** `BWindow::DispatchMessage`
  converts a mouse message's SCREEN point into the view-local
  `be:view_where` that `BView::MessageReceived` actually reads
  (`haiku/src/kits/interface/Window.cpp:3490-3512`); a message carrying only
  `"where"` is not converted at all, so the view's `MouseDown` receives
  **(0, 0)** — the toolbar's corner. Every synthetic click in the test landed
  there: the drag never grabbed a note, the pencil never drew, the CC drag never
  started, the Alt gestures never fired and the keyboard column never sounded.
  (The suite's older click helpers post `screen_where` for exactly this reason.)
  Fixed: one `click()` helper posts both points, converting through
  `BView::ConvertToScreen`, and every mouse interaction goes through it.
- **A key posted with the window's own token is not a focus message.**
  `BWindow::_IsFocusMessage` requires `B_PREFERRED_TOKEN`
  (`headers/private/app/MessagePrivate.h:131`), so `BMessenger(win)` never runs
  the menu's shortcut path and the key is dropped in `BWindow::MessageReceived`.
  The Cmd-C/Cmd-V checks post through the preferred handler now
  (`BMessenger((const BHandler*)nullptr, win)`), which is what a real key event
  carries — and that is the path the Edit menu's shortcut actually takes.
  (Credit: the mixer package root-caused this one and shared it.)
- **The Edit item's enabled state was read before the 2 Hz poll ran**, so the
  `copyEnabled` check raced its own setup: the test calls `UpdateEditMenu()`
  itself now.
- Two aiming bugs found in the same pass, fixed in the test: the docked pane is
  ~236 px tall (the split's height persists, and the default is `Themed(260)`),
  so a pitch row 36 semitones below the toolbar is *below the velocity lane* —
  the notes now sit high (90/94) and every click is aimed from the view's own
  metrics (`XForFrameForTest`, `LaneValueYForTest`, the row metric), with each
  aim CHECKed and printed so a geometry change fails at the aim; and the
  transport is parked at 0 before the editor opens, because the view centres
  itself on a playhead inside the region.

One **app** change came out of the same investigation, small and inside this
package's hosting: the docked editor's own keys. `MainWindow::DispatchMessage`
hands the roll its editor keys (`PianoRoll.h: RollEditorKey` — the palette 1-7,
`q`, `g`, `f`, the zoom pair, the nudge arrows, Del) while it holds the focus,
before the timeline's key rule. The roll draws its own keyboard and its keys are
deliberately NOT in the arrange's one map (`model/KeyMap.h` says so), but the
docked roll is a view of the main window — and the integration's new routing
(`TimelineTakesKey`) otherwise hands 1-6, `g`, the arrows and Delete to the
arrangement, which would leave the editor's tools, its grid menu, the nudge and
Delete dead while docked. Keys the roll does not use (Space, `R`, Home ...) are
untouched.

## Reopen, second round — five failures left, one root cause

The next VM run printed what the first fix's diagnostics were there for:
`roll view: 900x200 at scroll 0` and `AIM MISS: (54.8, 106.0) view 900x200
laneTop 136`. The miss was on the **x**, not the y: the test aimed at a note's
`start + 100` frames, which for a note at frame 0 is 0.8 px right of the
keyboard column -- inside the note, but inside its five-pixel LEFT-EDGE grab
zone, so the "move" became a start-resize (leaving a one-frame note). The Alt
checks fell over on the same aim from the other side: a one-frame note cannot
be hit at all, so the Alt-click fell through to the pencil and drew a note
instead of toggling the selection.

Fixed, all in the test:

- every interaction grabs the **widest** note at the **middle** of its body
  (`widestNote()` + `start + length/2`), which is far from both edge zones at
  any zoom; the grab is printed;
- the test's rows moved to the TOP of the usable band (pitches 92-95, the
  audition key at 96), because the pane's height is persisted in the settings
  file and the VM has had 200, 236 and 260 px panes across runs. The top rows
  are inside `[toolbar, velocity lane)` for any pane taller than ~150 px and the
  row CHECKs print the height if one is not -- the test reads the pane it
  actually has rather than assuming a height (and does not write to the user's
  settings to force one);
- the post-split check stopped comparing against the pre-split length (a drawn
  note's tail can legitimately reach it again, which is the notes command's
  documented "only ever grows" rule) and asserts the invariant the bug actually
  broke: **the region covers its own notes and nothing else**.

The pane's own numbers, worked through for the failing run's 200 px pane:
rows 34/46/58/70/82 (all < `laneTop` 136), the two drag aims at x 72.8 and
194.6 with both note edges > 5 px away, and the CC aim's y at 178.6 inside the
lane -- every one inside the pane and in the intended zone.

## Reopen, third round — the Alt pair, and the lesson it shares

Two checks left, both Alt gestures. The aim was right by then, so the failure
was in the gesture itself -- and it was an APP bug of the same family as the two
test-side ones found before it: **the roll read the wrong modifiers.**
`BView`'s global `modifiers()` asks the input server what is held *right now*;
it is neither the modifiers the gesture was made with nor something a posted
message can set, so a synthetic (and, under load, a real) Alt-press reported no
Alt: `MouseDown` took the plain-click path (`SelectOnly` collapsed the selection
to the note clicked) and the duplicate never ran. Every other view here reads
the *event's* own modifiers for exactly this reason
(`UiMetrics.h: EventModifiers`, which is what the arrangement's Alt gestures use
-- and why those are driveable and testable while this one was not).
`PianoRollView::EventMods()` is now the roll's one reader: the Alt pair, the
Ctrl-drag velocity gesture, `KeyDown`'s command check and `Snapped()`'s Shift
free-placement. Real input is unchanged (app-server events carry their own
modifiers) and an event without the field still falls back to the live value.

This is the third instance of one lesson in this reopen, worth stating plainly
for the next person writing a test against this UI: **the kit ignores what a
posted message does not carry.** A mouse message needs `screen_where` (or the
view sees (0, 0)); a key needs the preferred token (or the focus view never gets
it); a modifier has to be on the event (or the gesture does not exist). All
three now have a working precedent in this suite.

## Shots to look for (the orchestrator's screenshot pass)

`ui_functional_tests` under `DAW_UI_SHOTS`, both theme modes, and a 150 % run
(the roll's geometry moved):

- **`docked-editor` / `popped-out-editor`** — the toolbar now carries the grid
  field; every button must be inside the pane (the MIDI button shrinks rather
  than hanging off the right edge). The pop-out's title must read
  "Piano Roll — <track>".
- **`roll-after-undo`** — the note sits at its pre-drag position in the grid
  AND in the region's row of the timeline behind it.
- **`roll-clipboard`** — after Cmd-C/Cmd-V in the docked editor the pasted note
  is visible next to the playhead; the Edit menu's Copy must read ENABLED with
  a note selected in the editor and nothing selected in the arrangement.
- **`roll-grid-16th` / `roll-grid-quarter`** — the grid field's label reads
  "1/16" then "1/4", and the new notes land on different x.
- **`roll-cc-lane` / `roll-cc-drag`** — the lane's staircase and handles, the
  faint default-value line, and (after the drag) TWO points with no staircase
  behind them.
- **`roll-region-moved` / `roll-after-split`** — the region's edges drawn as
  two vertical lines, notes outside dimmed, and the whole-part offset gone.
- **`pianoroll-cc-lane` / `pianoroll-cc-point` / `pianoroll-window`**
  (pre-existing shots) — the lane's button caption, the window title, the
  bottom lane's colour in BOTH modes (System mode's well is the user's document
  colour now, not a black band).
- The keyboard column, the region edges and the dimmed notes in dark mode: the
  keys stay white/black by design (they are the instrument, like the velocity
  heat map); everything around them is a token.
- **`roll-audition`** — the shot comes after a keyboard-column click that the
  engine actually rendered; the pump/keyboard must look pressed-and-drawn
  rather than blank.

## What is left undone, and what could not be verified

- **Nothing from A–J is left undone.** G's bend lane landed; D's mute switch is
  in Preferences.
- **The audition is the least verified piece.** The engine side (the ring, the
  voice path, the bus rule) compiles under the Haiku headers, was reviewed line
  by line and now has a check that will run on the VM, but **it was not run
  here**: no VM and no audio device on this host. The VM check proves the note
  reaches the instrument render; it cannot prove it reaches the speakers.
  Marc's click list: open a region, click the keyboard column (it must sound
  through that track's instrument — change the track's instrument/FX and hear
  it), draw a note (a short blip), click a velocity lollipop; then Preferences ▸
  "Preview notes when editing MIDI" off and confirm silence, and press Play
  after a click to confirm the engine hands over without a hiccup. Clicking on
  an armed MIDI track while its input monitor is up is the case that only
  rebuilds the monitor graph (`RecordController::UpdateMidiMonitor`).
- The **2 Hz poll** is the backstop for model changes that reach the window
  through no message at all; it is gated on the stack serial, so an idle session
  does no work beyond one comparison per tick. A change made *without* going
  through `CommandStack` (nothing in the tree does that from the UI) would be
  picked up by the poll, but a roll's commit could then race it — the same
  refusal path as any stale commit.
- The **piano keyboard column's key colours are deliberately literal** (white
  and black keys in both modes): a piano keyboard is not panel chrome, and the
  dark theme's keys have always been drawn this way. Everything else in the
  view reads a token.
- `fClipboard` is per-view (a popped-out roll and a docked one do not share
  notes); the arrangement's clipboard is clips and stays separate on purpose.
- The `kMsgRollOpened` rule is "one popped-out editor": popping a second region
  out closes the first window. Nothing in the UI offers two side by side (the
  dock hosts the other one).
- The Edit menu's Cmd-C/V/X/D routing is per-FOCUSED-view: with the docked
  editor focused the notes win, with the timeline focused the clips do. The
  transition is a click away (each view takes the focus on a click).
- Region **pitch bend audition**: the bend lane edits the model and sounds on
  the next render, but clicking in the bend lane does not audition anything (the
  audition is a note, not a controller move) — noted rather than faked.

## Findings from the review that turned out wrong at the code

- **"Cmd-Z with the docked roll focused may do nothing."** It does work: the
  menu bar claims Cmd-modified keys for its own window before the focus view
  (Haiku's `BWindow::_HandleKeyDown`), and the docked roll lives in the window
  whose Edit menu owns Undo. What was broken was the roll's state after the
  undo, which is item A.
- **"the roll emits only its three edit posts"** (item D's framing): true, and
  a fourth (`kMsgRollAudition`) was added — but the engine had no way to sound a
  note without a live MIDI input, so the claim that the path "exists, it is just
  not wired" would have been wrong: the voice pool, a bus for the target track
  and the graph-build rule all had to be added.
- **The lane menu's "no effect yet"**: the review expected one label list; the
  model already had the list spread across `MidiControl.h`'s evaluators and
  `CcDefault`. `CcVoiceEffect` is new — the evaluators' prose is now the table's
  source, not the other way round.

## Not in this package (named so they are not lost)

- `PianoRoll`'s title names the track it opened on; renaming the track while the
  window is up does not retitle it.
- The dock strip and the pop-out title use the track name; a region name (the
  clip has no name field for MIDI regions) would be a model change.
