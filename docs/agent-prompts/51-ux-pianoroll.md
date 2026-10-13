# Task: piano-roll UX defects (package 51)

Branch `fix/ux-pianoroll`, off `master`. A UX review of the MIDI editor produced
`A`–`J` below; this file is the scope and the justification — implement it, do
not re-litigate it. **A, B and C are integrity bugs and come first**: the roll
can write a note list the model no longer matches, which is data loss, not a
cosmetic defect.

The orchestrator owns the VM: this session never runs `scripts/vm.sh`, never
sshes to the VM and never pushes. Screenshots are the real judgement for what
this branch draws, and the record names what to look for.

## Codebase orientation (read before coding)

- **`src/ui/PianoRoll.{h,cpp}`** — `PianoRollView` (custom-drawn: keyboard
  column, pitch × time grid, bottom lane, toolbar) and the thin `PianoRoll`
  `BWindow` host. Edits run on a *snapshot* (`fNotes`, `fEvents`, `fClipStart`,
  `fClipLen`) and are posted to the main window (`kMsgApplyNotes`,
  `kMsgApplyEvents`, `kMsgApplyMidiOp`), which owns model mutation through
  `CommandStack`. The roll never re-reads the model.
- **`src/ui/MainWindow.cpp`** — hosts the roll: `fDockRoll` (docked view,
  `OpenDockedEditor` ~2608) or a popped-out `PianoRoll` window registered in
  `fRollMsgr` via `kMsgRollOpened` (~2673). `PushRollPlayhead` (~4062) is the
  only thing it currently pushes. Note edits land in the `kMsgApplyNotes` /
  `kMsgApplyMidiOp` / `kMsgApplyEvents` handlers (~1369-1438).
- **The one revision counter**: `CommandStack::CurrentSerial()` (never reused,
  bumped by Execute/Undo/Redo/coalesce) is the model's revision — the 2 Hz
  `MSG_TITLE` poll and `UpdateEditMenu` already ride it.
- **`src/model/SnapGrid.h`** — kit-free grid (values, triplets, Off, snap +
  step maths); the arrange's `TimelineView::OpenGridMenu` and its `Snapped()`
  (~1447) are the pattern to copy. The roll is welded to `kSnapDiv = 4`.
- **`src/model/MidiControl.h`** — the model's own statement of which
  controllers act on a voice (CC1 mod wheel, CC7 × CC11 channel gain, CC10 pan,
  CC64 sustain; MidiExpression.h/Sustain.h). The lane menu currently offers all
  128 with GM names and says nothing about which do anything.
- **`src/engine/Engine.{h,cpp}`** — the voice pool (`Graph::fVoices`),
  `UpdateLiveVoices`/`RenderMonitorOnly`, and the live-input path the armed
  MIDI monitor uses (`SetLiveMidi`, `RecordController::UpdateMidiMonitor`).
  Audition has to reuse that pool: the region track's instrument is what must
  sound.
- **Tests**: kit-free logic is host-tested (`tests/*_tests.cpp`,
  `ctest --test-dir build-host`); `tests/ui_functional_tests.cpp` drives the
  real windows on the VM and is where a Haiku-only regression test goes.
  `src/ui/` does not compile on the host — `scripts/haiku_syntax_check.sh`.

## Work items

### A. The roll must re-read what it edits (integrity)

1. `MainWindow` keeps `fRollSerial` (the `CurrentSerial()` it last pushed). When
   the serial moved, it pushes the region's current state to **every** open roll
   — the docked view and the `fRollMsgr` window — as one message
   (`kMsgRollRegion`: track, clip, start, length, notes, events, serial). Pushed
   from: `MSG_EDIT_UNDO`, `MSG_EDIT_REDO`, the `kMsgApply*` handlers,
   `kMsgUiRefresh`, `kMsgReloadEngine` (a region drag posts only that one), and
   the 2 Hz `MSG_TITLE` poll as the backstop for edits the window never hears
   about (the timeline's own commits).
2. The roll applies the pushed region through the kit-free rule
   (`RollEdit.h: DecideRollSync`): content or length changed → **Reload**
   (snapshot replaced, selection cleared, any in-flight drag cancelled — an
   uncommitted gesture loses to the model); only the region's start moved →
   **Repoint**; the region is gone → the docked roll's host clears the pane and
   the popped-out window quits.
3. Every edit the roll posts carries the serial it was based on; `MainWindow`
   applies it only when that serial is still `CurrentSerial()` and otherwise
   refreshes the roll instead. That is the "re-reads before it commits" rule: a
   commit built on a state the model no longer has is never applied.
4. Verify the docked roll's Cmd-Z. `PianoRoll` (the window) forwards Cmd-Z via
   `ForwardUndoRedo`, but the **docked view** lives in the main window and has
   no `DispatchMessage` of its own. Haiku's `BWindow::_HandleKeyDown` offers
   Cmd-modified keys to the window's own menu bar first
   (`haiku/src/kits/interface/Window.cpp:3681`), so the main window's Edit menu
   does own Cmd-Z with the docked roll focused — record this as verified by
   source, and say in the record what could not be checked on the VM.

### B. The roll must notice its region changing under it

- A timeline drag on a MIDI region commits `MoveMidiClipCommand` (id kept,
  only `startFrame` written) and posts `kMsgReloadEngine`; `S` (`SplitAtPlayhead`)
  commits `SplitMidiClipCommand`. The roll held the open-time
  `fClipStart`/`fClipLen`, so its next gesture rewrote the whole part shifted,
  and a split grew the half back to the old length. A/B are one mechanism
  (item A's push): the reload carries the region's start and length, so the
  roll re-points, re-reads, and a region that is gone closes the editor
  (`ClearDockedEditor` for the dock, `B_QUIT_REQUESTED` for the window).

### C. Follow-playhead must be controllable from the roll

- `MainWindow` keeps the follow state (the View menu's mark is the source of
  truth) and fans `MSG_FOLLOW` out to the docked roll and the registered
  window; the roll gets `SetFollow(bool)` (its hard-coded `fFollow = true` goes)
  and opens with the menu's current state, so the mark and both views agree.

### D. Note audition

- The roll emits an audition message (`kMsgRollAudition`: track, pitch,
  velocity, on) on: a keyboard-column click (note-on on mouse-down, note-off on
  mouse-up or when the pointer leaves the column), a drawn note (pencil/brush,
  on creation) and a velocity-lane click.
- `MainWindow` routes it to the engine's audition path: `Engine::AuditionNote`
  pushes into a lock-free ring the RT thread drains into the existing voice pool
  (`Graph::LiveVoice`), routed to the bus of the region's track, so the sound is
  that track's instrument + fader + FX. While the transport is stopped and
  nothing is armed, the window starts the monitor-only engine for it (the same
  `SetMonitorOnly` + `Load` + `Start` path the idle MIDI monitor uses) and stops
  it again when the audition goes quiet.
- A Preferences switch to mute it is *desirable if cheap*: the plan is a
  "Preview notes when editing" toggle in the Preferences window, honoured in
  `MainWindow` (the roll always asks; the window decides). If it does not land
  cheaply, the record says how the audition is turned off instead.

### E. The snap grid

- The roll gets a `SnapGrid` (default 1/16, matching today's behaviour), a
  toolbar grid button showing `SnapGridLabel` and opening the same menu the
  arrange has (values, Triplets, Off — one kit-free definition), and `g` as the
  key, exactly as the arrange. `Snapped()` goes through the kit-free
  `SnapClipRelative`, so triplets and swing parts can be drawn. Shift stays the
  free-placement override.

### F. CC points: drag the point, do not paint over it

- Mouse-down in a CC lane resolves the point under the cursor first
  (`CcPointNearFrame`, the existing 7 px grab radius in frame space) and drags
  **that** point (identity = its index) instead of `SetCcAt`ting a new one at
  every new x — which left the old point behind plus a staircase of new ones.
  Only empty space paints new points (the ramp path, unchanged). A point moved
  onto a frame that already holds one of the same stream absorbs it (one point
  per stream per frame, the step function's rule). The lane menu gets a
  **Clear lane** entry (one undo step; "no effect" for the velocity lane).

### G. The lane menu must not advertise what does nothing

- One kit-free table (`MidiControl.h: CcVoiceEffect`) is the source of truth for
  which controllers move a voice; the lane menu's labels come from it
  ("CC74 Cutoff — no effect yet" vs "CC64 Sus — sustain pedal"), so the menu
  cannot drift from the engine.
- **Pitch bend**: the model stores bend events and they sound, but the lane
  filters to `MidiClipEvent::CC`, so a bend is invisible and uneditable. The
  lane generalises to a lane address (−1 velocity, −2 pitch bend, ≥0 CC) with
  its own value range (0..16383, centred at 8192) and a menu entry; if the
  clean implementation does not fit the schedule, the record says what is left.

### H. Editing feel (one coherent item; each part small)

- **Copy/Paste/Duplicate**: Cmd-C / Cmd-V (paste at the playhead, else at the
  region head) / Cmd-D (duplicate in place, offset one grid step) and
  Alt-drag (duplicate the selection and drag the copies). All through
  `CommandStack` as one edit each (`kMsgApplyNotes`).
- **Left-edge resize**: the 5 px grab zone exists at the right edge only;
  add the left edge (drag changes start + length, notes stay put) with
  **cursor feedback** (`BCursor`, the same ids `TimelineView` uses).
- **Arrows**: Left/Right nudge the selection by one grid step when something is
  selected (scrolling stays the fallback with nothing selected); Up/Down
  transpose the selection a semitone.
- **Zoom**: `+`/`-` go through `ZoomBy` (anchored on the playhead when in view)
  instead of multiplying `fFramesPerPixel` about the view's left edge, and `f`
  fits the region.

### I. The region's bounds are drawn

- The roll draws the region's start/end edges and dims notes whose **start**
  falls outside `[0, fClipLen)` — the same rule the engine plays by
  (`RollEdit.h: NoteInRegion` mirrors `MidiOps`' window), so "make this two
  bars longer" stops being guesswork.

### J. Theme scale, tokens and the toolbar

- Every metric in the view goes through `Themed()` (`kKbdW`, `kRowH`,
  `kVelLaneH`, `kToolbarH`, all toolbar rects) and every colour through the
  theme tokens — including the bottom lane's literal `Rgb(18, 20, 25)`. The
  toolbar lays itself out from `Themed()` and the view's own width (the MIDI
  button is reachable at 150 % and in a narrow dock; the label truncates).
- The popped-out window is zoomable, named "Piano Roll — <track>", and its
  opening size follows the theme scale.

## Verification

1. `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build build-host -j2` (exit code checked).
2. `ctest --test-dir build-host` green, with the count.
3. `sh scripts/haiku_syntax_check.sh` — no NEW failures (four FLAC/Vorbis
   pre-existing).
4. The kit-free rules — region sync/identity, the commit gate, snap-in-region,
   the region window rule, the CC point hit/move/clear, the lane value maths —
   live in `src/model/RollEdit.h` and are covered by `tests/rolledit_tests.cpp`,
   mutation-checked and named in the record.
5. `tests/ui_functional_tests.cpp` gets the regression checks (undo leaves the
   roll stale; a region moved under an open roll; the follow toggle; the grid
   menu's effect; the CC drag identity) and a `Shot()` for each, because those
   are the Haiku-only halves.
6. The record `docs/agent-prompts/51-ux-pianoroll-PR.md` names the shots to
   look for on the VM (dock and pop-out, System and Dark, at 150 %).

## Definition of done

- Host exit 0, host suite green, syntax check with no new failures, every new
  behaviour mutation-checked.
- The record says what was left undone and why, what could not be verified, and
  any finding from the review that turned out wrong at the code.
- Commits: `marc@duskaudio.com`, conventional subjects, no AI trailer.
