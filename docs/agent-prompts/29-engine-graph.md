# Task: M4.1 — the engine graph off the window thread

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/engine-graph` off `master`. This is item M4.1 of `docs/PLAN_1.0.md`
(the task list in `24-continue-1.0-ENTRY.md` §5, T5), and it is the fix for the
measured **3.1 s play start** on the 32-track / 320-clip project (budget 300 ms;
`docs/agent-prompts/23-offscreen-and-icons.md`, `README.md` M1.5 row).

## Codebase orientation (read before coding)

- The live engine is `src/engine/Engine.{h,cpp}` (Haiku-only: links the Media
  Kit). `Engine::Load` (`Engine.cpp:211`) does everything on the calling
  (window) thread: it opens a **new `BSoundPlayer`** on every call, builds one
  `TrackStream` per audio clip (each opens its WAV, spawns a disk thread and
  waits up to 50 × 2 ms for the ring to prime — `TrackStream::Prepare`,
  `Engine.cpp:87`), groups streams into `Bus` nodes, builds the FX chains, the
  topo order (`ResolveOrderWithEdges`, `Engine.cpp:453`), the PDC delay lines
  (`ComputePdc`, `:486`), the node buffers, the metronome, the tempo map and the
  loudness meter. On 320 clips that is 3.1 s of blocked window thread, with the
  audio device closed and re-opened at both ends of it.
- `Engine::FillBuffer` (`Engine.cpp:856`) is the RT callback body: it walks
  `fOrder`, mixes each node's streams, runs the insert chains
  (`RunInsertSlot`, shared with the Exporter), routes by PDC delay, sums to the
  master, meters, and advances `fPlayhead`. **RT rules (`24-continue-1.0-ENTRY.md`
  §7, `Engine.h:8`): no allocation, locks, file I/O, syscalls or logging in
  `FillBuffer` and everything it calls — arithmetic on preallocated memory,
  lock-free rings and atomics only.**
- `TransportController` (`src/ui/TransportController.{h,cpp}`) owns the engine
  (`fEngine`) and is the only place that builds one: `StartPlayback` (play, seek,
  loop wrap), `ReloadActiveEngine` (a structural edit during playback),
  `StartRecordEngine` (overdub/record). `RecordController::UpdateMidiMonitor`
  and `StopMidiMonitor` (`RecordController.cpp:389`) build a monitor-only engine
  of its own. The window's 60 Hz `MSG_PULSE` (`MainWindow.cpp:1691`) polls the
  engine: `UpdateMix`, playhead, meters, loop wrap, record seam, `IsFinished`.
- The isolation that a *swap* needs is already the codebase's idiom:
  `RingBuffer` is a lock-free SPSC ring, live mix params are per-node atomics,
  `QuiesceMonitorInput` (`Engine.cpp:730`) proves "no RT callback can still be
  holding this pointer" by waiting out two `fCallbackGen` boundaries, and
  `fPlayerRunning` (`Engine.h:593`) is set before `BSoundPlayer::Start()` and
  cleared only after `Stop()` returns.
- Snapshotting the model for an off-thread worker is an existing pattern:
  `RenderJobs` takes `std::make_unique<Project>(*fWin->fProject)` before its
  export thread starts (`RenderJobs.cpp:32`). `Project` is plain data.
- Kit-free code is host-tested; `src/engine/Engine.cpp` is not compilable on the
  Linux host — check it with `sh scripts/haiku_syntax_check.sh` after every
  edit and on the VM before every commit.
- The functional tests drive the real windows by posting the messages the
  widgets post (`tests/ui_functional_tests.cpp`); the plan asks for "an engine
  swap during playback with no `BSoundPlayer` re-create (an engine counter is
  exposed for tests)".

## Goal

Play, seek, loop wrap, a structural edit, a record start and a project load must
all rebuild the audio graph **without blocking the window thread and without
re-opening the output device**. The graph is built on a worker thread, published
by one atomic pointer exchange that the RT callback performs once per block, and
the graph it replaces is freed on a reclaim thread — never on the RT thread and
never while the RT side can still see it. An edit made during playback is
audible with **no gap**: the old graph keeps playing until the new one is in.

## Work items

1. **Kit-free swap machinery + host tests.** New header
   `src/engine/GraphSwap.h`: an `atomic<Graph*>` slot with `Publish` (exchange,
   queue the old graph for reclaim), `Detach` and `Active()`, plus a reclaim
   thread that frees queued graphs once the owner's `quiesced()` predicate says
   no RT thread can hold them. Kit-free (STL + `<thread>`), used by `Engine` for
   the real graph and host-tested with a fake graph type: publish/detach
   semantics, exactly-once destruction, "not freed while !quiesced", "freed once
   quiesced", drain on destruction, and a publisher/consumer stress loop under
   ASan (a premature free is a use-after-free and ASan catches it).
   New header `src/engine/StreamRebase.h`: the frame math that re-aligns a
   stream's ring when a graph is swapped in mid-playback — `RebaseSkipFrames(
   origin, clipStart, clipEnd, target)` — host-tested at its edges (target
   before/inside/past the clip, origin inside/past). One new host test target
   `engine_graph_tests` for both.
2. **`RingBuffer::Skip` + `TrackStream` rebase.** `RingBuffer::Skip(n)` is a
   consumer-side O(1) bulk advance of the read cursor (the RT thread is the only
   reader). `TrackStream` records its ring origin (`max(buildStart, clipStart)`)
   and gets `RebaseTo(Frame timelineFrame)`: skip the frames this stream would
   have consumed between its origin and `timelineFrame`, in bulk where the ring
   has them, and carry the remainder in the existing `fSkipDebt` resync. It runs
   on the RT thread inside `Mix`, only on the first block after a swap — same
   arithmetic on preallocated memory, no allocation, no locks.
3. **Split the engine into engine + graph.** The per-load state moves into a
   nested `Engine::Graph` (streams, `Bus`es, node buffers, order, per-node
   peaks, master FX chain + insert slots, scratch/monitor buffers, live-voice
   pool and note scratch, metronome, tempo map, loudness, `endFrame`,
   `buildStart`). The `Engine` keeps what must survive a swap: the
   `BSoundPlayer`, the transport atomics (playhead, playing, finished, peaks,
   master gain), the monitor/monitor-only atomics, the MIDI route list, the
   insert-meter and watched-insert publishing storage (the UI keeps pointing at
   inserts by index across rebuilds), the callback generation and the graph
   slot. `FillBuffer` becomes: load the pointer once per block (acquire); no
   graph → silence, no advance; else render the graph. **Everything the RT path
   calls takes the graph it was loaded from — no second load, no engine-owned
   container is touched.**
4. **The worker build.** `Engine::RequestLoad(project, start, minEnd, mode)`
   copies the project on the caller's thread (the `RenderJobs` snapshot idiom),
   stores the request (latest wins — a newer request replaces a queued one) and
   wakes the builder. The builder runs the old `Load` body off-thread — file
   opens, disk threads, priming, FX, topo order, PDC, buffers — into a fresh
   `Graph`, then publishes it (`GraphSwap::Publish`), records `LoadsCompleted`/
   `LastLoadStatus`, and hands the replaced graph to the reclaimer. It checks a
   shutdown flag between clips so quitting never waits out a full build. A graph
   whose request was superseded while it built is discarded on the worker, not
   published.
   `mode` is the *transport* decision:
   - `LoadMode::NewPosition` (play, seek, loop wrap, record start, monitor
     start): the active graph is **detached at request time** — the RT goes
     silent and the playhead stops until the new graph is in, which is today's
     seek/loop-wrap behaviour without the freeze — and the new graph starts at
     the position `Start()` set.
   - `LoadMode::InPlace` (a structural edit while playing, `SyncFxToEngine`'s
     rebuild): the active graph **keeps playing and keeps advancing**; the new
     graph joins at the current playhead through the rebase (item 2), so the
     edit is heard with no gap and no drift.
   `status_t Engine::Load(project, start, minEnd)` stays, as the synchronous
   wrapper (EnsurePlayer → RequestLoad(NewPosition) → wait → status) for the
   windowless callers: `prototypes/play_clip`, `mix_project`,
   `tests/lv2_live_editor_tests.cpp`, and `RecordController::UpdateMidiMonitor`.
5. **Keep the output device open.** `BSoundPlayer` becomes persistent on the
   Engine: `EnsurePlayer(project)` creates it only when there is none, or when
   the requested sample rate or `SetBufferFrames` value differs from the live
   one, and reports the device error synchronously (that is the only failure the
   UI must show; a build failure is polled). Player `Start`/`Stop` still bracket
   playback, and no rebuild path calls either.
6. **Reclaim.** The old graph is freed on the reclaim thread once `fPlayerRunning`
   says no callback is in flight, or two `fCallbackGen` boundaries have passed
   since it was queued (the `QuiesceMonitorInput` proof). The destructor order
   is: signal + join the builder, `Stop()` the player, stop and drain the
   reclaimer, free the active graph.
7. **Counters for the tests.** Public on `Engine`: `GraphsPublished()`,
   `LoadsCompleted()`, `LastLoadStatus()`, `PlayersOpened()`, `PlayerStarts()`,
   `HasGraph()`, `GraphPending()`. MainWindow exposes a small public accessor
   block for the functional tests (next to `IsPlaying()`).
8. **Wire every rebuild path.** `TransportController::StartPlayback` →
   `EnsurePlayer` + `RequestLoad(NewPosition)` + `Start` (no `fEngine.reset`, no
   synchronous `Load`); `ReloadActiveEngine` (playing) → `RequestLoad(InPlace)`;
   `StartRecordEngine` → `EnsurePlayer` + `RequestLoad(NewPosition)` + **wait** +
   `Start` — the one path that still waits, because the take's alignment
   (`RecordPlan`) is measured from the moment the engine rolls, so the capture
   must not begin before the graph is in (it blocked on the same build before
   M4.1, and still does, for the same reason); the pulse polls
   `LoadsCompleted`/`LastLoadStatus` and stops the transport with today's
   `ReportError` wording when a build failed (`B_ENTRY_NOT_FOUND` stays silent).
   `UpdateMix`, `SyncFx`, `SetFxParamLive`, `SetFxTempo`, `PublishFxWatchNow`,
   `TrackPeakL/R` all address the *active* graph — through a `GraphPin`, so the
   reclaimer cannot free a graph an off-RT caller is inside (the callback
   generation alone is a proof about the RT thread only).
9. **Tests on the VM.** `ui_functional_tests` gains `TestEngineGraphSwap`:
   play, force a structural rebuild, assert `GraphsPublished` moved,
   `PlayersOpened` and `PlayerStarts` did not, the transport never stopped, and
   the playhead kept advancing across the swap. `TestBigProjectPlayback` is
   tightened to what M4.1 actually fixes — the window's response — with the
   graph-ready time measured and printed separately (the honest numbers go in
   the record; the remaining audio-start cost is M4.3's).
10. **Record + measure.** `29-engine-graph-PR.md` with: the measured before/after
    numbers and the command that produced them, the mutation checks by name,
    and what M4.1 does *not* fix (the audio start still waits for the disk
    streams — M4.3).

## Definition of done

- Host: `cmake --build build-host` exit 0, `ctest --test-dir build-host` all
  green including the new `engine_graph_tests`; ASan (`b-asan`) green;
  `sh scripts/haiku_syntax_check.sh` 0 FAIL.
- VM: `build` and `build-off` ctest green, `ui_functional_tests` green
  including `TestEngineGraphSwap` and the tightened `TestBigProjectPlayback`
  bound, with the measured numbers in the record.
- Every new behaviour has a test that was mutation-checked (break it, watch it
  fail, restore) and the mutations are named in the commit body.
- No `Co-Authored-By:` or any AI-attribution trailer, in commits or the PR.
- The record states plainly what was and was not measured.
