# feat(midi): transform suite — quantize, swing, humanize, legato

Package 04. Branch `feature/midi-tools` off `master` `c23c217`.

Almost all of the package is kit-free and host-tested; the UI half is Haiku-only
and is compile-verified with the cross-compiler, then click-tested by Marc. The
"Semantics chosen" section answers what `04-midi-tools.md` asked the PR to
record; "What is NOT verified" is the honest list.

## What changed

| File | What |
| --- | --- |
| `src/model/MidiOps.h/.cpp` (new) | The transforms: `Quantize`, `Humanize`, `Legato`, `TransposeSemitones`, `ScaleVelocity`, plus `NotesEqual` and the grid/op names. Kit-free, no UI, no engine. |
| `src/model/Commands.h/.cpp` | `ApplyMidiOpCommand` — records the result of a named transform as one undoable step. |
| `src/ui/QuantizeWindow.h/.cpp` (new) | The quantize settings dialog (grid / strength / swing / note ends), in the `RenameWindow` idiom: it never touches the model, it posts the settings to the roll. |
| `src/ui/PianoRoll.h/.cpp` | A `MIDI` toolbar button + popup (the window has no menu bar), the `q` shortcut, and the commit path `kMsgApplyMidiOp`. |
| `src/ui/MainWindow.cpp` | The `kMsgApplyMidiOp` handler; the two note-list messages now share one decode (`ParseNoteList`). |
| `tests/midiops_tests.cpp` (new) | 100 checks: strength/swing/triplets/tempo changes, humanize determinism, legato, clamps, window rules, idempotency. |
| `tests/model_tests.cpp` | `test_apply_midi_op_command`: names, restores exactly, and never resizes the region. |

## Semantics chosen

**Grid** — a step in BEATS, not frames: `1/4` = 1 beat, `1/8` = 1/2, `1/16` =
1/4, `1/32` = 1/8, and the triplet grids `1/4T` = 2/3, `1/8T` = 1/3, `1/16T` =
1/6 (three in the space of two). `src/model/Grid.h` was not reused: it encodes
one constant tempo and meter and has no production callers — the live snap paths
already go through `TempoMap` (the piano roll's own `Snapped` is the precedent).

**Position math** — every position is converted to absolute beats with
`TempoMap::BeatAt`, snapped in beat space, and converted back with `FrameAt`.
That is what makes the grid musically right across a tempo change or a ramp
inside the region, and with a region that does not start on a bar line. The
cost is that `FrameAt(BeatAt(f))` can differ from `f` by one frame; snapping is
still idempotent (a snapped note maps to itself).

**Strength** — the target is the interpolated point in beat space:
`newBeat = beat + strength * (target - beat)`. At 0.5 a note 1000 frames into a
120 BPM / 48 kHz session lands on exactly 500.

**Swing** — delays every SECOND grid slot by `swingPct/100 * step/3`. At 100%
an off-beat sits two-thirds of the way through its pair, i.e. the classic
triplet feel; at 0 it is straight. Starts only, never ends: an end that swung
would shorten the note that swung into it.

**Note lengths** — untouched unless "Quantize note ends" is on, in which case
ends snap too (unswung). A note can never end up shorter than one frame.

**Region window (the non-destructive rule)** — the region keeps content outside
`[0, lengthFrames)` but does not play it. So: a time-domain transform does not
touch a note that is already outside (snapping one back in would make it
suddenly audible), and a note that was inside is kept inside — the start is
clamped to the window and the end to its edge, so a transform can neither
silence a note it moved nor make the region grow. `ApplyMidiOpCommand` does not
grow the region at all, which is the second half of the same guarantee and the
difference from `SetMidiClipNotesCommand` (that one grows, by design, for notes
drawn past the end — both behaviours are pinned by tests).

**Selection** — the mask is index-aligned with the note list; an EMPTY mask
means "every note". The roll passes an empty mask when nothing is selected, so
a menu item does something useful on its own. (The task doc asked for "the
selection, or all notes when nothing selected (match existing piano-roll
selection semantics)" — the existing edit tools never fall back to all, so the
fallback here is a deliberate choice for commands, not an inherited one.)

**Where the transform runs** — in the roll, on its snapshot; the RESULT travels
to the model, not the parameters. The model command then has nothing to
recompute and no selection indices to re-resolve against a note list that may
have moved under it, and it matches how every other roll edit already commits.
A consequence worth stating: quantize uses the roll's tempo-map COPY
(`fTempo`), exactly as the roll's pencil and drag snapping already do.

**No-ops** — the roll compares the result with `NotesEqual` and posts nothing
when nothing moved: the command stack has no no-op detection, so a no-op
quantize would otherwise push an undo entry that undoes to the same state.

**Humanize defaults** — ±8 ms of timing and ±10 of velocity, seed varied per
run (so a second humanize is a fresh take, not a no-op). The function itself is
deterministic from its seed — same seed and input, same output — which is what
the tests pin; a groove-template feature would want that seed to be saveable.

**Shortcut** — `q` runs the last-used quantize. The roll's `KeyDown` handler was
enumerated first: it claims arrows, `+`/`=`, `-`/`_`, Delete/Backspace, `1`-`7`,
Command-A and byte 1; the window claims space (transport). Letters other than
`a`/`A` were free, so `q` is new and unclaimed. Command-Q is passed through to
the app rather than swallowed.

**Undo naming** — the command carries the transform's name ("Quantize",
"Humanize", "Legato", "Transpose", "Adjust Velocity"), which is what the task
doc wanted. NOTE: the Edit menu's Undo/Redo items are static labels and
`CommandStack::UndoName()` has no consumer anywhere (pre-existing), so the name
is recorded but not yet displayed. Wiring it is a separate small change; it is
listed under "Not done" below rather than smuggled into this package.

## What is verified, and how

```
cmake --build build-host                       # exit 0
ctest --test-dir build-host                    # 47/47  (46 on master + midiops_tests)
./build-host/midiops_tests                     # 100 checks, 0 failures
./build-host/model_tests                       # 231 checks, 0 failures
cmake -B b-asan -DDAW_SANITIZE=ON && ctest --test-dir b-asan   # 47/47, clean
sh scripts/haiku_syntax_check.sh <all Haiku-only sources>      # 0 FAIL (see below)
```

**Mutation testing** — every mutation was applied, the test run, and the code
restored (this is what caught a vacuous region-growth assertion; see below):

| Mutation | Result |
| --- | --- |
| swing divisor 3 -> 6 | caught (4 checks) |
| strength ignored (always full snap) | caught (2) |
| window rule off (`InWindow` always true) | caught (3) |
| humanize hash loses the note index | caught (1) |
| end cap disabled (note may grow the region) | caught (2) |
| start-overflow clamp removed | caught (1) |
| legato overwrites instead of extending | caught (1) |
| triplet grid step 1/3 -> 1/2 | caught (4) |
| `ApplyMidiOpCommand::Do` grows the region | caught (3) |
| `ApplyMidiOpCommand::Undo` does nothing | caught (1) |
| `ApplyMidiOpCommand::Do` ignores the new list | caught (3) |
| `Name()` loses the transform name | caught (2) |

The one that first got away: the region-growth test originally used a region
that the *edit* path had already grown, so a growing `Do()` was indistinguishable
from a correct one. The test now asserts both halves (the edit path grows to
8500, the transform path leaves it at 8000) and the mutation fails 3 checks.

**Cross-compile check** — `PianoRoll.cpp`, `QuantizeWindow.cpp` and
`MainWindow.cpp` all report OK. Two notes on the script:

- Its DEFAULT file list is stale: 10 files, and it does not include the piano
  roll, the inspector, the mixer window, the plugin browser, `Lv2UiWindow.cpp`
  or `src/midi/*`. Every Haiku-only source was checked explicitly with the file
  argument (27 OK). Fixing the default list is a small chore for R4.
- `Lv2UiWindow.cpp` and `src/plugin/Lv2Host.cpp` FAIL the check with
  `lilv/lilv.h: No such file or directory` — the cross tree has no lilv headers.
  Pre-existing, unrelated to this package, and the reason those two are not in
  the default list.

## What is NOT verified

| Claim | State |
| --- | --- |
| Everything in `src/ui/` | **Compiles (cross-compiler); never clicked.** The popup menu, the settings dialog, the `q` key and the commit path need a window. Click list below. |
| VM `build` + `build-off` + ctest | **NOT RUN YET, deliberately**: the VM's `~/haiku-daw` is the working checkout for Marc's package-07 click test, and `vm.sh sync` hard-resets it. Nothing in this package touches the engine or the Media Kit, so the host suite plus the cross-compile check is the whole verification available until the VM frees up. **Queue this before merging.** |

### Click list (piano roll window)

1. Open a MIDI region's piano roll (double-click a MIDI region on the timeline).
   Add a few notes deliberately off the grid (Pencil tool, Shift-drag or zoom in
   so off-grid is visible).
2. Press `q`.
   -> Expected: the notes snap to the nearest 16th; Edit ▸ Undo reads as one
   step and restores every note in one go.
   ✗ Failure: nothing moves (the run was a no-op or the message never arrived);
   or the timeline does not redraw; or an undo entry appears that changes nothing.
3. Click `MIDI` (toolbar, right of the zoom buttons) -> "Quantize…".
   Set Grid `1/8T`, Strength 50, Swing 100, tick "Quantize note ends", press
   Quantize.
   -> Expected: the dialog closes and the notes move halfway toward the swung
   triplet grid, ends quantized too.
4. Press `q` again. -> Expected: the same settings are reused without the dialog
   (this is what "last settings" means).
5. Select a few notes (Pointer, drag a marquee), then "Transpose +12".
   -> Expected: only the selected notes move; with nothing selected the same
   item moves the whole region.
6. "Humanize", twice. -> Expected: both times notes shift slightly in time and
   velocity; the second run is different from the first.
7. "Legato" on a chord -> Expected: each note extends to the start of the next
   note (any pitch); the last one does not change.
8. Undo everything (Edit ▸ Undo repeatedly). -> Expected: each transform is one
   undo step, in order, and the region is exactly as it was.
9. Make a region shorter than its content (drag its right edge left so some
   notes fall outside), then quantize the whole region.
   -> Expected: the notes outside stay outside (the region does not grow back,
   and nothing that was silent starts sounding).

## Not done (and why)

- **Timeline right-click -> Quantize** (task item 4, explicitly optional): the
  settings live on the piano-roll window, and the timeline has no path to them.
  Sharing them needs a home in `AppSettings` or on the project; noted rather
  than half-built.
- **Edit menu label wiring** (see "Undo naming"): `UndoName()`/`RedoName()` are
  ready, the menu is static. ~5 lines in `MainWindow`, no way to test it from
  here.
- **Groove templates** — what a future feature would need: a stored list of
  per-slot offsets (in beats) plus a strength, applied where `Quantize` computes
  its `target`; the extraction side (read the timing of an existing clip into
  such a list) is the real work. `QuantizeOpts` and the `Quantize` signature were
  kept free of UI types so a `Groove` field can be added beside `swingPct`.
- **Pitch-bend / mod-wheel transforms**: out of scope here (they are CC events,
  not notes, and belong with the CC-lane work).
