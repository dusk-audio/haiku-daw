# Record — M4.3 the disk-stream pool

Branch `feature/disk-stream-pool` off `master` (`3667e7d`). Spec:
`docs/agent-prompts/37-stream-start.md`. Plan: `docs/PLAN_1.0.md` §M4.3
(`24-continue-1.0-ENTRY.md` §5, T5's third item).

## What the item was for

The plan's M4.3: "A fixed set of worker threads, and only clips near the playhead
hold an open stream. This replaces one thread per clip with a wait of up to
100 ms each." Its budget: the plan's verification says a 32-track / 300-clip
project plays in under 300 ms. M4.1's own record handed this item the number it
had left behind — **3295 ms** on the VM's 51-track / 320-clip project — and the
phrase "one `TrackStream` per clip is still opened and primed per rebuild".

## The measurements that shaped it

Every number below is from a completed run on the VM (2 vCPU, QEMU disk), with
`DAW_BUILD_TIMING=1`, and each one is what decided the next step:

| Step | Build | What it said |
|---|---|---|
| as it was | **3295 ms** | snapshot 0.1 ms, **streams 3128 ms**, everything else 0.6 ms — the build *was* the streams |
| split open from prime | — | open + ring alloc **31 ms** (0.09 ms/clip), thread start + prime wait **2515 ms** — the wait is per clip and they *serialize* |
| open all, then wait | 1113 ms | opens 750 ms: 329 fillers starting at once **starve the builder** (680 ms of contention); thread starts 14 ms when nothing competes |
| fills in waves of 4, nearest the playhead first | 883 ms | thread starts 166 ms once competitors exist — a floor the per-clip thread cannot go below |
| **the pool** (4 workers, static round-robin) | 721 ms | left: **359 ms of ring allocation** (2 s × 329 = 253 MB, allocated and zeroed up front) |
| ring sized to what the pool needs (0.5 s) | 446 ms | left: the priming wait, always exactly the 200 ms cap |
| one poll per round, waiting only for the clips due inside the pre-roll | 301 ms | left: streams whose **source had ended** made the build wait out its budget |
| EOF is "nothing to do", not "try again" | 461 ms → then 301 ms | the pool had been **hammering a finished stream 683 922 times in 200 ms** — a busy spin, found by the diagnostic |
| final | **151 ms** | 329 streams opened in 95.6 ms, primed in 52.1 ms |

The last line of the final run: `big project: engine build 151622 us,
command-to-graph 304 ms`, `master peak 1.061, sound 304 ms after the play
command (0 ms after the graph landed)`.

**Sound at 0 ms after the graph lands** is the pre-roll doing its job: by the time
the graph is published, the clips the transport needs are already in their rings,
so the first callback renders audio, not silence.

## What landed

| Piece | Where |
|---|---|
| the fixed set of fill threads (lock-free: static assignment) | `src/engine/StreamPool.h` (new, kit-free, host-tested) |
| which clip the transport needs next (loop-aware), for the fill order | `src/engine/StreamOrder.h` (new, kit-free, host-tested) |
| `Open()` (file, ring, cursor, seek) split from filling | `src/engine/Engine.{h,cpp}` (`TrackStream`) |
| `FillOnce`/`FillStep`: bounded progress, EOF is "nothing more" | `src/engine/Engine.{h,cpp}` |
| the ring sized to the pool's visit interval, not 2 s | `src/engine/Engine.cpp` (`kRingFramesPerStream`) |
| the build: open all, order by due, one pool, one batched wait | `src/engine/Engine.cpp` (`BuildGraph`) |
| the engine's own build duration, for tests and the hardware box | `Engine::LastBuildUs`, `MainWindow::EngineBuildUs` |
| `DAW_BUILD_TIMING=1` kept as a permanent diagnostic | `src/engine/Engine.cpp` (the M0.7 `DAW_TIMELINE_TIMING` precedent) |
| the audio-out guard | `MainWindow::MasterPeakL`, `TestBigProjectPlayback` |
| host suites | `tests/stream_pool_tests.cpp` (new, 75 checks) |
| the functional flow | `tests/ui_functional_tests.cpp` (`TestBigProjectPlayback` grown) |

**What is deliberately NOT here:** "only clips near the playhead hold an open
stream". With the pool the build is inside the budget, so what lazy opening buys
now is memory (a project's rings are allocated up front) — and it costs an
RT-safe open/close path where the callback reads a slot while a worker swaps the
stream behind it. The fill ORDER already puts the clips the transport needs first,
so nothing is late. Marc's call whether 1.0 wants that memory back; the spec says
so in its own section.

## Verification

- Host `build-host`: build exit 0, ctest **68/68** (the new `stream_pool_tests`
  is the one addition). `b-asan`: build exit 0, ctest **68/68** (the pool's
  threads run under ASan; a stream filled by two workers or a worker that never
  exits is what its tests look for).
- `sh scripts/haiku_syntax_check.sh`: **73 OK, 4 FAIL** — the four
  FLAC/Vorbis files, which fail identically on pristine `master` on this host
  (no FLAC/Vorbis headers for the cross compiler; verified by stashing).
- VM `build` (LV2 on): ctest **70/70**. `build-off`: ctest **51/51 in the suite
  runs** — one ctest run failed a theme-colour check
  (`ui_functional_tests.cpp:5415`, the dock strip's title colour, in
  `TestDockPages`, untouched by this branch); two further runs of the same
  binary were clean (801 checks, 0 failures).
- VM `ui_functional_tests`: **817 checks, 0 failures** (the two new checks are
  the engine-build bound and the audio-out guard).
- The M4.2 flow still measures the same: **3 wraps in 3008 ms (1003 ms per
  pass)**, no graph, player or player-start movement.
- Mutation checks, each run against the new host tests (break, watch it fail,
  restore):
  - `StreamPool`: every stream assigned to the first worker → **2 failures**
    (the round-robin/worker-count check).
  - `StreamPool`: a stream that makes no progress counted as work → **1 failure**
    (the busy-spin check — the bug the VM found first).
  - `StreamOrder`: a clip ending exactly at the playhead scored as playing →
    **1 failure**.
  - `StreamOrder`: a clip past the loop end scored as reachable → **3 failures**.
- No window's drawing or layout changed, so no screenshot pass was run; the UI
  edits are two test-only accessors and nothing else.

## Left for Marc / later packages

- **Ears, on 192.168.1.186:** play the big project and listen for dropouts. The
  VM has no audio, so what is verified here is that the build is fast, that every
  stream is filled by one worker at a time, and that the master meter reads a
  level — not that the 0.5 s ring never underruns on real hardware. The ring's
  size is the thing to watch: it is the slack between the fill workers and the
  callback.
- **Idle cost is now 4 polls, not 329** (the pool's workers sleep when everything
  they own is full), but a graph that is loaded and STOPPED still holds its rings
  and its workers. Retiring or pausing a graph on Stop is not this item.
- **Lazy streams** (memory) as above.
- The `docs/agent-prompts/README.md` status table is now **14 merges** behind
  (rows stop at M1.6, and PRs #1–#13 plus M4.2 have no row). M8.2 owns bringing
  it up to date; M4.2's row is added with this branch rather than left implied.

## On merge

One row for the table (the M4.2 row is added in this branch's docs commit):

> M4.3 disk-stream pool | **Merged** — a fixed set of fill threads
> (`StreamPool.h`, static assignment, lock-free) replaces one thread per clip,
> the ring is sized to the pool's visit interval instead of 2 s, and the build
> opens every stream, orders them by what the transport needs next, and makes one
> bounded pre-roll wait. The VM's 51-track / 320-clip build went **3295 ms →
> 151 ms** with sound **0 ms after the graph lands**. Host 68/68, VM `build`
> 70/70 + `build-off` clean, `ui_functional_tests` 817 checks. Record
> `37-stream-start-PR.md`
