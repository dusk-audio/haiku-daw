# Task: Edits that bypass undo become commands (plan M0.5)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/undo-gaps` off master. The project's rule is that EVERY model mutation
goes through `CommandStack`; four UI paths still write the model directly, so
those edits cannot be undone and do not mark the project dirty (M0.1's title
marker never lights for them).

## The sites (verified)

- **Tempo field** (`MainWindow.cpp`, `case MSG_TEMPO`): writes `tempoBPM` and
  `tempoMap.SetTempoAt(0, bpm)` directly.
- **Master fader** (`case MSG_MASTER`): writes `masterGain` directly (every
  slider post — it must coalesce into ONE undo step per drag).
- **Mixer strip apply** (`case kMsgApplyMix`): writes `gain`/`pan`/`muted`/
  `soloed` directly, plus the mute-group cascade writing `muted` on every group
  member.
- **Solo-safe** (`TimelineView.cpp`, the track menu): toggles `soloSafe`
  directly ("transient mix state" per the old comment — the plan overrides
  that: it is project state, it is serialized, and it must undo).

## Work items

1. **`SetTempoCommand(double bpm)`** (kit-free): `Do` mirrors today's handler
   (`tempoBPM = bpm`, `SetTempoAt(0, bpm)` with ramp false — the handler's own
   behaviour) after capturing the old `tempoBPM` and the old frame-0
   `TempoChange` (bpm AND ramp, so undo restores a ramp the change would
   otherwise flatten); `Undo` restores both.
2. **`SetMasterGainCommand(float gain)`**: captures the old gain; **coalesces**
   (`CoalesceInto`) so a slider drag is one undo step, like `SetSendsCommand`.
3. **`SetSoloSafeCommand(TrackId, bool)`**: captures the old flag.
4. **The mixer strip apply becomes one `MacroCommand`** ("Mixer Strip") of the
   existing `SetTrackGainCommand`/`SetTrackPanCommand`/`SetTrackMuteCommand`/
   `SetTrackSoloCommand`, plus one `SetTrackMuteCommand` per mute-group member
   for the cascade — so the whole strip (and the group) undoes in one step. An
   unchanged field gets no sub-command.
5. **Wire the four sites** to execute those commands (the tempo one keeps its
   `ReloadActiveEngine()`; the mixer one keeps its refresh post and invalidation;
   solo-safe keeps its refresh post). The master fader keeps applying live —
   the engine reads `project.masterGain` per poll, and a coalesced command per
   post still leaves the model current.

## Definition of done

- Host tests (in `model_tests.cpp`): each command applies and undoes; a second
  `SetMasterGainCommand` folds into the first (one undo returns to the start);
  the mixer macro undoes the strip AND the group cascade in one step.
  Mutation-checked (skip the capture; make coalescing refuse).
- `sh scripts/haiku_syntax_check.sh` 0 FAIL; VM `build` and `build-off` ctest
  green; host suite counts in the record.
- PR record `docs/agent-prompts/15-undo-gaps-PR.md`, noting the tempo field's
  pre-existing ramp-flattening (unchanged here) and that a slider drag cannot
  be driven from a test (the commands are host-tested; the wiring is
  code-read).
