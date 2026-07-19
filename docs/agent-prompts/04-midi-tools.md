# Task: MIDI transform suite — quantize, swing, humanize, legato

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch `feature/midi-tools` off master. This package is almost entirely kit-free and fully testable on this Linux host.

## Codebase orientation (read before coding)

- Layering: kit-free `daw_model` static lib builds anywhere and carries the ctest suite (`cmake -S . -B build-host && cmake --build build-host -j && ctest --test-dir build-host`). GUI (`src/ui/`) is Haiku-only — not compilable here; pattern-faithful edits only.
- MIDI model: `MidiClip` (`src/model/Project.h:66`) holds `std::vector<MidiNote> notes` (`{pitch, velocity, startFrame, lengthFrames}`, clip-relative frames, `Project.h:38`) and `std::vector<MidiClipEvent> events` (CC/PitchBend/Program/Pressure, `Project.h:51`).
- Musical time: `src/model/TempoMap.h` — frame-anchored tempo changes (with ramps) + meter changes. Use it for all frame↔beat conversion; never assume constant BPM. The timeline grid/snap logic lives in `src/model/Grid.h` (see `tests/grid_tests.cpp`) — reuse rather than reinvent beat-subdivision math.
- Mutation funnel: UI → `Command` (`src/model/Commands.h`) → `Project`, via `CommandStack`. Piano-roll note edits already go through `SetMidiClipNotesCommand` (`Commands.cpp:757` — replaces the clip's note list wholesale, one undo step). Your transforms should reuse exactly that command, not invent new mutation paths.
- Piano roll UI: `src/ui/PianoRoll.cpp` (~875 loc, own looper, edits a snapshot, posts `kMsgApplyNotes` to `MainWindow`). Tool palette + `KeyDown` handler show the shortcut conventions (`1`-`7` tools, Cmd-A select all).
- Tests: standalone `int main()` + `CHECK` macro per file, one `add_executable`/`add_test` in `CMakeLists.txt` (lines 76-215), link `daw_model`. Deterministic only — no wall-clock, no real RNG without a fixed seed.

## Goal

Commercial-baseline MIDI transforms, operating on a selection of notes (or whole clip): quantize with strength and swing, humanize, and a couple of cheap high-value extras.

## Work items

1. **Kit-free transform library** — new `src/model/MidiOps.h/.cpp`, pure functions over `std::vector<MidiNote>` (take the full list + a set/predicate of selected indices, return the transformed list). Frames in, frames out; callers pass the `TempoMap` and the clip's absolute start frame so grid positions are musically correct across tempo changes.
   - `Quantize(notes, sel, tempoMap, clipStart, grid, strength, swingPct, quantizeLengths)`: `grid` as a beat fraction (1/4..1/32 incl. triplets — reuse `Grid.h` subdivision types if they fit); `strength` 0..1 interpolates original→grid; `swingPct` 0..100 delays every second subdivision; optional note-end quantize.
   - `Humanize(notes, sel, timingJitterFrames, velocityJitter, seed)`: deterministic `std::minstd_rand`-style PRNG from the caller-provided seed (mirror the deterministic `NoteRandom` idea used by the sampler's round-robin). Clamp velocity 1..127, clamp start ≥ 0.
   - `Legato(notes, sel)`: extend each selected note to the start of the next note of any pitch (monophonic-legato semantics; leave the last note's length).
   - `TransposeSemitones` / `ScaleVelocity(mul, add)`: trivial, but they complete the menu.
   - All functions must be stable and idempotent where it makes sense (quantize at strength 1 twice == once).
2. **Commands**: thin wrappers that compute the new note list via `MidiOps` and delegate to `SetMidiClipNotesCommand` semantics — either reuse it directly from the UI layer or add named commands (`QuantizeNotesCommand` etc.) so the undo menu reads "Undo Quantize" instead of "Undo Edit Notes". Named commands preferred; no coalescing.
3. **Piano-roll integration** (`src/ui/PianoRoll.cpp` — Haiku-only, careful):
   - A "MIDI" menu (or extend the existing context/tool UI following current conventions): Quantize (with a small settings row or submenu for grid/strength/swing — check how existing dialogs like `RenameWindow` are built and keep it minimal), Humanize, Legato, Transpose ±1/±12, Velocity ±10.
   - Shortcut: `q` = quantize selection with last-used settings. Operates on the selection, or all notes when nothing selected (match existing piano-roll selection semantics).
   - Settings persist for the session on the window object; snapshot/apply flow unchanged (`kMsgApplyNotes`).
4. **Timeline hook** (optional if time allows, else note it): right-click on a MIDI region → Quantize with last-used settings, whole clip. Follow `TimelineView.cpp` context-menu conventions.

## Definition of done

- New ctest target `midiops_tests` covering: on-grid no-op; strength 0.5 midpoint (exact frame math); swing displaces only off-beats; triplet grids; quantize across a mid-clip tempo change (grid spacing changes at the change point — build a `TempoMap` with a ramp in the test); humanize determinism (same seed = same output, different seed differs), clamping at velocity/frame bounds; legato chains; idempotency checks.
- Existing tests untouched and passing.
- PR description: exact grid/strength/swing semantics chosen, and what remains for a future "groove template" feature.
