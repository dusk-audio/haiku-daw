# Record — M4.2 the loop is walked inside the running graph

Branch `feature/seamless-loop` off `master` (`40dcc96`). Spec:
`docs/agent-prompts/36-seamless-loop.md`. Plan: `docs/PLAN_1.0.md` §M4.2
(`24-continue-1.0-ENTRY.md` §5, T5's second item), following M4.1
(`29-engine-graph.md`).

## The defect this closes

`MainWindow`'s 60 Hz pulse saw the playhead reach `transport.loopEnd`, rewound it
and called `TransportController::StartPlayback()` — a position-changing rebuild.
That detaches the active graph (the RT renders silence, the playhead stands
still) and builds a whole new one on the worker: every clip file re-opened, every
disk thread restarted, every ring primed, the FX rebuilt. **Once per loop pass,
for as long as the loop runs**, with a hole the length of a build. On the
32-track / 320-clip project a build is seconds, so a one-bar loop was unusable.
M4.1's record says so in as many words: "M4.1 moves it off the window thread…
the plan's 300 ms *audio* budget needs M4.3", and the loop wrap was the case it
did not fix.

## What landed

**The loop is part of the graph** (`Engine::Graph::fLooping/fLoopStart/fLoopEnd`,
resolved at build from the snapshot the builder already had, through
`LoopActive()`). Fixed for the graph's life — which is what lets the RT callback
and every disk thread agree on the seam with **no handshake at all**.

**The RT wraps by splitting the callback** (`Engine::Graph::Render` is now a
segment driver over the old body, renamed `RenderSegment`). At the loop end the
block is cut at exactly that frame: `[pos, loopEnd)` renders at the old position,
`[loopStart, …)` for the rest of the block. Sample-exact, one graph, one
callback, no rebuild and no device gap. Everything inside a segment — fades,
automation, MIDI notes, the click, the meters — is rendered at the position it
actually plays at, and FX chains, delay lines and live voices are *not* reset:
the loop is a jump, not a new graph, so a reverb tail carries across the seam.

**The disk threads produce in playback order across the seam** — the ring is a
FIFO, and under a loop that order has a seam in it. `StreamLoop.h`'s
`ProduceCursor` walks the passes a clip plays (the first from the clip's own seek
position to the first cut, then one per loop pass, intersected with the clip's
window); the disk thread seeks at each pass start, drops the resampler's carried
state (`Resampler::Reset`, new) and never writes past a pass end. Pre-rolling the
loop start is then nothing special: the disk thread was already ahead of the
playhead, and the frames it pushes next are exactly the frames the callback is
about to ask for. **No second ring, no second graph, no extra stream memory.**

**The window stopped wrapping.** The pulse's playback-wrap branch is gone; it
displays the playhead the engine reports, including just after a wrap.

**A loop-range edit re-positions the engine** (`kMsgTransportRange`, posted by
`ToggleLoop`, `LoopBetweenMarkers` and the ruler drag's commit; handled like
`kMsgSeek`). The range is part of the graph, and the in-place join counts wraps
to know how many passes a build spanned — which only means the same thing when
both graphs walk the same loop. A range change is therefore a position change,
not a content edit; an edit that leaves the range alone (a clip moved, an insert
added) keeps M4.1's seamless in-place join, now loop-aware
(`LoopRebaseSkipFrames`, which walks the looped path: first pass tail + complete
passes + the current pass's head).

**Loop recording still re-aligns in the window**, deliberately: the record graph
is built with `noLoop` so its playhead keeps running through the loop end and the
pulse's existing `ph >= tr.loopEnd` seam check fires exactly as before. The take
layout is measured from the moment the engine rolls (RecordPlan) and re-rolling
per pass is what keeps passes aligned with the capture; making that seam seamless
needs the take layout's own pass and is not this task.

| Piece | Where |
|---|---|
| the pass walker, the block splitter, the loop-aware join | `src/engine/StreamLoop.h` (new, kit-free) |
| pass-driven producer, pass seeks, resampler reset | `src/engine/Engine.cpp` (`DiskLoop`, `SeekToPass`, `Prepare`), `src/engine/Resampler.{h,cpp}` |
| the segment driver, the graph's loop, the wrap counter | `src/engine/Engine.{h,cpp}` (`Render`/`RenderSegment`, `fLooping`, `Engine::fWraps`/`Wraps()`) |
| the record path built unlooped | `src/ui/TransportController.cpp` (`noLoop`) |
| the pulse no longer wraps; the range message; the test hook | `src/ui/MainWindow.{h,cpp}`, `src/ui/UiMetrics.h` |
| the loop edits that re-position | `src/ui/TimelineView.cpp` |
| host suites | `tests/stream_loop_tests.cpp` (new, 104 checks) |
| the functional flow | `tests/ui_functional_tests.cpp` (`TestSeamlessLoop`) |

## Verification

Every number below is from a completed run on this branch's final tree.

- Host `build-host`: build exit 0, ctest **67/67** (66 before; the new suite is
  the one addition).
- `b-asan` (`-DDAW_SANITIZE=ON`): build exit 0, ctest **67/67**.
- `sh scripts/haiku_syntax_check.sh`: **73 OK, 4 FAIL** — the four FAILs are
  `FlacSink/FlacSource/VorbisSink/VorbisSource.cpp` and they fail identically on
  pristine `master` on this host (the cross compiler has no FLAC/Vorbis headers;
  verified by stashing the branch and re-running). `Engine.cpp`,
  `TimelineView.cpp`, `TransportController.cpp`, `MainWindow.cpp` and
  `tests/ui_functional_tests.cpp` are all OK.
- VM `build` (LV2 on): ctest **69/69**; `build-off` (`-DDAW_LV2=OFF`): ctest
  **64/64** — both including `ui_functional_tests`, both green, on this branch's
  tree as it stood for the runs before the last comment-only edits. Every other
  suite in both dirs is green in every run. The one exception is the FX-drag
  check covered below, which then started failing in both dirs and on master's
  binary alike.
- VM `ui_functional_tests`: **815 checks**, 0 failures on all eight runs before
  that. The new
  flow reports:

      seamless loop ...
        loop: 3 wraps in 3006 ms (1002 ms per pass)
        loop: playhead 96384, loop [96000, 144000)

  A 1 s loop wrapping three times took 3006 ms of wall clock — **1002 ms per
  pass**, i.e. the loop's own length, not the loop's length plus a graph build.
  Across those three wraps `GraphsPublished`, `PlayersOpened` and `PlayerStarts`
  did not move by one, `IsPlaying` stayed true, and the reported playhead stayed
  inside the loop: nothing was re-opened, nothing was re-built, and the device
  never stopped. That is the whole claim of M4.2, measured.

### One check fails on this VM — on master's binary too

`TestFxRetargetFlush`'s synthetic drag (`heard != LimiterDesc().p(0)`) began
failing late in the session, in every run. Attributed by building **master's own
tests** on the VM and running them beside this branch's:

| Binary | Result |
|---|---|
| this branch (final tree) | 815 checks, **1 failure** — the drag check, twice in a row |
| `origin/master` as this VM has it (`a004507`; two merges behind this branch — PRs #4 and #6) | 775 checks, **the same single failure**, twice in a row |

The check posts a synthetic mouse drag to the FX window and retries four times
before failing. It is in code this branch does not touch (the loop change
reaches `Engine`, `TimelineView` and the record path, not the FX panel), and the
two rows say the same thing: the VM fails it with or without this branch. The
same binary passed it earlier in the day, so it is not deterministic. One other
one-off failure was seen in ~12 runs, also in a test this branch does not touch:
the humanize draw (`ui_functional_tests.cpp:1086`, stochastic — it waits 5 s for
the notes to differ). **Marc: a VM-side check is red, and it is not this
branch's.**

## Mutation checks (each broke the code, watched the guard fail, restored)

- `LoopRebaseSkipFrames`: counting `wraps` complete passes instead of `wraps-1`
  → **11 failures**.
- `ProduceCursor`: the first pass ignoring the loop cut (`clipEnd` instead of
  `loopEnd`) → **14 failures**.
- `SegmentFrames`: never splitting the block (`return frames`) → **4 failures**.
- `LoopActive`: dropping the block-length rule → **1 failure**.

The functional flow's own guard is the counter trio: mutating it means either
removing the engine's wrap (`Engine::Graph::Render` not wrapping) — which the
`WaitFor(wraps >= 3)` catches — or restoring the pulse's wrap branch, which
`EngineGraphsPublished() == swaps0` catches.

## What the screenshot pass found

No window's drawing or layout changed, so this is a check for regressions rather
than a design pass. The pass was run in **both** modes (`DAW_UI_SHOTS`, System
and `DAW_UI_THEME=dark`), and the new shot reviewed at full size in both:

- `63-seamless-loop` (System) — the transport rolling, the loop band drawn on
  exactly `[2 s, 3 s)` (measured in pixels: the band starts on the bar-2 line and
  is 47 px wide at 47 px/s), the playhead inside it at 2.16 s, the readout
  agreeing (`2.1 0:02.160`). Nothing overlaps, nothing is clipped, the lanes and
  the ruler are clean.
- `63-seamless-loop` (dark) — same window, the dark palette's loop band, the
  playhead inside it, no artefacts.
- Compared `62-engine-graph-swap` against `63-seamless-loop` (the shot before the
  loop is switched on): the only difference is the loop band, which is what the
  test turned on. The tempo/meter text in the ruler is unchanged between them
  (its top row sits under the toolbar in both — pre-existing, M1.4's area, not
  touched here).

## Left for Marc / later packages

- **Ears, on 192.168.1.186:** loop a busy project and listen across the seam —
  nothing is audible on the VM, so the *audible* seamlessness is a listening
  check. What is verified here is that nothing is rebuilt, the wrap lands on the
  frame, and every pass takes the loop's own time.
- **Seek while playing** still goes through `StartPlayback` (a position-changing
  rebuild). The in-engine seek needs the producer half of this work *plus* a
  re-prime handshake — after a jump the producer cannot infer, the RT must not
  read stale ring content. Its own task.
- **The loop-record seam** still re-rolls the engine per pass (above).
- A loop-range change during playback is a short rebuild (a hitch), exactly as it
  was before this change; making *that* seamless needs the join to model a range
  transition, which the pass count cannot express today.
- A loop shorter than one output block is treated as "not looping"
  (`LoopActive`): walking it would split a callback into an unbounded number of
  segments. It plays through to the end of the material instead.

## On merge

The status table in `docs/agent-prompts/README.md` wants one row for this
branch (it is not added here: the table records merges, and this branch is a PR
until Marc says otherwise):

> M4.2 seamless loop | **Merged** — the loop is walked inside the running graph:
> the callback splits at the loop end (sample-exact) and the disk threads
> produce each clip's audio in playback order across the seam, so a pass costs
> the loop's own length (measured on the VM: 1002 ms per pass of a 1 s loop)
> and `GraphsPublished`/`PlayersOpened`/`PlayerStarts` do not move. A loop-range
> edit re-positions the engine; loop recording still re-aligns per pass. Host
> 67/67, VM `build` 69/69 + `build-off` 64/64, `ui_functional_tests` 815 checks
> (one pre-existing FX-drag check is red on the VM, on master's binary too).
> Record `36-seamless-loop-PR.md`
