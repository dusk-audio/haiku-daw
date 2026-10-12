# Task: M3.4 — the mixer strip extras (trim, phase, sends, solo groups, dB entry)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/m3-transport-mixer-jobs` off `master`. This is item M3.4 of
`docs/PLAN_1.0.md` (M3.4, "Mixer strips"). Inserts already exist and are not
restyled here (M3.4's last bullet is the widget kit's, already merged).

## Codebase orientation (read before coding)

- `src/model/Project.h` — `Track` carries `gain`, `pan`, `muted`, `soloed`,
  `soloSafe`, `muteGroup`, `sends[]` (`Send { dest, level, preFader }`). Mute
  groups already cascade: `SetTrackMuteCommand::Do` applies the new state to
  every track with the same `muteGroup` (`Commands.cpp:129`).
- `src/model/Commands.h` — one command per model edit; `SetTrackMuteGroupCommand`
  (`:130`) is the template for the new ones. **Every model edit goes through
  `CommandStack`** (`docs/PLAN_1.0.md`, hard rules) — a drag previews by writing
  the model and commits ONE command on release (the timeline's rule).
- `src/model/ProjectIO.cpp` — the text format. New per-track fields are
  **append-only optional lines**, written only when non-default, so a project
  that never uses them saves byte-identically and every older file still loads.
  `trackfold` (`:241` write, `:560` read, applied after the whole file is read,
  addressed by track id) is the pattern to copy. Compat tests:
  `tests/projectio_tests.cpp` (`LoadPatched`, round-trip ~100-200,
  compat ~480-600).
- `src/engine/Engine.cpp` — the live mix. An audio track's fader is per stream
  (`TrackStream::SetMix`, `:1251` `UpdateMix`, "the fader is per-stream",
  `Render` at `:672`); a MIDI or bus node's fader is `midiGainL/R` /
  `busGainL/R` applied in `Render` (`:708`, `:719`). Gain/pan automation
  overwrites those (per block, `:644`), which is why trim and phase are their
  own atomics applied where the node's own material is scaled, not folded into
  the fader value.
- `src/engine/Exporter.cpp` — the offline mix, the same shape: the track's own
  material is built dry (`:566`), pre-fader sends tap it (`:627`), the fader
  multiplies it (`:640`), the chain runs (`:662`), post-fader sends tap it
  (`:664`). Sends already honour `s.level` (`sumDelayed(db, nb, s.level, delay)`,
  `:420`) — a strip send knob goes through `SetSendsCommand`, **not** a second
  path.
- `src/ui/MixerWindow.{h,cpp}` — the strip rack: `MixerStripInfo` is a flat
  snapshot (no model structures, the window is its own looper), `DrawStrip`
  draws, `MouseDown`/`MouseMoved`/`MouseUp` edit the snapshot and post intents
  (`kMsgApplyMix`, `kMsgApplySend`, ...), and `MainWindow` turns them into
  commands. `MainWindow::BuildMixerStrips` (`:204`) builds the snapshot;
  `kMsgUiRefresh` (`:753`) pushes a fresh one; the `kMsgApplyMix` handler
  (`:1355`) runs one macro per strip gesture.
- UI rules: `docs/UI_GUIDELINES.md` is binding (screenshots are the
  orchestrator's; this package writes the spec and the code so that looking at
  it can pass — no overlap, text truncated, `Themed()` metrics). The mixer's
  strips are 96 px wide and the rack must stay usable at the window's minimum
  height (`SetSizeLimits(160, 100000, 320, 100000)`).

## Goal

Every strip has an input trim, a phase-invert button, a level knob per aux send,
and a solo group; the fader takes a typed dB value on double-click. All of it is
model + engine + exporter state, host-tested, undoable, and persisted.

## Design

### 1. Model

`Track` gains, beside `gain/pan/muted/soloed/soloSafe/muteGroup`:

```cpp
float trim       = 1.0f;    // input trim (linear), pre-fader, pre-FX
bool  phaseInvert = false;  // polarity flip on the track's own material
int   soloGroup  = 0;       // >0 = solo-group id; soloing any member solos all
```

### 2. Commands (beside `SetTrackMuteGroupCommand`)

- `SetTrackTrimCommand(track, trim)` — clamps to 0..4 (a +12 dB ceiling).
- `SetTrackPhaseCommand(track, invert)`.
- `SetTrackSoloGroupCommand(track, group)`.
- `SetTrackSoloCommand` gains the solo-group cascade, exactly as
  `SetTrackMuteCommand` has the mute-group cascade (members' old flags are
  stored for Undo). With `soloGroup == 0` — every existing project — the
  behaviour is byte-for-byte what it was.
- The send knobs use the existing `SetSendsCommand` (whole-list replace, one
  coalescing command per drag, as the sends editor already does).

### 3. Engine and exporter

- `TrackStream` gains `std::atomic<float> fTrim{1.0f}` and
  `std::atomic<bool> fPhase{false}`, applied in `Mix` to the contribution (one
  snapshot per block, like `fGainL/R`); `Engine::UpdateMix` pushes them from the
  model. Because they are separate from the fader atoms, gain/pan automation
  cannot drop them.
- `Engine::Graph::Bus` gains `trim`/`phaseInv` atoms for MIDI and bus nodes,
  applied where `midiGainL/R` / `busGainL/R` scale the node's own material.
- The exporter applies the same two factors to the fader multiply at `:640`,
  in both the constant and the automated branch.
- No engine change is needed for solo groups: the cascade sets `soloed` on the
  members, which the existing audibility rule already reads.

### 4. Persistence

Append-only optional lines, written only when non-default and applied by track
id after the file is read (the `trackfold` pattern):

```
tracktrim <id> <trim>
trackphase <id> 1
tracksologroup <id> <group>
```

### 5. UI

`MixerStripInfo` gains `trim`, `phaseInvert`, `soloGroup`, and `sends`
(`[{label, level}]`, filled from the model's `Send.dest` names in
`BuildMixerStrips`, so the strip never resolves an id). The flat-run encoding
both directions already used for inserts is extended to the sends.

Strip additions, top to bottom (all rects from one `LayoutStrip`, so drawing and
hit-testing cannot disagree):

- a trim knob and a phase `Ø` button in one row above the pan knob; the trim
  value is shown in the strip's value readout when it is not unity;
- one row per aux send (up to two, `+N` beyond that opens the sends editor):
  the destination's short name, a small level knob, and the level as text. It is
  the same `SetSendsCommand` path the sends editor uses, so a knob drag is one
  coalesced undo step;
- the solo button's row gains a `G` box showing the solo group (0 = "--");

and double-clicking the fader or the value readout opens an inline
`DawTextField` over that strip's value row, filled with the current gain in dB,
focused. Enter parses it (kit-free `ParseDecibels` in `src/dsp/MeterMath.h`),
converts to a linear gain and applies it through the same `kMsgApplyMix` path;
Escape or a click elsewhere cancels. `-99.9`/`-inf` means silence (gain 0).

## Work items

1. `src/model/Project.h` — the three fields.
2. `src/model/Commands.h/.cpp` — the three commands, the solo cascade, and their
   `CoalesceInto` (a trim knob drag is one undo step).
3. `src/model/ProjectIO.cpp` — write/read the three optional lines;
   `tests/projectio_tests.cpp` round-trip (set the fields, save, load, compare;
   save a file with the current format flags, patch in the lines, load).
4. `src/engine/Engine.{h,cpp}` — the per-stream and per-bus trim/phase atoms and
   their application; `UpdateMix` pushes them.
5. `src/engine/Exporter.cpp` — the same two factors on each track's material.
6. `src/ui/MixerWindow.{h,cpp}` + `src/ui/MainWindow.cpp` — the snapshot fields,
   the strip rows, the hit tests, the drags, the inline dB field, the new
   intent messages, and the `MainWindow` handlers that turn them into commands.
7. `src/dsp/MeterMath.h` — `ParseDecibels` (and its `GainFromDb` inverse if one
   is missing).
8. Tests: `tests/model_tests.cpp` (commands: trim clamp, phase, solo cascade +
   undo), `tests/exporter_tests.cpp` (a trimmed/inverted track bounces at the
   right level and polarity; a send level reaches the bus), `tests/meter_tests.cpp`
   (the dB parse), `tests/projectio_tests.cpp` (round-trip + old-file compat),
   and a VM check in `tests/ui_functional_tests.cpp` for the strip intents.

## Out of scope (say so in the record)

- Trim is applied to a track's own material (pre-fader, pre-FX), which is the
  console meaning of an input trim; it is not a per-clip gain and does not
  replace `Clip.gain`.
- Send *pre/post* stays the sends editor's business; the strip knob edits the
  level of an existing send and never creates or removes one.
- No change to the meter drawing (M3.3).

## Definition of done

- `cmake --build build-host` exits 0, `ctest --test-dir build-host` green (count
  in the record), `sh scripts/haiku_syntax_check.sh` 0 FAIL.
- Every new behaviour has a test, mutation-checked (mutations named in the
  record).
- The record says plainly which parts are unverified here: the VM screenshots
  and the audible/visual judgement are the orchestrator's.
