# Record — M4.1 engine graph off the window thread (package 29)

Branch `feature/engine-graph` off `master` (`6d88063`). Spec:
`docs/agent-prompts/29-engine-graph.md`. Task: `24-continue-1.0-ENTRY.md` §5 T5
first item; plan item M4.1 (`docs/PLAN_1.0.md`).

## What changed

- **`src/engine/GraphSwap.h`** (new, kit-free): the RT-safe half of a rebuild —
  one `atomic<Graph*>` the callback loads once per block, a publish that
  retires whatever it replaced, and a reclaim thread that frees a retired graph
  only once the owner says no RT thread can still hold it.
- **`src/engine/StreamRebase.h`** (new, kit-free): `RebaseSkipFrames()`, the
  arithmetic that re-aligns a clip stream when a graph is swapped in while the
  transport keeps rolling.
- **`src/engine/RingBuffer.h`**: `Skip(n)` — the consumer-side O(1) bulk
  advance the rebase uses.
- **`src/engine/Engine.{h,cpp}`**: everything a rebuild replaces now lives in
  `Engine::Graph` (streams, buses, node buffers, mix order, per-node peaks,
  master chain + insert state, scratch/monitor buffers, live-voice pool,
  metronome, tempo map, loudness, end frame). `Engine` keeps the transport
  atomics, the monitor/live toggles, the MIDI routes, the watched-insert and
  meter publishing storage, the device, and the graph slot. A request snapshots
  the project on the caller's thread and hands it to a builder thread; the
  builder publishes the finished graph with one atomic exchange and records the
  result (`LoadsCompleted`/`LastLoadStatus`). `FillBuffer` loads the pointer
  once and renders the block from it — no graph, no playhead advance.
  `BSoundPlayer` is persistent (`EnsurePlayer`), created only when the
  requested rate/buffer differs from the live one.
- **Rebuild modes**: `LoadMode::NewPosition` (play, seek, loop wrap, record
  start, monitor start) detaches the active graph at request time — silence and
  a standing playhead until the new one lands, which is what a seek did before,
  without the frozen window. `LoadMode::InPlace` (a structural edit while
  playing) leaves the old graph playing and advancing; the new graph joins at
  the current playhead through the per-stream rebase, so the edit is heard with
  no gap and no drift.
- **`src/ui/TransportController.{h,cpp}`**: the engine is created on first use
  and kept (its device survives every rebuild); play/seek/loop-wrap/rebuild
  request instead of loading; `PollEngineLoad()` (called from the 60 Hz pulse)
  is what stops the transport with the old wording when a build failed. The
  record start still WAITS for its graph — the take's alignment is measured from
  the moment the engine rolls, so the capture must not begin early.
- **`src/ui/RecordController.cpp`**: the idle-monitor path uses the same engine
  (`SetMonitorOnly(true)` + a synchronous load + start) instead of replacing it.
- **Tests**: new host target `engine_graph_tests` (reclaim semantics under load,
  rebase math edges, `RingBuffer::Skip`); `ui_functional_tests` gains
  `TestEngineGraphSwap` and a tightened `TestBigProjectPlayback`.
- **Off-RT graph safety** (`GraphPin`, `Engine.h`): the reclaimer's two-boundary
  rule is a proof about the RT thread only, so every off-RT user of the active
  graph (UpdateMix, SyncFx, SetFxParamLive, SetFxTempo, PublishFxWatchNow,
  TrackPeakL/R, SetMidiRoutes, Start/Stop) now counts itself in before loading
  the pointer, and the reclaim predicate waits for that count to be zero. A
  window thread preempted for two audio blocks inside one of those calls would
  otherwise have been reading a graph the reclaimer had freed. Found by review
  while the build was running; no test fails without it (the window is narrow),
  so it is named in the record as review-covered, not test-covered.
- **`scripts/haiku_syntax_check.sh`**: two things. (1) One line of mine — the
  include set was missing `headers/os/add-ons/*/`, so `EffectsWindow.cpp` (which
  includes `<Screen.h>` → `<Accelerant.h>`) always reported FAIL and "0 FAIL"
  was unreachable. (2) **The LV2 coverage**: on master the check compiled the
  `DAW_HAVE_LV2` branches OUT (the define comes from the `daw_lv2` target's
  PUBLIC flags), so `src/ui/`'s and `tests/`'s LV2-only code was never checked.
  The T3 agent fixed that on `origin/feature/lv2-state` (`33d6f05`); this branch
  **cherry-picks that commit** (script only) rather than writing a second copy
  of it. The check below therefore ran with `-DDAW_HAVE_LV2=1` active
  (pkg-config has lilv-0 0.28.0 and lv2 1.18.10 here), which is what the VM
  build does and what `tests/ui_functional_tests.cpp`'s LV2 half needs.

## Measurements (VM, beta6, 2 vCPU)

From `ui_functional_tests` / `TestBigProjectPlayback` (39 tracks / 320 clips —
the project the earlier tests have left by then; the test's own rows print 32
tracks / 320 clips):

| what | before | after (two runs) |
|---|---|---|
| play: the WINDOW thread is free again (Plan bound 300 ms) | 3.1 s, blocked | **20 ms**, 20 ms |
| play: the graph is in place (audio starts) | ~3.1 s | **2592 ms**, **2632 ms** |
| a structural edit during playback | stop, then 3.1 s of rebuild: audible gap + frozen window | graph swapped under a rolling transport; no stop, no device re-open |

The "before" number is the one recorded on 2026-10-09 in
`23-offscreen-and-icons.md`/`README.md` (M1.5), measured with the same test on
the same VM. The two "after" numbers are printed by the test itself, in two
separate runs (`298c636`, and the final pass at `08f08da`):

```
  big project: 39 tracks, 320 clips, window rolls in 20 ms, graph swapped in 2592 ms
  big project: 39 tracks, 320 clips, window rolls in 20 ms, graph swapped in 2632 ms
```

So M4.1 meets the plan's 300 ms bar for what it set out to move (the window
thread), and the audio start is still the disk work — one `TrackStream` per
clip, each opening its file and priming its ring, now on the worker thread.
M4.3's disk-stream pool is what brings that under the 300 ms budget; the test
keeps a generous upper bound on it (12 s) with the measured value in the
comment, because a bound near 2.6 s would be a flake under two agents sharing
this VM, not a regression guard.

## Verification

Every row below was run from this worktree, under the shared VM lock
(`flock -w 5400 /tmp/haiku-daw-vm.lock ...`). The *code* under test is the same
in `298c636` … `b56a585` (the commits after it are this record, a comment and
the cherry-picked script fix — none of them compile into the targets):

| suite | command | result |
|---|---|---|
| host build | `cmake -S . -B build-host && cmake --build build-host -j8` | exit 0 |
| host ctest | `ctest --test-dir build-host` | **53/53 passed** |
| ASan build | `cmake -B b-asan -DDAW_SANITIZE=ON && cmake --build b-asan -j8` | exit 0 |
| ASan ctest | `ctest --test-dir b-asan` | **53/53 passed** |
| new host target | `./build-host/engine_graph_tests` | 47 checks, 0 failures |
| cross-check | `sh scripts/haiku_syntax_check.sh` (+ the LV2 files) | **0 FAIL** |
| VM build | `sh scripts/vm.sh build` (sync + `cmake --build build -j2`) | exit 0 |
| VM ctest (`build`, LV2 on) | `ctest --test-dir build` | **55/55 passed** |
| VM `ui_functional_tests` | `DAW_UI_SHOTS=/tmp/shots ./ui_functional_tests` | **224 checks, 0 failures** |
| VM build-off | `cmake -B build-off -DDAW_LV2=OFF && ctest --test-dir build-off` | **51/51 passed** |
| screenshots | `DAW_UI_SHOTS=/tmp/shots3 ./ui_functional_tests`, then all 25 fetched and opened | ok — see below |

`ui_functional_tests` was 210 checks on master; the new `TestEngineGraphSwap`
adds 14.

**Shots reviewed** (this package draws nothing new, so the pass is "did the
window survive it"): `00-startup` (an empty Untitled project, correct),
`13-theme-150`, `16-docked-editor`, `19-engine-graph-swap` (playing, the two
`swap-N` tracks with their clips, the transport lit, the playhead sweeping),
`20-big-project-playing` (the 320-clip project rolling at 00:02.603 — i.e. the
transport ran straight through the swap), `21-save-as-panel`,
`24-opened-project`. Nothing overlapping, clipped or stale; nothing to fix.
(One earlier pass ran with a stale crash alert from another agent's killed test
process on the shared desktop; that run's shots are not the ones reviewed here.)

One VM caveat, stated because it cost time and will bite the next agent:
`~/haiku-daw/build` is **shared mutable state**. It holds whatever a sibling
agent's sync+build last produced, so a `ctest`/`./ui_functional_tests` run
against it without your own sync+build in the same lock hold silently tests
*someone else's* binary. One run here did exactly that (253 checks with a
different test's output) before it was caught; the sync, the build and the
tests must be one uninterrupted lock hold.

## Mutation checks

1. **Free a retired graph without waiting for quiescence** (drop the
   `while (!fQuiesced())` in `GraphSwap::ReclaimLoop`): `engine_graph_tests`
   fails 3 checks (`TestNotFreedUntilQuiesced`), and under ASan the stress loop
   reports `heap-use-after-free ... tests/engine_graph_tests.cpp:144`. Restored.
2. **`RebaseSkipFrames` returning 0** (the whole rebase disabled): 7 checks in
   `engine_graph_tests` fail. Restored.
3. **The pre-M4.1 rebuild shape** (`ReloadActiveEngine` → `fEngine.reset(new
   Engine())` + `StartPlayback()`, i.e. throw the engine away instead of
   swapping the graph): `ui_functional_tests` / `TestEngineGraphSwap` fails
   **3 checks** — the graph-swap counter never advances, `EnginePlayerStarts()
   == starts0`, and the transport is no longer playing once the edit has
   landed. The counter checks fail because the test then re-reads them from a
   *different* engine object, which is exactly the "the engine was replaced"
   fact it exists to catch. (One of its four checks, `EnginePlayersOpened() ==
   players0`, passes under this mutation by coincidence: each engine creates one
   player, so both counts read 1 — the other three are what catch it.)
   Restored. The mutation ran from a throwaway commit (`refs/heads/_vmwt`,
   deleted afterwards) applied by a script whose `trap` reverts the working tree
   on any exit, so a half-run cannot leave it behind.

## Not verified / for Marc

- **The audio start on the big project is still the graph build** (measured
  above): M4.1 moves it off the window thread and keeps the device open, but
  one `TrackStream` per clip is still opened and primed per rebuild. The plan's
  300 ms *audio* budget needs M4.3 (the disk-stream pool / lazy streams).
- **The rebase keeps a swap in sync by arithmetic** (host-tested); whether a
  swap during playback is *audibly* seamless is a listening check — nothing is
  audible on this VM, so it is on the hardware click list: play a busy project,
  add/remove an insert while it rolls, and listen for a seam.
- The record start still blocks the window thread on its build (by design, for
  take alignment) — the same freeze as before M4.1, on that one path.
- Not covered by a test: a request superseded while a build is in flight (the
  latest-wins coalescing). The path is small and review-checked; the UI reaches
  it by seeking twice inside one build.

