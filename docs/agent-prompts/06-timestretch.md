# Task: Time-stretch for audio clips (WSOLA)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch `feature/timestretch` off master. The DSP and exporter halves are kit-free and fully testable on this Linux host; the live-engine and timeline-UI halves are Haiku-only (pattern-faithful edits, user verifies on VM).

## Codebase orientation (read before coding)

- Layering: kit-free `daw_model` (model + DSP + `src/engine/Exporter.cpp` + `src/engine/WavSource.*` + `src/engine/Resampler.*`) builds and tests here: `cmake -S . -B build-host && cmake --build build-host -j && ctest --test-dir build-host`. `src/engine/Engine.cpp` (realtime) and `src/ui/` are Haiku-only.
- Clip model: `struct Clip` (`src/model/Project.h:21`) — non-destructive reference to a source file: `startFrame/lengthFrames/sourceOffset/sourcePath`, fades, `gain`, take fields. Timeline unit is output-rate audio frames (`Frame = int64_t`).
- Live playback path: one `TrackStream` per clip (`src/engine/Engine.h:45`, impl `Engine.cpp:46-201`): a disk thread (`DiskLoop`, `Engine.cpp:123-155`) decodes the file, runs it through `Resampler` (`src/engine/Resampler.cpp`, linear, used for rate-matching), and fills a lock-free `RingBuffer`; the RT thread drains it in `Mix()` (`Engine.cpp:157-201`) inside the clip window, applying gain/fades and tracking `fSkipDebt` for underrun resync. Seeks rebuild stream position from the timeline frame.
- Offline bounce: `Exporter.cpp` decodes clips directly (find its clip-render path) — live and bounced audio must remain sample-identical, which for stretch means both sides must run the **same** stretch implementation with the same phase/window decisions.
- Serialization: append-only line-based text (`src/model/ProjectIO.cpp`); clip line keyword `clip`. New data = optional trailing tokens or a new optional line (`clipstretch <clipId> <ratio> <semis>` recommended — safer than widening the `clip` line; verify how the loader treats unknown keywords and add compat tests using the `LoadPatched` pattern from `tests/instrument_io_tests.cpp`).
- Mutation funnel: UI → `Command` (`src/model/Commands.h`) → `Project` via `CommandStack`; clip edits like `ResizeClipCommand`/`SetClipFadeCommand` are your templates. Engine picks up structural edits via full rebuild (`Engine::Load`) triggered by `MainWindow::ReloadActiveEngine`.
- Tests: standalone `int main()` + `CHECK` macro, one `add_executable`/`add_test` in `CMakeLists.txt` (lines 76-215), linking `daw_model`. See `tests/exporter_tests.cpp`, `tests/resampler_tests.cpp` for style.

## Goal

Clips can play at a different speed without pitch change (stretch-to-fit by dragging), the top ask being tempo-conforming loops and fitting audio to the project tempo. v1: time-stretch only; pitch-shift field is modeled but may stay unimplemented (see item 1).

## Design decision (made — implement, don't relitigate)

Streaming WSOLA on the disk thread: `DiskLoop` gains a stretch stage between decode/resample and the ring buffer. No cache files to manage, fits the existing decode→ring architecture, and WSOLA's lookahead is fine off-RT. The exporter uses the identical `Stretcher` class on its offline path. WSOLA (not phase vocoder) for v1: simpler, no phasiness on percussive material, adequate quality for 0.5×–2.0×.

## Work items

1. **Model** (`src/model/Project.h`): `Clip` gains `double stretchRatio = 1.0;` (output duration multiplier; 1.0 = off) and `int pitchSemis = 0;` (serialized now so the format is stable; v1 engine may ignore it — if you do implement pitch, it's resample-then-stretch composed ratios). `lengthFrames` stays the *timeline* length; source frames consumed = `lengthFrames / stretchRatio` (pin this convention everywhere, including fades which are timeline-domain). Serialize via the `clipstretch` line; clamp ratio to [0.25, 4.0] on load.
2. **DSP** (`src/dsp/Stretch.h/.cpp`, kit-free): `class Stretcher` — push-model streaming API: `Prepare(sampleRate, channels, ratio)`, `Process(const float* in, int inFrames, float* out, int outCap) -> {inConsumed, outProduced}`, `Flush`, `Reset(sourcePos)`. WSOLA: windowed segments (~50 ms), cross-correlation search (~±10 ms) for best overlap, overlap-add. Deterministic: same input + same start position ⇒ same output, so live/seek/bounce agree. Document algorithm constants in the header.
3. **Seek determinism**: on seek, `TrackStream` restarts decode at a computed source position; a naive WSOLA restart yields different alignment than continuous play. Handle it: quantize stretch state to a fixed synthesis-hop grid anchored at the clip start so `Reset(sourcePos)` reproduces the exact synthesis phase continuous playback would have at that point. This is the invariant your exporter-vs-seek test proves.
4. **Live engine** (`src/engine/Engine.cpp`, Haiku-only): in `DiskLoop`, when `stretchRatio != 1.0`, route decoded (and rate-matched) audio through the clip's `Stretcher` before the ring write. Ring/RT side unchanged. Watch buffer sizing: at ratio 0.25 the disk thread consumes 4× source per output frame — check `RingBuffer` fill loop assumptions and the prefill on seek.
5. **Exporter** (`src/engine/Exporter.cpp`, kit-free): same `Stretcher` on the offline clip path. This is where tests run.
6. **Commands + UI**:
   - `SetClipStretchCommand` (`src/model/Commands.h/.cpp`, kit-free, coalesces during a drag like `SetTrackHeightCommand` does).
   - `src/ui/TimelineView.cpp` (Haiku-only): Cmd/Alt-drag on the clip's right edge = stretch-to-fit instead of trimming; render a small `×1.07`-style badge on stretched clips; follow the existing resize-drag code path (`ResizeClipCommand` handling) and snap rules.
     - **The ratio comes from the clip's currently-referenced source span, NOT the source file's length.** A `Clip` is a non-destructive window into its file: it starts at `sourceOffset` and consumes `lengthFrames / stretchRatio` source frames (the convention pinned in item 1). So `newRatio = newTimelineLen / currentSourceSpan`, where `currentSourceSpan = oldLengthFrames / oldStretchRatio`. Using the whole file length instead would compute a wildly wrong ratio for any trimmed clip and would pull in material the user had trimmed away — and every loop dragged in from the browser is trimmed. `lengthFrames` still becomes `newTimelineLen` exactly as the existing resize path sets it; only the ratio derivation is new.
     - If the source has a hard end (offset + span already reaching the file end), stretching must not read past it — clamp the span to what the file actually holds before dividing, so the ratio stays finite and the stretch does not run off the end.
7. **Docs**: one paragraph in `docs/ARCHITECTURE.md` on the stretch stage position in the signal chain and the determinism invariant.

## Definition of done

- New ctest targets: `stretch_tests` — output length within one hop of `in * ratio` across ratios {0.5, 0.8, 1.0 (bit-exact passthrough — ratio 1 must bypass), 1.25, 2.0}; sine input keeps its frequency within a few cents (zero-crossing count); determinism: full render == chunked push-model render == render started from `Reset(midPos)` for the overlapping region. `exporter_stretch_tests` — a stretched clip bounces to the expected timeline length and content; unstretched projects bit-identical to before your change (regression guard).
- Trimmed-clip coverage (kit-free, so it runs here — this is the case the ratio derivation is easy to get wrong): a clip with a **nonzero `sourceOffset`** and a length shorter than its file, stretched to fit. Assert the resulting `stretchRatio` is computed against the trimmed span rather than the file length, and that the bounce consumes exactly `lengthFrames / stretchRatio` source frames starting at `sourceOffset` — i.e. the stretched clip contains the same source material as before the stretch, just slower or faster.
- All existing ctests pass (especially exporter + resampler suites).
- PR description: WSOLA constants chosen, the seek-determinism mechanism, CPU cost estimate per stretched stream, and what pitch-shift v2 would need.
- **UI (any change to what a window draws or how it is laid out):** screenshots reviewed per `docs/UI_GUIDELINES.md` — `DAW_UI_SHOTS` run on the unlocked VM, a `Shot()` for every window you add or change, every shot opened and checked against its section 3 list, what you looked at and fixed named in the PR record. Green tests alone are not done.
