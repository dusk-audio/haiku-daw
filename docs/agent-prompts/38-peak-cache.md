# Task: M4.4, first slice — waveform peaks as a background job with a disk cache

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/peak-cache` off `master`. This is the first slice of item M4.4 of
`docs/PLAN_1.0.md` ("Background peaks with an on-disk peak cache"), the fourth
item of T5 in `24-continue-1.0-ENTRY.md` §5.

## The defect

`MainWindow::RebuildPeaks` (`src/ui/MainWindow.cpp`) runs **on the window
thread** and, for every clip in the project, opens the file and scans it top to
bottom (`PeakCache::Build` reads every frame) to fill the envelope the timeline
draws. Nothing is kept between runs: a project reopened twice scans every file
twice, and the window is frozen for the whole of it (a 320-clip project is
hundreds of megabytes of reading). It runs on load, on Save-As, on Relink and on
`kMsgUiRefresh`-driven rebuilds.

The plan's words for the fix: "Background peaks with an on-disk peak cache keyed
by path, modification time and size."

## Goal

Opening a project never blocks the window on waveform scanning, the envelopes
appear as they are ready, and a second open of the same media reads them from a
cache instead of decoding the audio again — with the cache never able to draw a
stale waveform for a file that changed.

## Design

1. **The cache name and validity are kit-free** (`src/model/PeakCache.{h,cpp}`),
   because that is where a bug is silent: a wrong key draws the wrong waveform.
   - `PeakCacheKey`: the source file's modification time and size, the bucket
     size, the frame count and the sample rate the envelope was built from.
   - `SerializePeakCache` / `DeserializePeakCache(data, n, want, out)`: a small
     binary format with a magic and a version. **Anything that does not match is
     a miss, never a wrong waveform** — a truncated file, an unknown version,
     another file's envelope, a coarser bucket size, a source whose mtime or
     size moved.
   - `PeakCacheFileName(path)`: a hash of the path, so a path cannot escape the
     cache directory and two paths cannot collide onto one file.
2. **A loader thread** (`src/ui/PeakLoader.{h,cpp}`, Haiku-only, like the rest of
   `src/ui/`): given a project's clip paths and the cache directory, it walks
   them — cache hit or `PeakCache::Build` then save — and hands finished
   envelopes back to the window. It is generation-stamped, so a build for a
   project that has been replaced stops publishing instead of filling the new
   project's map with the old one's clips.
3. **The window drains under its own lock.** The worker never touches the
   window's map: it fills its own, the 60 Hz pulse moves them over while the
   window is locked (which is also when the timeline draws, so the drawing code
   needs no new locking), and the timeline is invalidated once per drain.
4. **The cache lives beside the settings** (`ProjectDocument::PeakCacheDir`),
   created on demand. A cache file that cannot be written is not an error: the
   envelope is still there for this session, and the next one rebuilds.

## Tests

- **Host** (`tests/peak_tests.cpp`, the existing target): the key's equality; the
  file-name hash (stable, and different for different paths); a save/load round
  trip that returns the exact buckets; and rejection of each mismatch — short
  buffer, bad magic, bad version, another file's key (mtime, size, bucket size,
  frame count) — each of which must leave the caller recomputing.
- **VM** (`tests/ui_functional_tests.cpp`): with a project whose clips are real
  files, the envelopes arrive **without the window blocking** (the test posts a
  message and gets it handled while the loader is still working, where the old
  path would have the window in `RebuildPeaks`), all clips end up with an
  envelope, and the counters show the first build as misses and a second pass
  over the same project as **hits** — which is the cache actually being used,
  the only way the VM can prove it.

## Out of scope (say so in the record)

The item's other half: **Freeze, Reverse, Normalize and Strip Silence run as
`RenderJobs` with progress**. They are still synchronous on the window thread
(`RenderJobs::RegionNormalize` and friends decode the whole clip there), and they
want the export's own worker + progress-bar pattern, which this slice does not
touch. The spec for that is the same item, next slice.

## Definition of done

- Host build exit 0, host ctest green, ASan green, `haiku_syntax_check.sh` with
  no new FAIL.
- VM `build` and `build-off` ctest green, `ui_functional_tests` green with the
  new checks and the numbers in the record.
- A mutation check per new behaviour, named in the commit body.
- No window's drawing or layout changes — the timeline invalidates the same way
  it always did; if that turns out to be untrue, `docs/UI_GUIDELINES.md` §1
  applies and the shots get reviewed.
