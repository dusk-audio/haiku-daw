# Task: M4.3 — the disk-stream pool (a graph build no longer waits for its streams)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/disk-stream-pool` off `master`. This is the first slice of item M4.3 of
`docs/PLAN_1.0.md` ("Disk-stream pool"), the third item of T5 in
`24-continue-1.0-ENTRY.md` §5.

## What the measurement says (2026-10-12, VM, 51 tracks / 320 clips)

`DAW_BUILD_TIMING=1` breaks a graph build into its parts. The numbers below are
from a completed run of `TestBigProjectPlayback`:

    Engine: build timing: snapshot 0.1 ms
    Engine: build timing: 329 clip streams opened and primed in 2878.8 ms
                          (open+alloc 31.0 ms, thread+prime 2515.1 ms)
    Engine: build timing: everything else to here 3129.3 ms, 41 buses
      big project: graph swapped in 3295 ms

So the whole cost of a big build is **the per-stream prime wait**, and the wait
is not the disk: opening a clip's file, allocating its 2 s ring and seeking cost
**0.09 ms per clip** (31 ms for 329 of them), while `TrackStream::Prepare`'s poll
loop — `for (i < 50 && ReadAvailable() < kRingFloats / 2) sleep(2 ms)` — costs
**7.6 ms per clip**, and the clips are waited for **one after another**, so 329
of them serialize into seconds. Every stream has its own disk thread already; the
fills would run concurrently if the build did not insist on finishing one before
starting the next.

Plan M4.3's own words name this wait ("This replaces one thread per clip with a
wait of up to 100 ms each") and propose a pool of worker threads with only the
clips near the playhead open. The measurement says the *user-visible* half of
that — the plan's 300 ms audio-start budget — needs something much smaller:
**make the fills overlap, and stop asking each stream for a second of audio
before playback may begin.**

## Goal

A play (or a seek, or an in-place rebuild) starts its audio in well under the
plan's 300 ms on the 32-track / 320-clip project, with no change in what is
heard and no new thread, lock or allocation on the RT path.

## Design (this slice)

1. **Split `TrackStream::Prepare` into `Open()` and `WaitPrimed()`.**
   `Open()` keeps everything that touches the file and the thread — source,
   resampler, pass cursor, seek, ring, `std::thread` — and returns. It costs
   0.09 ms and is what a stream needs to *exist*. `WaitPrimed(wantFrames,
   timeoutUs)` is the poll loop alone, best-effort, and stops early when the
   stream has produced everything it ever will (`ProduceCursor::done`, M4.2: a
   clip whose window is behind the playhead, or one with no pass left in the
   loop, must not make the build wait out the timeout).
2. **Build in two phases** (`Engine::BuildGraph`): open every clip's stream and
   start every disk thread first (the shutdown check stays between clips), then
   make **one** bounded pass of `WaitPrimed` over the streams. The fills overlap,
   so the batch costs the slowest stream, not their sum.
3. **Pre-roll, not half a ring.** The target becomes a pre-roll measured in
   output frames (`kPrimePreRollFrames`, ~250 ms of audio) instead of half the 2 s
   ring, and the batch's own cap is a few hundred ms — inside the plan's budget,
   with headroom for a cold cache. A stream that misses the cap is not a stall:
   the disk thread keeps filling, and the RT's existing under-run debt resyncs
   whatever arrives late (the path an xrun already takes).
4. **Keep `DAW_BUILD_TIMING`** as a permanent, documented diagnostic (the same
   shape as M0.7's `DAW_TIMELINE_TIMING`): one line per build with the open and
   prime split, because this is the number the hardware box will be asked for.
5. **Measure the audio start, not just the build.** `TestBigProjectPlayback`
   writes 1 s of *silence* per clip, so today no test can tell "the graph landed"
   from "sound came out". Give its WAV a signal, and have the test time the play
   command to the first non-zero master peak — the plan's budget in the only form
   the VM can verify, with the bound left generous (a shared 2-vCPU VM is not a
   benchmark) and the measured number printed.

## The pool came IN, with the measurement that forced it

The slice above was measured first (3295 ms → 1113 ms with the opens batched and
the fills overlapped), and the numbers said the rest of the cost was the
per-clip thread model itself:

- **Thread starts:** 329 of them, MEASURED at 166 ms once the fills were already
  competing — and 14 ms when nothing competed.
- **Contention:** starting every fill at once (thrashing, not waiting) cost
  680 ms of a build in pure starvation of the builder thread.
- **Memory, and it is the same cost twice:** every open stream holds a ring, so
  a big project allocates 2 s × 329 = **253 MB** of ring and pays **359 ms** to
  allocate and zero it, whether or not the transport ever reaches 95% of those
  clips.
- **Idle:** 329 threads each polling their ring every 2 ms while it is full —
  ~165k wakeups/s on a 2-vCPU VM — and they keep doing it while the transport is
  *stopped*, because a graph is only retired, never paused.

So the pool is in this slice: a fixed set of `kStreamPoolWorkers` threads
(`StreamPool.h`, lock-free because assignment is static) serving every stream,
which is what the plan asked for and what the numbers said was needed. What it
replaced — one thread per clip, plus the waves that stood in for a pool while
this was being measured — is gone.

The plan's "only clips near the playhead hold an open stream" is *not* in this
slice, and here is the honest reason: with the pool, the build is inside the
budget, so the remaining benefit of opening lazily is memory (a project's rings
are allocated up front) and it costs an RT-safe open/close path (a slot the
callback reads while a worker swaps the stream behind it). The build's own
ordering (StreamOrder.h) already fills the clips the transport needs first, so
nothing is late. Marc's call whether 1.0 wants the memory back.

## Definition of done

- The build-timing line on the VM shows the big-project build inside the plan's
  300 ms (measured before/after, both printed in the record).
- The audio actually starts: the new check in `TestBigProjectPlayback` passes
  with the measured number recorded.
- Host build exit 0, host ctest green, ASan green, `haiku_syntax_check.sh` 0 new
  FAIL, VM `build` and `build-off` ctest green.
- A mutation check per new behaviour, named in the commit body.
- No window's drawing changes; if the implementation ends up changing one,
  `docs/UI_GUIDELINES.md` §1 applies.
