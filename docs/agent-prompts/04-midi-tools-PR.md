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
| `tests/midiops_tests.cpp` (new) | 128 checks: strength/swing/triplets/tempo changes, humanize determinism, legato, clamps, window rules, idempotency. |
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

**The settings dialog only applies on its button.** Two of its controls are
settings, not edits, so they carry no message at all — a `BPopUpMenu` is
radio-mode by default, which means choosing a grid marks the item *and* invokes
it, so with a message attached picking a grid would quantize on the spot with
the strength and swing the user had not set yet (same for ticking the checkbox).
Reading Haiku's `MenuItem.cpp` is what settled it: `SetMarked` happens in
`Invoke` under `IsRadioMode`, before the message is sent, so dropping the
message keeps the radio mark and the menu field's label while sending nothing.

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

The branch was rebased onto master after package 07 merged (2026-10-09), so
every count below is the rebased branch — master + 07 + this package:

```
cmake --build build-host                       # exit 0
ctest --test-dir build-host                    # 49/49  (48 on merged master + midiops_tests)
./build-host/midiops_tests                     # 128 checks, 0 failures
./build-host/model_tests                       # 237 checks, 0 failures
cmake -B b-asan -DDAW_SANITIZE=ON && cmake --build b-asan -j8 && ctest --test-dir b-asan   # 49/49, clean
sh scripts/haiku_syntax_check.sh <all Haiku-only sources>      # 0 FAIL (see below)
# On the VM (2 vCPU, -j2), both configurations, configure and build exit 0:
ctest --test-dir build                         # 50/50   (LV2 on)
ctest --test-dir build-off                     # 46/46   (-DDAW_LV2=OFF)
```

**Mutation testing** — every mutation was applied, the test run, and the code
restored. The last column says which suite caught it:

| Mutation | Result |
| --- | --- |
| swing divisor 3 -> 6 | caught (7 checks) |
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
| **strength interpolates in FRAME space** | caught (1) — added after the review |
| **swing offsets from the note instead of snapping** | caught (20) — added after the review |
| **`clipStart` ignored in the beat lookup** | caught (2) — added after the review |
| **length measured from the pre-clamp start** | caught (1) — added after the review |
| **NaN strength not sanitised** | caught (1) — added after the review |
| **jitter arithmetic in 32-bit** | caught by UBSan only (see below) |
| **commit applies the list reversed / zeroed / duplicated** | caught (6 / 2 / 2) — added after the review |

One mutation is deliberately caught by a *sanitizer* rather than by a check:
32-bit jitter arithmetic wraps to the same value, so no assertion can see it;
the UBSan build reports it as a runtime error. `midiops_tests` carries a
maximum-width jitter case (velocity ±INT_MAX) precisely so the sanitizer build
has something to trap on.

Two early mutations also got away and changed the code, not just the tests: the
region-growth test originally used a region the *edit* path had already grown
(a growing `Do()` was indistinguishable from a correct one), and the clip-start
case asserted a value the clamp floor would produce anyway. Both are fixed.

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

## The adversarial review, and what it changed

Four lenses (transform math, command/integration, UI wiring, test vacuity) read
the whole diff. Every finding below was reproduced before it was fixed.

**Fixed in the model** (`src/model/MidiOps.cpp`):

1. **The new length was measured from the pre-clamp start** (medium). With
   "Quantize note ends" on, a note whose snapped start is clamped at the region
   head kept a length measured from where it had *not* landed, so its end sat up
   to half a grid step past the line the code itself had computed — and a second
   quantize moved it again (a 1-frame note, in the reviewer's worst case). The
   start now clamps before any length is derived from it. Pinned by a new
   edge case that asserts the absolute end frame and idempotency.
2. **NaN strength or swing was not sanitised** (low): both comparisons are false
   for NaN, so it reached `FrameAt(NaN)` and every affected note landed on frame
   0. "Not a number in range" now reads as *no movement*, the safe default.
3. **The jitter arithmetic could overflow `int`** (low): a draw of `h % span`
   exceeds INT_MAX for the widest jitters, and subtracting in `int` is signed
   overflow — UB, reported by the project's own UBSan build. Done 64-bit now,
   with the maximum-width test above.

**Fixed in the UI:**

4. **The roll's own edit could leave its window stale** (medium). Drawing a note
   past the region end grows the region in the model, but the roll's `fClipLen`
   stayed put — so the very note the user had just drawn was the one a later
   quantize skipped (and a note near the old edge lost a tail the region now had
   room for). `Apply()` now grows `fClipLen` by the same rule the command uses.
   A region resized from the *timeline* while the roll is open is still stale —
   that is the pre-existing snapshot editor, see "Known, not fixed".
5. **Holding `q` stacked one undo step per auto-repeat** (low), and at strength
   < 1 each one moved the notes further, so nothing collapsed them. Repeats are
   ignored now, the way the project already filters them for space.
6. **A transform could land in the middle of a note drag** (low): the drag
   replays `fDragOrig` on the next mouse move, so the transform survived for the
   unselected notes and was overwritten for the dragged ones — a state no single
   action produced. `RunMidiOp` refuses while a drag is live.

**Test gaps closed** (all nine from the vacuity lens, each proven by a mutant
that passed before): the commit's applied list is now asserted field by field
instead of by size and one pitch; the clip-start case asserts the absolute frame
so the clamp floor cannot stand in for it; strength 0.5 is pinned across a tempo
change (frame-space interpolation passes everything else); swing is pinned for
OFF-grid notes and combined with strength; humanize's ceiling clamp has its own
fixed-seed case; the grid enum's numeric order (the wire contract with the
settings dialog) and every grid name are pinned; `ScaleVelocity` pins rounding;
and one check that compared lists of different lengths was replaced with the
comparison it meant.

**Judged not defects** (recorded, not changed): `MidiOpName((MidiOp)op)` casts a
forged int32 to an enum, which is formally UB, but the switch default handles
every value and only a hand-built message can reach it; legato takes its "next
note" from the whole list including out-of-window notes, which is intended (a
silent note still ends a phrase) and is what the header says; and the review's
own note that a partial-strength quantize is naturally non-idempotent — that is
the feature, not the bug the idempotency requirement is about.

## What is NOT verified

| Claim | State |
| --- | --- |
| Everything in `src/ui/` | **Compiles (cross-compiler); never clicked.** The popup menu, the settings dialog, the `q` key and the commit path need a window. Click list below. |
| VM `build` + `build-off` + ctest | **DONE 2026-10-09**: configure and build exit 0 in both configurations, 50/50 (LV2 on) and 46/46 (`-DDAW_LV2=OFF`) — the rebased branch, i.e. master + package 07 + this package. |

### Click list (piano roll window) — mostly automated now

**Automated on the target** (`tests/ui_functional_tests.cpp`, run by `ctest` on
the VM): the roll opens on a real region; the settings message its dialog posts
quantizes and leaves one named undo step; the `q` key does the same through a
dispatched `B_KEY_DOWN`; and Humanize, Legato, Transpose and Velocity each run
through the entry point their menu item calls, asserting the model they leave
and the undo name the user would read.

**Still manual** (no honest way to synthesize): the popup menu itself — opening
the MIDI button and picking an item (the transforms behind it are covered); the
mouse drags (a drag previews on the model and commits on mouse-up); holding `q`
(auto-repeat is a real key stream); and how any of it looks.



Run it with the transport stopped (or expect the change to be heard from the
next Play): a piano-roll edit is not live — the engine rebuilds from the model
on Play — and a transform is an edit like any other.

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
10. Hold `q` down for a second or two.
   -> Expected: ONE quantize (one undo step), not a stream of them.
11. Draw a note past the region's right edge with the Pencil (the region grows
   to cover it), then press `q`.
   -> Expected: that note is quantized too — it is inside the region now.

## Known, not fixed

- The transform clamps to the region window **as the roll saw it when it
  opened** (`fClipLen` — now at least kept in step with the roll's own growing
  edits — plus the pre-existing `fClipStart` and `fTempo` copies). Resize or
  move the region on the *timeline* while its piano roll is open, and a
  transform will still use the old window and grid phase. This is the snapshot
  editor the roll already is — a note drawn in it lands by the same stale origin
  — and the fix is a "region changed" push the roll does not have. Stated rather
  than buried; it is not a new class of staleness.
- Humanize's seed comes from `system_time()`, so reproducing a specific take
  means calling `Humanize` directly (the function is deterministic from its
  seed; the UI just does not keep the seed). A groove/save-the-seed feature
  would change that.

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
