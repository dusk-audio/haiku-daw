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
| host `ctest --test-dir build-host` | TBD |
| ASan/UBSan (`b-asan`) | TBD |
| `scripts/haiku_syntax_check.sh` | TBD |
| VM `build` ctest | TBD |
| VM `build-off` ctest | TBD |
| VM `ui_functional_tests` | TBD |
| `DAW_UI_SHOTS` review | TBD |

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
| 10 | `lane-controller` — the lane writes a fixed CC7 whatever it shows | `ui_functional_tests` (VM) |

The live-input path (`UpdateLiveVoices`) is Haiku-only and cannot be
mutation-checked on the host; it is compile-checked (`haiku_syntax_check.sh`)
and built on the VM. Nothing audible is verified on the VM either (no audio
device) — the live path's evidence is that it renders through the same
`VoiceExpression` as the recorded path, which *is* host-tested end to end.

## UI review (UI_GUIDELINES §1/§3)

TBD — shots and what was fixed.

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
