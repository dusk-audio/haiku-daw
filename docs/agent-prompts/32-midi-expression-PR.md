# Record — T10 / M7.3 MIDI expression (package 32)

Branch `feature/midi-expression` off `master`. Spec: `32-midi-expression.md`.

## What shipped

1. **Pitch bend is audible, and it is a ratio, not a retune.** `Render` gained a
   trailing `const VoiceExpression& expr = {}` (`src/synth/IInstrument.h`), so
   every existing caller and test compiles untouched. A voice's oscillator phase
   (Synth) / sample read position (Sampler) now advances by
   `elapsed(g) = (g − noteStart) + [Δ(g) − Δ(noteStart)]`, where
   `Δ(x) = ∫₀^x (ratio(t) − 1) dt` is computed from the track's bend events
   (`src/model/MidiExpression.h`). `Δ` is a pure function of the frame — a voice
   still carries no state — and the block's ratio is the block's **mean**
   `(Δ(end) − Δ(start))/frames`, so the phase the voice reaches at a block's end
   is exactly the next block's `Δ`: no step at any seam, including a block a
   bend event lands inside. `MidiNote` gained a render-only
   `bendPhaseFrames = Δ(note start)` (never serialized) so the part of the
   integral that belongs to the time *before* a block is not lost — that term is
   what lets a note begin under a held bend without a phase offset.
   Full-scale bend = ±2 semitones (GM default; the model has no RPN).
2. **Sustain (CC64) latches**, in one policy implemented twice:
   `ApplySustain` (a region's recorded notes, `src/model/Sustain.h`, reached
   through the new `Track::CollectPlaybackNotes()` which the engine *and* the
   Exporter render) and `SustainLatch` (the live state machine the engine
   drives). A key-up under the pedal is deferred to the pedal lift, or cut by a
   re-strike of the same key; the pedal only releases what it took; a note never
   sounds past its region window. The offline form is a pure function of the
   event list, so a recorded pedal and a played one sound the same.
3. **Live CC and bend reach the sounding voices** (`Engine::UpdateLiveVoices`,
   Haiku-only, RT): a preallocated `uint8_t fLiveCc[128]`, `fLiveBendRatio`, and
   a per-block `fLiveBendPhase` accumulator (the live twin of Δ) are handed to
   every voice through the same `VoiceExpression`, so a wheel or pedal move is
   audible on notes already down. Re-striking a pedal-held key releases and
   retriggers. Allocation-free: fixed arrays and arithmetic only.
4. **Mod wheel (CC1) drives a 5 Hz vibrato, ±1 semitone at full wheel**, in both
   voices; its phase contribution is the closed form of `∫A·sin`, so a
   vibrating voice is still a pure function of its block and the wheel never
   walks the pitch away (the integral returns to zero at every whole period).
5. **The piano roll's CC lane offers any of the 128 controllers** instead of a
   fixed four (`PianoRollView::SetLaneCc`, public so a test can drive what the
   popup does; the menu lists the controllers the region already has, then all
   128 in four groups, labelled `CC<n> <name>` where the name is known). The
   lane draws the same fallback the render path uses (`CcDefault`: 127 for
   CC7/CC11, 64 for CC10, 0 elsewhere), so the staircase on screen is the level
   that sounds.
6. **No `ProjectIO` change was needed**: the `mev` record already carries any
   controller number with range checks. Added round-trip and old-file compat
   tests instead (append-only, nothing in the loader touched).

## Verification

| check | result |
| --- | --- |
| host `cmake --build build-host` | exit 0 |
| host `ctest --test-dir build-host` | 53/53 green (`midi_expression_tests` 61 checks, `sampler_tests` 122, `exporter_tests` 91, `projectio_tests` 156, `synth_tests` and `midicontrol_tests` unchanged and green) |
| ASan/UBSan (`b-asan`, clean reconfigure) | build exit 0, 53/53 green |
| `scripts/haiku_syntax_check.sh` | stock list on `master`'s copy: 9 OK + 1 pre-existing FAIL (`src/ui/EffectsWindow.cpp`, `<Screen.h>` → `<Accelerant.h>` is not on its include path — `feature/lv2-state` fixes that, and the file is untouched here). The equivalent sweep of **all 65 Haiku-compiled sources** with that fix plus `-DDAW_HAVE_LV2=1` (also from `feature/lv2-state`'s copy, and not cherry-pickable onto master's older script): **0 FAIL**. |
| VM `build` ctest | 55/55 passed (23.3 s), `ui_functional_tests` 17.7 s |
| VM `build-off` ctest | 51/51 passed (21.0 s), `ui_functional_tests` 16.6 s |
| VM `ui_functional_tests` | **226 checks, 0 failures** (210 before this package; the new `test_piano_roll_cc_lane` adds 16) |
| `DAW_UI_SHOTS` review | 26 shots taken and pulled; `02-pianoroll-cc-lane` and `03-pianoroll-cc-point` reviewed at full size and zoomed — the review found a real bug (the caption read `CC1` for the mod wheel), fixed in `c43b594`, and the re-run's shots show `Mod` |
| VM checkout after the runs | `git rev-parse HEAD` = `c43b594` (the branch), so no run tested another tree; `grep -A 10 DEBUGGER /boot/system/var/log/syslog` names only the first faulted run, nothing since the fix |

## Mutation checks (each: break it, watch the named test fail, restore)

| # | mutation | caught by |
| --- | --- | --- |
| 1 | `bend-note-offset` — drop the note's own `bendPhaseFrames` in `Synth::Render` | `midi_expression_tests` (3 FAIL) |
| 2 | `bend-block-term` — drop the within-block ratio advance | `midi_expression_tests` (3 FAIL) |
| 3 | `sampler-bend` — stop sliding the sampler's read position | `sampler_tests` (2 FAIL) |
| 4 | `vibrato-integral` — return 0 from `VibratoPhaseFrames` | `midi_expression_tests` (4 FAIL) |
| 5 | `modwheel-expression` — stop reading CC1 into the expression | `exporter_tests` (1 FAIL) |
| 6 | `sustain-pedal` — latch never defers a note-off | `midi_expression_tests` (15 FAIL) |
| 7 | `sustain-window` — ignore the region-window cap | `midi_expression_tests` (4 FAIL) |
| 8 | `sustain-restrike` — drop the re-strike cut | `midi_expression_tests` (4 FAIL) |
| 9 | `playback-notes` — `CollectPlaybackNotes` skips the pedal | `exporter_tests` (2 FAIL) |
| 10 | `lane-controller` — the lane writes a fixed CC7 whatever it shows | `ui_functional_tests` on the VM: 221 checks, **2 failures** (`sawCc1`, `differs`), exit 1 |
| 11 | `lane-label` — the caption stops naming the controller | `ui_functional_tests` on the VM: 226 checks, **1 failure** (`laneLabel() == "Mod"`), exit 1 |

The live-input path (`UpdateLiveVoices`) is Haiku-only and cannot be
mutation-checked on the host; it is compile-checked (`haiku_syntax_check.sh`)
and built on the VM. Nothing audible is verified on the VM either (no audio
device) — the live path's evidence is that it renders through the same
`VoiceExpression` as the recorded path, which *is* host-tested end to end.

## UI review (UI_GUIDELINES §1/§3)

Shots taken (`DAW_UI_SHOTS`, dark theme as `main.cpp` sets up), all 26 opened;
the two that show this change were reviewed at full size and zoomed:

- `02-pianoroll-cc-lane` — the roll with the lane switched to CC1: the button
  is tinted (a controller lane), the lane strip is empty, nothing overlaps, the
  caption fits its rect. **This is where the caption bug showed**: it read
  `CC1`, not `Mod` (`SetLaneCc` drew the buffer while `LaneLabelFor` returned
  the name without copying it). Fixed in `c43b594`, and the caption is now
  required to agree with the lane by the functional test
  (`laneLabel() == "Mod"/"Sus"/"CC74"/"Vel"`) — the check that would have
  caught it without a screenshot.
- `03-pianoroll-cc-point` — the same lane after a click: the staircase holds
  127 from frame 0 (the level the engine renders, `CcDefault` for the fallback)
  with its handle at the point, inside the lane, nothing clipped.

Also opened `01-pianoroll-window` and the main-window shots
(`00-startup`, `14-playing`, `18-docked-editor`, `21-big-project-playing`) to
confirm nothing else about the roll or a lane changed: the velocity lane and
its lollipops are untouched, the button's rect and the toolbar are unchanged.

## The first run of the new UI test faulted (and what it taught)

The first VM run wedged inside `test_piano_roll_cc_lane` and left Haiku's crash
dialog on the screen for the next agent. The syslog named it exactly:

```
KERN: DEBUGGER: Looper must be locked.
KERN:   BView::Invalidate() + 0x1d
KERN:   daw::PianoRollView::SetLaneCc(int) + 0x60
KERN:   TestPianoRollCcLane(MainWindow*, Project&) + 0x546
```

i.e. the test called the view from the test thread without the roll's lock, and
`BView::Invalidate()` inside `SetLaneCc` tripped `BLooper::check_lock()` — which
is a `debugger()` call, not an exception. Fixed by holding the roll's lock at
every call site (`setLane`/`laneCc`/`Bounds`/`Invalidate` helpers), which is the
rule the file already states for `RunMidiOp`. The lesson is now in
`docs/HANDOFF.md`'s dev-loop lessons (a faulted run leaves the dialog, it wedges
later runs, `sendkey ret` clears it, and
`grep -A 12 DEBUGGER /boot/system/var/log/syslog` names the frame).

## Known limits / decisions

- **Live channel controllers are engine-wide**, not per MIDI endpoint: route
  demux applies to notes, so with two keyboards one wheel moves every monitored
  track. Recorded lanes are per-track, as they should be.
- **The live latch is per engine instance**, and the engine is rebuilt at every
  playback start (`TransportController`), so pedal/bend state does not survive a
  transport start; the next CC64 from the keyboard restores it. `Stop()` clears
  the controllers with the voices, so a keyboard that vanished mid-pedal cannot
  colour the next start.
- **The bend is quantized to block boundaries** in pitch (the phase is exact at
  every seam). A bend event landing mid-block is averaged over that block, so
  the pitch inside that one block is the mean rather than the step — inaudible
  at buffer sizes, and the alternative (stepping at the event frame) is what
  would click.
- **Only CC1, CC7, CC10, CC11, CC64 and pitch bend act on a voice.** Any other
  controller can be drawn, recorded, saved and exported, and does nothing
  audible yet — MIDI learn / a parameter target is post-1.0 (§PLAN).
- The vibrato's ±1 semitone wobble is not reflected in the PolyBLEP residual
  (the block's bend ratio is), which is a deliberate approximation; it only
  affects saw/square at the extremes.
- For a `loop_sustain` sample voice in its release tail, the read position the
  tail starts from is anchored with the current block's ratio, so a bend that is
  actively MOVING during that tail shifts it by under a frame of read position
  (zero whenever the wheel is held still). Commented where it lives
  (`Sampler::Render`); the alternative is per-voice state, which would break the
  "a block is a pure function of its start frame" contract.
- `VoiceExpression` itself lives in its own tiny header
  (`src/model/VoiceExpression.h`) so `synth/IInstrument.h` can take one without
  pulling the project model into every voice translation unit; the evaluators
  that fill it from a track's events are in `src/model/MidiExpression.h` (the
  spec's "expression descriptor" item, split for that reason).
