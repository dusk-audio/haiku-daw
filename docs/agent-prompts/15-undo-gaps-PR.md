# feat: edits that bypassed undo are commands (plan M0.5)

Branch `feature/undo-gaps` off `master` (`b123e4a`). Two commits: the spec, the
change.

## What changed

Four UI paths wrote the model directly, so they could not be undone and never
marked the project dirty (M0.1's title marker stayed dark for them):

- **Tempo field** → `SetTempoCommand`: `Do` mirrors the old handler (`tempoBPM`
  plus the map's frame-0 change), and `Undo` restores both, including the ramp
  flag the old write flattened on any edit.
- **Master fader** → `SetMasterGainCommand`, **coalescing** like
  `SetSendsCommand`: a drag's posts fold into one entry, so one undo returns to
  the value the drag started from. The engine still reads `project.masterGain`
  per poll, so the move is heard live.
- **Solo-safe** → `SetSoloSafeCommand`. It serializes, so it is project state;
  the old "transient mix state (like arm)" comment was the only thing saying
  otherwise.
- **Mixer strip apply** → one `MacroCommand` ("Mixer Strip") of the existing
  `SetTrack{Gain,Pan,Mute,Solo}` plus one mute per group member for the
  cascade, so the whole strip — and the group — undoes in a single step.
  Unchanged fields contribute no sub-command at all, and the live refresh posts
  are unchanged.

## What is verified, and how

- **Host: 52/52**; `model_tests` grew the M0.5 cases (+33 checks, 283 total):
  tempo apply/undo with both mirrors, the coalesced master drag (three posts,
  one undo, redo lands on the last), solo-safe, and the mixer macro's
  single-step undo of gain + pan + mute + the group cascade.
- Mutation-checked, each restored: `SetTempoCommand::Undo` skipping the map
  restore fails the map checks; `CoalesceInto` refusing fails the drag check.
- `sh scripts/haiku_syntax_check.sh`: `src/ui/MainWindow.cpp`,
  `src/ui/TimelineView.cpp` OK, 0 FAIL.

## What is NOT verified

| Claim | State |
| --- | --- |
| VM `build` / `build-off` ctest | **Pending**: the VM was taken down mid-session (an in-flight openssh package update broke its `sshd`; the syslog showed `rexec of .../.self/lib/openssh/sshd-session failed`). Rerun both suites and the ui flows when it is back, then merge. |
| The wiring itself (a slider drag, the temo field's Enter, the strip Apply, the track menu's Solo Safe) | Cannot be driven from a test: a slider drag and a popup menu are outside the harness. The commands are host-tested; the four handlers are code-read and compile-checked. Click line for the hardware pass: drag the master fader, undo once → back to where the drag started; set a tempo, undo → the field and the grid follow. |
| The tempo field's ramp flattening | Pre-existing (the handler always wrote `ramp=false`); `Undo` now restores a flattened ramp, but a *change* still flattens it. Recorded, not fixed — nothing in the UI sets a ramp at frame 0 today. |
