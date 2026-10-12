# Record — M4.4 first slice: waveform peaks as a background job with a disk cache

Branch `feature/peak-cache` off `master` (`34ecbe6`). Spec:
`docs/agent-prompts/38-peak-cache.md`. Plan: `docs/PLAN_1.0.md` §M4.4
(`24-continue-1.0-ENTRY.md` §5, T5's fourth item).

## The defect

`MainWindow::RebuildPeaks` opened and scanned **every clip's file on the window
thread**, on every load, keeping nothing: a project reopened twice read all of its
media twice, with the window locked for the whole of it (its own message handler,
so nothing else could run). The plan's fix, in its own words: "Background peaks
with an on-disk peak cache keyed by path, modification time and size."

## What landed

| Piece | Where |
|---|---|
| `PeakCacheId` — what a cached envelope is valid for | `src/model/PeakCache.h` |
| `SerializePeakCache` / `DeserializePeakCache` — a versioned byte format | `src/model/PeakCache.{h,cpp}` |
| `PeakCacheFileName` — a path hash, so a path cannot escape the cache dir | `src/model/PeakCache.cpp` |
| `PeakCache::Cancel` — a scan that gives up promptly | `src/model/PeakCache.{h,cpp}` |
| the loader worker (stat → cache hit, or build + write) | `src/ui/PeakLoader.{h,cpp}` (new) |
| `<settings>/HaikuDAW/peaks/` | `ProjectDocument::PeakCacheDir` |
| the window: start the loader, drain in the pulse, keep the pulse alive | `src/ui/MainWindow.{h,cpp}` |
| host tests | `tests/peak_tests.cpp` (60 checks, was 40) |
| the functional flow | `tests/ui_functional_tests.cpp` (`TestPeakCache`) |

The worker never touches the window's map: finished envelopes go into its own
list and the window drains them under its own lock, which is the lock the
timeline already draws under — so no drawing code learned about the thread. Two
details that only showed up while wiring it:

- **The pass must be cancellable per chunk**, because `Start` joins the previous
  pass: without that, a reload would block the window on a whole file (the
  `PeakCache::Cancel` hook, asked once per decoded chunk).
- **The pulse must outlive the load**: the envelopes are drained by the 60 Hz
  pulse, so `UpdatePulse` now keeps it running while `PeakLoader::HasWork()` —
  and that is work *or* a full drain list, because a worker that finished between
  two pulses still has envelopes to hand over.

A cancelled pass keeps **nothing** (`Build` returns false and leaves the cache
invalid) and abandons the files it had not reached, cache writes included — which
is what the functional test had to learn: it waits for the pass to *finish*, not
for the first envelope to appear.

## Verification

- Host `build-host`: build exit 0, ctest **68/68** (no new target: the cache
  bytes live in the existing `peak_tests`, now 60 checks). `b-asan`: build exit
  0, ctest **68/68**.
- `sh scripts/haiku_syntax_check.sh`: **75 OK, 4 FAIL** — the four
  FLAC/Vorbis files that fail identically on pristine `master` on this host
  (no FLAC/Vorbis headers for the cross compiler).
- VM `build`: ctest **69/70**; `build-off`: **63/64** — the single failure in
  each is the FX-drag check below. Every other suite in both directories passed.
- VM `ui_functional_tests`: **827 checks** (build) / **811** (`build-off`, which
  compiles the LV2 checks out), 1 failure each — the FX-drag check. The new flow
  reports (this run had a warm cache from an earlier run; a cold run after
  removing the directory reported `7 envelopes, 0 from cache, 7 built`, and the
  pass after it, `7 hits, 0 misses`):

      peak cache ...
        peaks: 6 envelopes, 5 from cache, 2 built
        peaks: second pass 7 hits, 0 misses, this file -> 1 (1 = cache)

  The **second pass over the same media is seven cache hits and no builds** —
  which is the only way the VM can show the on-disk cache is doing anything, and
  it is asserted about the file this test writes (a project's other clips may
  point at media that is not on the machine). The files on disk match exactly:
  the 20 s test clip's cache is 30044 bytes = 3750 buckets × 8 + a 44-byte
  header, which is what `ls` shows in `~/config/settings/HaikuDAW/peaks/`.
- **One check fails on this VM, and it is not this branch's.** `TestFxRetargetFlush`
  (`heard != LimiterDesc().p(0)`, `ui_functional_tests.cpp:6080`) failed in every
  run of this session. Attributed the same way M4.3's record did it: master's own
  tests were built on this VM (`origin/master`, `34ecbe6`) and run — **775 checks,
  the same single failure** (`ui_functional_tests.cpp:5635` in master's copy). It
  is a synthetic mouse drag to the FX window, retried four times, in code this
  branch does not touch; it passed in some earlier runs and passes on other VM
  states, so it is environmental (the drag is synthesised at window coordinates).
  Marc should know it is red again.
- Mutation checks (break, watch it fail, restore):
  - the id ignoring the file's size → **1 failure**.
  - the id ignoring the bucket size → **1 failure**.
  - a cancelled scan keeping what it read → **4 failures**.
  - the cache name hashing only a path prefix → **1 failure**.
  - removing the bounds checking in the reader (the bucket-count guard and the
    per-bucket reads together) → the suite **dies**; each alone is masked by the
    other, which is the point of having both. The count guard's own removal is
    not observable on this host (Linux overcommits the 32 GB reservation it
    prevents) — it is there so a corrupt file cannot ask for it.
- No screenshot pass: no window's drawing or layout changed. The envelope data
  the timeline draws is the same shape it always was, produced earlier and off
  the window thread.

## Left for Marc / later packages

- **The item's other half is not here:** Freeze, Reverse, Normalize and Strip
  Silence still decode their clip **on the window thread**
  (`RenderJobs::RegionNormalize` and friends), and they want the export's own
  worker + progress pattern. That is the next slice of M4.4; the spec says so.
- **The cache is never pruned.** A cache file lives until the media it describes
  is gone and nothing asks for it again; nothing deletes it. For 1.0 that is
  fine (kilobytes per clip, in the user's own settings directory), but a project
  cleanup pass (M8's "cleanup of unused media" is modelled, not built) is where
  it belongs.
- **A clip whose media is missing is skipped, not cached** — expected, and the
  reason the functional test asserts about its own file rather than about the
  whole pass.
