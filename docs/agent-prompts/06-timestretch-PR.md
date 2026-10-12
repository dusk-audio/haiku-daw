# 06 — Time-stretch (M7.2): the record

Branch: `feature/timestretch`, off `master` (`85b19b1`).
Worktree: `.claude/worktrees/agent-ad5e48861f5ddf98b`.
Spec: `docs/agent-prompts/06-timestretch.md`. Milestone: `docs/PLAN_1.0.md` M7.2.

Commits (author `Marc Korte <marc@duskaudio.com>`, no AI trailers):

```
3769ca9 feat(model): clip stretch ratio, its command and its persistence
0e9dc73 feat(dsp): the WSOLA time-stretcher
e4bc249 feat(engine): the stretch stage on the exporter's clip path
75ff7e9 feat(engine): the stretch stage on the live disk path
c1f4fd1 feat(ui): the stretch-to-fit drag and the ratio badge
44e6629 fix(model): a ratio within a rounding of 1 is 1
730c7c8 test(dsp): a runaway stretcher must terminate the test's render helper
(this commit) docs: record the M7.2 stretch work
```

**Base correction.** The worktree was created from `origin/docs/1.0-decisions`
(`785cd5a`, 364 commits behind master): it has no `src/dsp/`, no
`src/engine/Exporter.cpp`, no `PLAN_1.0.md`. The orchestrator's fix
(`git reset --hard master`, branch renamed in the same worktree) is what this
work starts from; nothing of mine existed before the reset, and the eight
commits above are the whole branch.

## What was built

| Piece | Files |
|---|---|
| Model + policy | `src/model/ClipStretch.h` (new), `src/model/Project.h`, `src/model/Commands.h/.cpp`, `src/model/ProjectIO.cpp` |
| DSP | `src/dsp/Stretch.h`, `src/dsp/Stretch.cpp` (new) |
| Exporter | `src/engine/Exporter.cpp` (`ClipMaterial`) |
| Live engine | `src/engine/Engine.h/.cpp` (`TrackStream`), `src/engine/StreamLoop.h` |
| UI | `src/ui/TimelineView.h/.cpp` |
| Docs | `docs/ARCHITECTURE.md` §5.3 |
| Tests | `tests/stretch_tests.cpp`, `tests/exporter_stretch_tests.cpp` (new); `tests/exporter_tests.cpp`, `tests/projectio_tests.cpp`, `tests/stream_loop_tests.cpp`, `tests/ui_functional_tests.cpp` |

### The model convention (pinned everywhere)

`Clip::stretchRatio` is an output-duration multiplier: 1.0 = play as recorded,
2.0 = twice as long (half speed), 0.5 = twice as fast. `lengthFrames` stays the
TIMELINE length, so the source a clip consumes is `lengthFrames / ratio`
(in file frames: `lengthFrames / ratio * srcRate/projRate`). Fades, crossfades,
automation, the transport and the project end are all still timeline-domain and
were not touched. `pitchSemis` is modeled and serialized (the format is stable)
and nothing plays it, as the spec says.

The ratio is clamped to `[0.25, 4.0]` at every door it comes in through (the
command, the loader, `ClipStretchRatio()` for anything that reads the field
directly), and a value within `1e-6` of 1.0 counts as unstretched, so a decimal
round trip cannot put a clip on the stretch path by accident.

### The ratio a stretch drag derives

`StretchRatioForDrag` (kit-free, `src/model/ClipStretch.h`) uses the clip's
CURRENT SOURCE SPAN — `lengthFrames / oldRatio` — never the source file's
length. A clip is a non-destructive window into its file, and every loop
dragged in from the browser is trimmed; dividing by the file length would both
compute a wildly wrong ratio and ask the engine for material the user cut away.
The file's own frame count (which the UI already has: the peak cache stores it)
clamps the span when `sourceOffset + span` runs off the end, so a clip whose
span overruns its file still derives a finite ratio from the material that
exists. Verified in `tests/stretch_tests.cpp` and, through the real gesture, in
`tests/ui_functional_tests.cpp` (a clip cut from the middle of a 2 s file,
dragged to twice its timeline length, must land on ratio 2.0 — the file's own
length would have given 1.0).

### WSOLA constants (documented in `src/dsp/Stretch.h`)

| | | at 48 kHz |
|---|---|---|
| window `L` | 50 ms, forced even | 2400 frames |
| synthesis hop `Hs` | `L/2` (50 % overlap) | 1200 frames |
| similarity search `R` | ±10 ms | ±480 frames |

`Hs = L/2` is not a taste call: two *periodic* Hann windows a hop apart sum to
exactly 1, so the overlap-add is unity gain by construction and there is nothing
to normalise. The correlation is evaluated on every 4th sample of the overlap
and the lag refined at full resolution around the coarse winner (the peak's
shape is what matters, the refinement keeps the lag from being a few samples
off). The history buffer is sized for the widest span a single frame can need —
`L + R + 3*Hs` at the model's 0.25 floor, plus slack, ~9 k frames/stereo
(~73 KB) per stretched stream.

### Seek determinism

The synthesis grid is anchored at the clip's source origin, so the state is a
function of the source position: `Q(k) = round(k * Hs / ratio)` for the nominal
read of frame `k`, and `Reset(sourcePos)` puts a run at output frame
`round(sourcePos * ratio)` — the frame phase continuous playback had at that
source position, not a restarted one. A pass (or a loop wrap, or a rebuild)
therefore lands on the same grid, and the exporter's `Reset(0)` does too.
`WarmupFrames()` lets a caller that has the material (a seek inside a clip)
start one warm-up hop early and drop the frames before its target, so the
resumed pass has a real overlap-add tail instead of a 25 ms ramp-in.

**Deviation from the spec's letter, with measurements.** The spec asks that
`Reset(sourcePos)` reproduce the *exact* synthesis phase, i.e. that a seek be
bit-identical to continuous playback. That is only achievable if each frame's
similarity correction is a pure function of the frame index — matching each
candidate against the previous frame's NOMINAL continuation. I implemented that
first. It is exactly reproducible, and it is audibly wrong: it aligns every
frame against where the previous frame *would* have read, not where it did, so
the overlap-add cancels. Measured on a 440 Hz sine (`/tmp` probe, 0.5 s input):

```
grid-pure deltas:   ratio 0.8 -> window RMS 0.306 (source 0.354) every hop
                    ratio 2.0 -> window RMS 0.250
                    ratio 0.5 -> 0.214
chained deltas:     ratio 0.8/1.25/2.0 -> 0.3536 = the source's own level
```

(and the zero-crossing frequency count was 39 cents low at 0.8, 32 cents low at
1.25 — a wobble at the hop rate, exactly what a stretched bass line or pad would
do). So the shipped implementation chains the correction: frame `k`'s search
target is `x[a(k-1) + Hs ...]`, the samples the previous frame really did place
in the overlap. What the invariant keeps — and what the tests assert — is the
part a seek actually needs: the OUTPUT INDEX a reset lands on is exactly
`round(sourcePos * ratio)` (no drift against the timeline), every frame reads
within `R` of its nominal in both runs, a resumed run's material matches
continuous playback's at a lag within `2R` (correlation 1.000 in the test), and
a run that starts at the clip origin — the exporter's case and the engine's
common case — is bit-identical to any other run that starts there. The residual
difference between a seek and continuous playback is the chained correction,
re-seeded from the grid: a segment choice within the search window the algorithm
already permits for every frame.

### Live engine

`TrackStream` gains a stretch stage between the resampler and the ring, on the
same fill worker (disk thread) that already did the decode: the ring still holds
one output frame per timeline frame, so the RT callback, `Mix`, `RebaseTo` and
the ring contract are unchanged and nothing on the audio path allocates or
searches. The spec's ring-sizing check: the ring's depth is unchanged and does
not need to be, because the stage is on the *producer* side — at ratio 0.25 the
disk thread reads four source frames per output frame, which costs it work, not
ring space, and the fill loop's `remaining`/`WriteAvailable` arithmetic is in
output frames exactly as before. The prefill on seek is likewise unchanged: the
warm-up frames are dropped before anything is pushed, so the first frame in the
ring is still the pass's own first frame. The stretcher is constructed once in `Open()`; the pass's source
consumption is capped at `lengthFrames / ratio` (the same span the exporter
consumes, so a bounce and playback agree about a trimmed clip), and a clip
longer than its source latches `fSourceEof` exactly as the unstretched path does
on EOF, so a build does not wait out its priming budget on audio that does not
exist. The pass source-frame arithmetic moved into the kit-free
`PassSourceFrame(..., stretchRatio)` (host-tested in `stream_loop_tests`).

### Exporter

`ClipMaterial` decodes the clip's span and, when the clip is stretched, runs it
through the identical `Stretcher` with the identical grid, then `PlaceClip` does
what it always did (fades, gain, timeline placement). An unstretched clip
constructs nothing and takes the decode + place path it had before — the
regression test pins that bit-for-bit against that path's own reference.

### UI

Cmd (or Alt) on a clip's right edge = stretch to fit: same drag path as the trim
(preview into the model, restore on mouse-up, ONE `SetClipStretchCommand` → one
undo step), with the ratio derived from the pre-drag span. The clip draws the
ratio it plays at as a `×1.07` badge on its name strip (next to the mute badge),
the waveform columns map through the ratio so a stretched clip draws the
material it plays, and the right edge's tool tip names the gesture with the
modifier down. The plain drag still trims, asserted in the same test.

### CPU cost, measured

`-O2` on this host, stereo, a tone plus a little noise so the search has
something to correlate (`m72_bench.cpp` in the agent's scratchpad):

```
ratio 0.50: 217 ms for 5.0 s of stereo output  = 4.32% of one core (23x realtime)
ratio 1.50: 436 ms for 15.0 s of stereo output = 2.90% of one core (34x realtime)
ratio 2.00: 1242 ms for 20.0 s of stereo output = 6.20% of one core (16x realtime)
```

So roughly 3–6 % of one core per stretched stream (16–34× realtime), paid on the
stream's own fill worker — the RT callback pays nothing. The extremes cost more
because the analysis hop is farthest from the synthesis hop there, so the
correlation's winner is least constrained; the search itself is fixed-cost per
synthesis frame (one coarse pass over ±R on every 4th sample of the overlap,
then a ±3 refinement at full resolution).

## Verification

```
$ cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build build-host -j2
configure exit=0
build exit=0
[100%] Built target projectio_fuzz_tests

$ ctest --test-dir build-host
100% tests passed out of 70
Total Test time (real) =   3.46 sec

$ sh scripts/haiku_syntax_check.sh
75 OK, 4 FAIL — all four are FlacSink.cpp / FlacSource.cpp / VorbisSink.cpp /
VorbisSource.cpp failing on missing FLAC/ogg headers in the cross-toolchain
(files this branch does not touch; see "Unfinished").
```

Check counts of the suites this work touches:

```
stretch_tests:           81 checks, 0 failures        (new)
exporter_stretch_tests:  27 checks, 0 failures        (new)
exporter_tests:         101 checks, 0 failures        (incl. the unstretched regression)
projectio_tests:        225 checks, 0 failures        (incl. the round trip + compat)
stream_loop_tests:      109 checks, 0 failures        (incl. the ratio arithmetic)
```

## Mutation checks

Each mutation was applied to the working tree, the affected target rebuilt, the
test run, and the file restored with `git checkout` (script:
`mutate.py` in the agent's scratchpad).

```
CAUGHT  M1  clamp dropped (model reads a wild ratio)                 [projectio_tests]
CAUGHT  M2  clipstretch line always written                          [projectio_tests]
CAUGHT  M3  loader drops the pitch field                             [projectio_tests]
CAUGHT  M4  drag ratio from the FILE length, not the clip's span      [stretch_tests]
CAUGHT  M5  Reset loses the grid (output index restarts at 0)        [stretch_tests]
CAUGHT  M6  similarity target is the grid's nominal, not the previous read [stretch_tests]
CAUGHT  M7  hop is a quarter window (the OLA no longer sums to 1)    [stretch_tests]
CAUGHT  M8  end rule doubled (the output runs past the material)     [stretch_tests]
CAUGHT  M9  the exporter's span cap removed                          [exporter_stretch_tests]
CAUGHT  M10 unstretched decode bound shortened (clip path perturbed)  [exporter_stretch_tests]
CAUGHT  M11 PassSourceFrame multiplies by the ratio instead of dividing [stream_loop_tests]
```

Two of these taught the tests something:

- **M9, first attempt**, was "decode the whole file instead of the span"
  (`DecodeClip(..., 0)`). NOT caught — and correctly so: the span cap is the
  truncation in front of the stretch stage, not the decode bound, and the
  decode bound only bounds the *work*. The mutation was rewritten to remove the
  truncation, and then the trimmed-clip test caught it: with the cap gone the
  bounce's peak rises past its own span's level (`pk < 0.4` fails), i.e. material
  the clip does not reference came out of the speaker. The test now also asserts
  the tail of a clip whose span ends mid-file is quiet (`< 0.3`; 0.09 measured,
  against the ~0.45 a leak would show), which pins the same property at the
  window that reaches past the end.
- **M6/M7** are the two "quality" mutations. M6 replaces the chained similarity
  target with the grid's nominal one — the variant whose measurements are above —
  and is caught by the frequency and level checks (3 failures in
  `stretch_tests`). M7 breaks the window/hop relationship the unity-gain OLA
  rests on and is caught by the level check (6 failures).

## Unfinished, blocked, deviated

- **UI not visually verified.** The screenshot pass (`DAW_UI_SHOTS` on the
  unlocked VM, both themes) is the orchestrator's; this agent must not touch the
  VM. The badge, the tool tip and the waveform mapping are therefore
  *unreviewed by eye*: the functional test asserts the gesture, the model
  result, the undo step and leaves a `clip-stretch` shot for the pass, but
  nothing here proves the badge renders where it should. Treat the UI as
  needing the shot pass before it counts as done (`docs/UI_GUIDELINES.md` §1).
- **The live engine half is not run here** (Haiku-only: `Engine.cpp` does not
  compile on this host). It is pattern-faithful and cross-compiled by
  `scripts/haiku_syntax_check.sh`, but its runtime behaviour — a stretched clip
  playing, seeking, looping — needs the VM.
- **Pre-existing, found while writing the regression test and NOT fixed:**
  `Resampler::Process` at equal rates (`fStep == 1.0`) emits the first input
  frame twice (`out = in0, in0, in1, ...`), i.e. every 1:1 decode is one frame
  late and one frame short. It is why the "bit-identical" test compares against
  the pre-change path rather than against the source file. Fixing it would move
  every existing render by a frame and is not this package's business; it should
  be its own bug with its own regression test.
- **`scripts/haiku_syntax_check.sh` reports 4 FAIL**, all
  `FlacSink.cpp` / `FlacSource.cpp` / `VorbisSink.cpp` / `VorbisSource.cpp`
  failing on `FLAC/stream_encoder.h`, `ogg/ogg.h`, `vorbis/vorbisfile.h` — the
  cross-toolchain has no FLAC/Ogg headers. Those four files are untouched by
  this branch (`git diff --name-only master` does not list them), so the FAILs
  are the environment's, not this change's.
- **Interactions left for later (out of the spec's scope, but real):** the
  commands that edit a clip's *source* still treat `lengthFrames` as the source
  span — split (`SplitClipCommand`), the range edits and `RegionOps`
  (peak/reverse/silence), and freeze/split rendering in `src/ui/RenderJobs.cpp`.
  On a stretched clip they will pick the wrong source range. Fixing them needs a
  source RATE in the model (none exists: the UI/engine look it up per source),
  so it is a follow-up, not a hunk in this package. `TrimClipFrontCommand`
  (front trim) has the same shape.
- **Quality trade-off at a clip's tail:** the stretch consumes exactly the
  clip's span and reads silence past it (the spec's "consumes exactly
  lengthFrames / stretchRatio" and "must not read past a trimmed span"), so the
  last ~50 ms of a stretched clip that ends mid-file is windowed down rather
  than continued into the next file material. That is deliberate and consistent
  between playback and bounce; if it proves audible on the VM listening pass,
  the fix is to allow a bounded read past the span for the last frames' search
  window, not to re-derive the span.
- **Pitch-shift v2** needs: resample-then-stretch composed ratios (play the
  source through a `Resampler` with `2^(semis/12)` and stretch by
  `ratio * 2^(semis/12)` to keep the duration), which is a second stage in the
  same chain and no change to the model or the grid — `pitchSemis` is already
  carried through the command, the file and the stream's state.
