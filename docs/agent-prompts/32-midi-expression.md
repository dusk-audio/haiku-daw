# Task: MIDI expression — sustain (CC64), pitch bend and mod wheel

You are working in the Haiku DAW repo, a native Haiku OS DAW (C++17, CMake).
Work on branch `feature/midi-expression` off `master`. Three other unmerged
branches touch `src/engine/Engine.cpp`, `src/model/ProjectIO.cpp` and
`tests/ui_functional_tests.cpp`: keep `ProjectIO` append-only, CMake in one
contiguous hunk, UI edits small.

## Codebase orientation (read before coding)

- Layering: kit-free `daw_model` (model + DSP + synth + Exporter) builds and is
  tested on the Linux host (`cmake -S . -B build-host && cmake --build build-host
  -j8 && ctest --test-dir build-host`). `src/engine/Engine.cpp` is **Haiku-only**
  (edit pattern-faithfully, no host compile — verify with
  `sh scripts/haiku_syntax_check.sh` and the VM build). `src/engine/Exporter.cpp`
  is kit-free: **it is the executable spec** — live playback and a bounce must
  stay sample-identical, so every expression rule is proved there first.
- `IInstrument` (`src/synth/IInstrument.h`) is the voice interface: `Render`
  ADDS every note sounding in `[blockStart, blockStart+frames)` into an
  interleaved-stereo buffer, ramping a per-channel gain `from` → `to` across the
  block. **The contract that matters:** a block is a *pure function of its start
  frame*; no state survives a call (that is what makes a graph rebuild at a loop
  seam click-free and a bounce identical to playback). Do not break it — see
  "Per-voice phase integration" below for how bend stays stateless.
- Voices: `Synth` (`src/synth/Synth.{h,cpp}`, oscillator + ADSR) and `Sampler`
  (`src/synth/Sampler.{h,cpp}`, SFZ/SF2 regions). `SynthInstrument.h` adapts the
  former; `InstrumentFactory.{h,cpp}` builds either from an `InstrumentDesc`.
  Both voices derive phase/read position from `(globalFrame - noteStart)`.
- Model: `MidiClip` / `MidiNote` / `MidiClipEvent` (`src/model/Project.h`);
  `MidiClipEvent` is a step function of clip-relative frames (`CC`, `PitchBend`,
  `Program`, `ChannelPressure`), flattened by `Track::CollectNotes()` /
  `CollectEvents()`. `src/model/MidiControl.h` evaluates them at a frame (CC7 ×
  CC11 gain, CC10 pan) — that is the path the engine and Exporter already share.
- Live input: `MidiInputPort` (`src/midi/MidiPort.h`, Haiku-only) publishes two
  independent rings, so `MidiEventRing` is drained by the RT thread through
  `IMidiInput`; `Engine::UpdateLiveVoices` (`Engine.cpp:778`) drains it per block
  into a preallocated `LiveVoice fVoices[64]` pool and rebuilds `fLiveNotes` /
  per-`Bus` `liveNotes`. `MidiRecorder` is the record-side consumer and already
  captures CC and bend as `MidiClipEvent`s.
- UI: `src/ui/PianoRoll.{h,cpp}` — a fully custom-drawn view. The bottom lane
  shows note velocity or one controller, chosen from a **fixed** 4-entry table
  (`kLanes`), cycled by a button. Edits post `kMsgApplyEvents` to `MainWindow`,
  which applies them through `SetMidiClipEventsCommand`.
- RT rules (absolute): the audio callback does no allocation, locks, file I/O,
  syscalls or logging — arithmetic on preallocated memory, atomics and lock-free
  rings only. `UpdateLiveVoices` and `FillBuffer` are RT code.
- Serialization: `ProjectIO` is append-only text; `mev <type> <frame> <data>
  <value>` already carries **any** CC number, pitch bend or pressure, with range
  checks on load (`ProjectIO.cpp:506`, `tests/projectio_tests.cpp:228`).

## Goal

A sustain pedal is the baseline for anyone playing piano, and today pitch bend
and the mod wheel are stored but silent. Make all three audible in both voices,
make a live controller reach the sounding voices, and let the piano roll's CC
lane offer any controller instead of four.

## Work items

1. **Expression descriptor + kit-free math** (`src/model/MidiExpression.h`, new):
   - `struct VoiceExpression { float bendRatio = 1; double bendPhase = 0;
     float modWheel = 0; }` — what a voice needs at a block start.
   - Bend range: **±2 semitones** (no RPN/bend-sensitivity event exists in the
     model; the GM default is the honest constant). `BendRatioAt(events, at)`.
   - The **bend phase integral**: Δ(at) = ∫₀^at (ratio(t) − 1) dt, a pure
     function of the frame and the (sorted, prefix-summed) bend events —
     `BuildBendTimeline(events)`, `BendPhaseAt(timeline, at)`,
     `BendRatioAt(timeline, at)`. Built once per Load **off** the RT thread.
   - Mod wheel: `ModWheelAt(events, at)` = CC1 0..1 (absent = 0).
   - `CcDefault(cc)`: the value a lane shows before its first point — 64 for
     CC10, 127 for CC7/CC11 (matching `MidiControl`'s fallbacks), 0 elsewhere.

2. **Per-voice phase integration** (`src/synth/Synth.cpp`, `Sampler.cpp`,
   `IInstrument.h`): `Render` gains a trailing
   `const VoiceExpression& expr = {}` so every existing caller and test compiles
   unchanged. Inside a voice the oscillator phase / sample read position uses
   **elapsed frames with the bend integrated**, never a re-derived frequency:
   ```
   elapsed(g) = (g − noteStart) + (expr.bendPhase − note.bendPhaseFrames)
                              + (expr.bendRatio − 1) · (g − blockStart)
   ```
   `note.bendPhaseFrames` is a **render-only** field on `MidiNote` (never
   serialized): Δ(note start), filled by whoever flattens the notes, so the
   integral that belongs to the time *before* a block is not lost — that is what
   keeps the phase continuous across a block boundary and stops a bend step from
   clicking. It is 0 for a note in a track with no bend events, so unbent
   playback is bit-identical to today.
   - **Mod wheel / LFO:** CC1 drives a **5 Hz sine vibrato on pitch, ±1 semitone
     at full wheel** (both voices; the sampler modulates its read position the
     same way). Its phase contribution is the closed form of ∫(A·sin) — pure, so
     the stateless contract holds:
     `Δvib(rel) = A·(1 − cos(ω·rel)) / ω`, `ω = 2πf/sr`, `A = modWheel · 0.06`.
     Documented at the definition and in this file: in the Synth it modulates
     the oscillator phase, in the Sampler the sample read position.
   - **Sustain does not live here:** a pedal-extended note is just a longer note
     (item 3), so both voices already honour it, and a reaped voice stays a pure
     function of the block.

3. **Sustain (CC64)** (`src/model/Sustain.h`, new; `Project.h`;
   `src/engine/Engine.cpp`):
   - The policy, in one statement: *CC64 ≥ 64 latches. A note-off that arrives
     while the pedal is down is deferred — the note keeps sounding until the
     pedal lifts, or until the same key is struck again, whichever comes first.
     A note-off while the pedal is up releases immediately, and notes still
     sounding when the pedal lifts keep sounding until their own note-off.*
   - **Offline / recorded form** — `ApplySustain(notes, events, maxEnd)`: extends
     each note whose key-up falls while CC64 is down (a pure function of the
     event list, so a recorded lane sounds exactly like a played pedal and a
     bounce matches playback). Never shortens, never moves a start, and never
     past `maxEnd` — the **region window**, so a note still cannot sound outside
     its region (`MidiOps.h`'s rule). Wired in as
     `Track::CollectPlaybackNotes()`, the playback form of `CollectNotes()`
     (which stays pure, so editing and SMF export are untouched); the engine and
     Exporter render *that*.
   - **Live form** — `SustainLatch`: the same policy as a state machine the
     engine drives event by event (`SetValue(cc64)` → lifted?, `DeferNoteOff()`,
     `NoteOn(key)` → was the pedal holding this key?), with the pedal state and
     the deferred keys queryable so a host test can assert the policy without an
     engine. It survives a `Load` (the pedal was already down when playback
     started — "pedal-down-at-load"), and the engine's `LiveVoice` gains a
     `sustained` flag it mirrors.
4. **Live controllers reach the engine** (`src/engine/Engine.cpp`, Haiku-only,
   RT-safe): `UpdateLiveVoices` also handles `kControlChange` and `kPitchBend`
   from the ring — a preallocated `uint8_t fLiveCc[128]`, `fLiveBend`, and a
   `double fLiveBendPhase` accumulator advanced per block by
   `(bendRatio − 1) · frames` so a live note's phase does not jump when the
   wheel moves. Live notes render through the **same** `VoiceExpression` path as
   recorded ones (a live expression built from `fLiveCc`/`fLiveBend`), and the
   latch defers/releases live voices. Live channel controllers are engine-wide
   (one keyboard's wheel is not attributed per endpoint) — state that limit in
   the record. No allocation, no locks: fixed arrays, plain arithmetic.
5. **Any CC in the piano roll's lane** (`src/ui/PianoRoll.{h,cpp}`, small):
   `fLane` (an index into a 4-entry table) becomes `fLaneCc` (−1 = velocity, the
   default). The lane button opens a `BPopUpMenu`: Velocity, the controllers the
   region actually contains, then the rest of the 128 in four "Controllers
   0–31 / 32–63 / 64–95 / 96–127" submenus, each entry labelled with the common
   name (`Mod`, `Vol`, `Pan`, `Expr`, `Sus`, …) or `CC n`. The menu is a popup
   a test cannot open, so the choice goes through a **public** `SetLaneCc(int)`
   (the `RunMidiOp` precedent). The lane draws what the engine renders — the
   step staircase — with `CcDefault()` for the value in force before the first
   point. Comment honestly which controllers the instrument acts on.
6. **Round trip** (`tests/projectio_tests.cpp`, append-only): a lane for an
   arbitrary controller (CC64, CC74) round-trips, and an old file without those
   lines still loads — the `mev` record already carries any number, so
   `ProjectIO.cpp` itself needs no change.

## Definition of done

- Host `cmake --build build-host` exits 0; `ctest --test-dir build-host` green;
  `b-asan` green; `sh scripts/haiku_syntax_check.sh` 0 FAIL; VM `build` and
  `build-off` ctest green; VM `ui_functional_tests` green **including new
  checks** that the lane offers an arbitrary controller (it lands in the model,
  and the note list fed to a voice carries it) and that a lane edit reaches the
  voices.
- New host tests (kit-free), each **mutation-checked** (break it, watch it fail,
  restore) and named in the commit body:
  - `midi_expression_tests`: bend ratio and the phase integral (including a
    note that straddles the bend event), **bend-ratio integration** — a
    block-rendered bend is sample-for-sample a reference whose phase is the
    numerically integrated ratio, and there is no step at the block seam where
    the bend changed;
  - the sustain state machine: pedal-down-at-load, overlapping notes, a
    deferred note released by the lift, a re-strike cutting a deferred note —
    and the live latch agreeing with `ApplySustain` on the same scripted stream;
  - mod-wheel depth: zero depth is bit-identical to no expression, full depth
    matches a finely-substepped numeric reference of the vibrato integral, and
    one full LFO period leaves the pitch where it started.
- Exporter test: a project with bend, CC64 and CC1 renders differently from one
  without — the source of truth that the whole path (not just the math) is wired.
- UI (the lane's menu and drawing change): `DAW_UI_SHOTS` pass on the unlocked
  VM, a shot showing the lane on an arbitrary controller, reviewed per
  `docs/UI_GUIDELINES.md` §3 (text fits, nothing overlaps, the button says what
  it is), with what was looked at and fixed named in the record.
- Record `docs/agent-prompts/32-midi-expression-PR.md` kept as you go; PR
  description: the expression contract, the bend integral's continuity argument,
  the sustain policy, the live-controller limitation, and the exact counts.
