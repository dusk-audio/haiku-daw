# Task: M4.2 — a seamless loop (the transport wraps inside the engine)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/seamless-loop` off `master`. This is item M4.2 of `docs/PLAN_1.0.md`
(the task list in `24-continue-1.0-ENTRY.md` §5, T5, second item), and it follows
M4.1 (`29-engine-graph.md`), whose graph-swap machinery this builds on.

## Codebase orientation (read before coding)

- The live engine is `src/engine/Engine.{h,cpp}` (Haiku-only: links the Media
  Kit). Since M4.1 everything a rebuild replaces — streams, buses, buffers, FX,
  the mix order, the meter scratch — lives in one `Engine::Graph`, built on a
  builder thread from a **snapshot** of the project and published to the RT
  callback with one atomic pointer exchange (`GraphSwap.h`). The callback loads
  that pointer once per block (`Engine::FillBuffer`) and renders the whole block
  from it.
- **One `TrackStream` per audio clip** (`Engine.cpp:114` `Prepare`, `:154`
  `DiskLoop`, `:204` `Mix`). `Prepare` opens the clip's file through
  `OpenAudioSource` (WAV / AIFF / FLAC / Ogg), seeks it to
  `sourceOffset + seekDelta`, and starts a disk thread that decodes, resamples
  to the output rate and pushes interleaved stereo frames into a lock-free
  `RingBuffer` (2 s per stream, `kRingFloats`). `Mix` — RT thread — consumes one
  ring frame for every timeline frame the playhead spends **inside the clip's
  window** `[startFrame, startFrame+lengthFrames)`, sums it with the clip's gain,
  pan, per-clip gain and fade envelope, and never blocks (on underrun it
  contributes silence and carries a `fSkipDebt`).
- The ring is therefore **a FIFO of the clip's audio in playback order**: ring
  frame k belongs to the k-th timeline frame in the window, the origin being
  `max(clipStart, startFrame + seekDelta)` (`StreamRebase.h` says the same in
  one paragraph, and `RebaseTo` is the only other reader of that rule).
- **The loop today rebuilds the engine.** `MainWindow::MessageReceived`'s 60 Hz
  `MSG_PULSE` (`MainWindow.cpp:2063`) sees `ph >= transport.loopEnd`, rewinds the
  playhead to `loopStart` and calls `TransportController::StartPlayback()`
  (`TransportController.cpp:45`), which issues
  `Engine::RequestLoad(..., LoadMode::NewPosition)`. A position-changing request
  **detaches the active graph immediately** (`Engine::DetachGraph`,
  `Engine.cpp:995`): the RT renders silence and the playhead stands still until
  the builder thread has produced a whole new graph — every clip file re-opened,
  every disk thread restarted, every ring primed, the FX rebuilt. Every loop
  pass, forever, with a hole the length of a build (the measured 3.1 s on the
  32-track / 320-clip project is exactly this work).
- `LoadMode::InPlace` is the opposite: the old graph keeps playing while the new
  one is built, and the new one joins at the playhead the transport has reached
  by then, through the per-stream rebase (`StreamRebase.h`, `TrackStream::RebaseTo`).
  M4.1's record says the loop wrap is the case M4.1 did *not* fix.
- The pulse is the window's only view of the engine: it reads
  `Engine::Playhead()`, `IsFinished()`, the meters and the peaks, and it is where
  the record seam is handled (`MainWindow.cpp:2030`).
- Kit-free code is host-tested (`tests/engine_graph_tests.cpp` covers
  `RebaseSkipFrames` and `RingBuffer::Skip`); `src/engine/Engine.cpp` does not
  compile on the Linux host — check every edit with
  `sh scripts/haiku_syntax_check.sh` and build it on the VM.
- **RT rules (`24-continue-1.0-ENTRY.md` §7)**: `FillBuffer` and everything it
  calls may not allocate, lock, log or do file I/O — arithmetic on preallocated
  memory, lock-free rings and atomics only.

## Goal

A loop plays as a loop: at the loop end the transport jumps to the loop start
**sample-exactly, with no hole, no rebuild and no device re-create**, and it does
so on every pass for as long as it runs. No `BSoundPlayer` re-create, no second
graph built, no clip file re-opened.

## Design

The engine owns the loop and wraps **inside the callback**, and the disk threads
pre-roll the loop start so their rings always hold the audio in playback order.

1. **The loop range is part of the graph**, like the tempo map, the routing and
   the mix order: the builder copies it out of the snapshot it was already given
   (`project.transport.loopEnabled/loopStart/loopEnd`). It is therefore fixed for
   the life of a graph, which is what makes the two threads below agree by
   construction.
2. **The RT wraps by splitting the block.** At the loop end the transport's
   position is discontinuous, so `Engine::Graph::Render` renders the callback in
   one or more *segments*: `[pos, loopEnd)` at the old position, then
   `[loopStart, ...)` for the rest of the block. The split is sample-exact (the
   first segment is cut at exactly `loopEnd`), and each segment is the existing
   render body with its own `blockStart` — which is what makes fades, automation,
   MIDI notes, the metronome and the transport itself land on the loop's own
   timeline. FX chains, delay lines, live voices and the meters are *not* reset:
   the loop is a jump, not a new graph, so a reverb tail carries across the seam.
3. **The disk thread produces in playback order, across the seam.** A stream's
   pass is the part of its window the transport will visit in one loop pass:
   `[max(clipStart, passLo), min(clipEnd, passHi))` where the first pass starts
   at the clip's own seek position and ends at `loopEnd`, and every later pass is
   `[loopStart, loopEnd)`. The producer seeks its source to the frame the pass
   starts at, resamples, and writes **at most that pass's frames**; at the pass
   end it seeks to the next pass's start and continues. The ring is a FIFO, so
   the consumer walks the loop without ever knowing a seam happened: the frames
   it reads next are, by construction, the frames for the frames it asks for.
   Pre-rolling is automatic — the disk thread runs ahead into the ring, so by the
   time the callback reaches the wrap the loop-start audio is already there.
4. **Nothing is rebuilt, and the UI stops wrapping.** The pulse's playback wrap
   goes; the engine reports the wrapped playhead like any other position.
5. **A loop-range edit while playing still reaches the engine.** The range lives
   in the graph, so an edit is a structural change: `ReloadActiveEngine()`
   (in-place) carries the new range, and the M4.1 rebase — generalised below to
   walk a *looped* path — joins the new graph to the running transport.

The arithmetic of 2 and 3 is the part a bug would live in, so it is kit-free
(`src/engine/StreamLoop.h`) and host-tested: the pass walker, the block splitter,
and the loop-aware rebase. The Haiku-only code is the wiring around them.

## Work items

1. **`src/engine/StreamLoop.h`** (new, kit-free, header-only, host-tested):
   - `WindowSpanFrames(clipStart, clipEnd, from, to)` — the frames a stream
     consumes walking `[from, to)` inside its window.
   - `ProduceCursor` — the producer's pass walker: `Start(origin)` sets the first
     pass, `Next()` advances to the loop pass (or reports done), `lo`/`hi` are the
     current pass, `SourceFrame(sourceOffset, t)` maps a timeline frame in the
     window to the source frame to seek to.
   - `LoopRebaseSkipFrames(origin, clipStart, clipEnd, target, looping, loopStart,
     loopEnd, wraps)` — `RebaseSkipFrames` generalised to the looped path: pass-1
     tail + complete passes + the current pass's head, where the number of
     complete passes is what the engine's wrap counter says happened since the
     graph was requested (`StreamRebase.h`'s function stays as the non-looping
     case, and the two must agree on it).
   - `SegmentFrames(pos, frames, looping, loopStart, loopEnd)` — how much of a
     callback block the RT renders before the wrap, and how many segments the
     block can take (a loop shorter than a block must not spin).
2. **`src/engine/Resampler.h`**: a `Reset()` (the phase and the carried input
   frame are per-pass state; a pass starts at a fresh seek).
3. **`src/engine/Engine.{h,cpp}`**:
   - `TrackStream` takes the loop range (one `StreamLoop` struct: `on`, `lo`,
     `hi`) and keeps it; `DiskLoop` becomes pass-driven through `ProduceCursor`;
     `RebaseTo(frame, wraps)` uses the loop-aware skip.
   - `Engine::Graph` gains `fLooping`, `fLoopStart`, `fLoopEnd` and the engine
     wrap count the request was issued at; `Render` becomes the segment driver
     over the existing body (`RenderSegment`), which no longer stores the
     playhead itself.
   - `Engine` gains an RT-incremented wrap counter (`fWraps`, `Wraps()`), stamped
     into every request; the graph's `finished` check is skipped while looping.
   - A record start builds its graph **unlooped** so the loop-record seam path
     (`MainWindow.cpp:2030`) is unchanged.
4. **UI**: the pulse's playback-wrap branch goes; `TimelineView`'s loop edits
   (`ToggleLoop`, `LoopBetweenMarkers`, the ruler drag's commit) ask the window
   to reload the engine when the transport is rolling; `MainWindow::EngineWraps()`
   as a test hook.
5. **Tests**: host (`tests/stream_loop_tests.cpp`: the pass walker's sequences,
   the split arithmetic, the rebase across wraps, and the cross-check that the
   passes' frames sum to the skip), VM functional (`TestSeamlessLoop` in
   `tests/ui_functional_tests.cpp`: loop a real project on a real engine and
   assert — across at least two wraps — that the wrap counter advanced, that no
   graph was published, no player was re-created and no player was restarted,
   and that the playhead stays inside the loop).

## Out of scope (say so in the record)

- **Seek while playing** still goes through `StartPlayback` (a position-changing
  rebuild). The in-engine seek needs the producer half of this work *plus* a
  re-prime handshake (the RT must not read stale ring content after a jump the
  producer cannot infer); M4.2's title and its audible defect are the loop, and
  the seek stays as it is.
- **Loop recording** keeps its per-pass engine restart: the record graph is built
  unlooped, so the existing `ph >= loopEnd` seam check still fires exactly as it
  does today. Making the record seam seamless needs the take layout's own pass.
- The `TrackStream`'s ring stays 2 s per stream; this task adds no per-stream
  memory.

## Definition of done

- `cmake --build build-host` exits 0, `ctest --test-dir build-host` is green
  (count in the record), ASan green, `sh scripts/haiku_syntax_check.sh` 0 FAIL.
- VM `build` and `build-off` ctest green, `ui_functional_tests` green with the
  new checks.
- Every new behaviour has a test, mutation-checked (name the mutation in the
  commit body).
- The record (`36-seamless-loop-PR.md`) states the pass counts, the mutations and
  what is **not** verified (audibility on the VM: nothing is audible there).
- No window's drawing or layout changes; if the implementation ends up changing
  one, §1 of `docs/UI_GUIDELINES.md` applies (shots, both modes).
