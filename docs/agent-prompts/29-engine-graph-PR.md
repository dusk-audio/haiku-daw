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
- **`scripts/haiku_syntax_check.sh`**: one line — the include set was missing
  `headers/os/add-ons/*/`, so `EffectsWindow.cpp` (which includes `<Screen.h>` →
  `<Accelerant.h>`) always reported FAIL and "0 FAIL" was unreachable.

## Measurements (VM, beta6, 2 vCPU)

<!-- filled in from the VM run -->

| what | before | after |
|---|---|---|
| 32-track/320-clip play, window thread | 3.1 s (blocked; M1.5 record) | |
| 32-track/320-clip play, graph in place (audio start) | same 3.1 s | |
| rebuild during playback (structural edit) | stop/rebuild, audible gap + frozen window | |

## Verification

<!-- counts + commands -->

## Mutation checks

<!-- break / fail / restore, by name -->

## Not verified / for Marc

<!-- -->
