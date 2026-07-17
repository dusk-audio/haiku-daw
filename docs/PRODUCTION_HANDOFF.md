# Production-Readiness Handoff

Paste this to a fresh session to continue the production-release push. Read
`docs/HANDOFF.md` + `docs/ARCHITECTURE.md` first (project background);
`docs/ROADMAP.md` holds the earlier feature-phase history (A–H). This document is
self-contained: the P0/P1 findings and the Phase X/Y/Z/I roadmap are summarized
in full below, so no external/unversioned plan file is needed to continue.

## Where we are

A 5-agent audit of the DAW found the gap to a shippable commercial product is
**stability/durability/depth, not features**. The **P0 ship-blockers (all 10) and
the P1 correctness batch are fixed and committed**, plus export mastering
(Phase X) is under way (bit-depth + dither + loudness normalization done). Work
is **committed on `master`** as of `c3eb04c` (author `marc@duskaudio.com`, no AI
trailer, conventional subjects); the VM checkout tracks it via the `git bundle`
sync. Future commits still land on `master` only when Marc asks.

### Verification (all green)
```
cmake --build build && (cd build && ctest)          # 29/29 pass
cmake -B b-asan -DDAW_SANITIZE=ON && ctest --test-dir b-asan   # ASan/UBSan clean
sh scripts/haiku_syntax_check.sh                    # 0 FAIL (engine/UI cross-compile)
sh scripts/vm.sh test                               # VM on-target: 29/29 pass
```
Host + on-target tests cover the kit-free changes; the engine/UI changes are
cross-compile-clean and build+test green on the VM, but the **audio-listening
checks still need Marc at the VM** (see "What's next" item 1).

## Landed (committed on master, host + on-target tested)

**P0 — ship blockers (10/10 done):**
- `Engine.cpp` `FillBuffer`: FTZ/DAZ denormal flush + `isfinite`→0 sweep before
  the DAC (main + monitor paths) — kills NaN blast + denormal xruns.
- `ProjectIO.cpp`: atomic save (tmp+rename+`enddaw` trailer); Load parses into a
  temp `Project`, version-gates (`DAW 2`→reject), swaps only on clean parse (a
  corrupt file never wipes the session); count loops capped (`kMaxListCount`) +
  stream-guarded; parse locals initialized.
- `WavSource.cpp`: reject float-fmt with bits≠32 (OOB read) + bad PCM depth; cap
  fmt chunk to 4 KB.
- `Exporter.cpp`: OOM fix — persistent full-length buffers only for routing
  destinations (buses/send-dests), leaf tracks share one `scratch`; `DecodeClip`
  bounded to the clip window.
- `Commands.cpp`: `MoveClipToTrackCommand` + `MoveMidiClipCommand` pre-check dest
  id-collision and re-home the source on add-failure (no silent data loss).

**P0 #9 + #10 — DONE (fixed in code; VM runtime verification still pending):**
- **#9 `UpdateMix` gain race** (`Engine.cpp`): FIXED — `Bus` mix params
  (`midiGainL/R`, `busGainL/R`, `audible`) are now `std::atomic` (relaxed), with a
  hand-written noexcept move-ctor so the move-only `std::vector<Bus>` still moves.
  The non-atomic UI/RT access — a real data race independent of any NaN
  containment — is resolved at the source. (The P0#1 NaN guard only bounds the
  *output*; it never made the race safe.) Still wants VM runtime confirmation
  (drag faders + toggle mute during playback → no glitch).
- **#10 monitor/live-MIDI UAF** (`Engine.h` fMonSource/fLiveMidi): FIXED — a
  callback-generation counter + `Engine::QuiesceMonitorInput()` (returns false if
  quiescence can't be confirmed) lets teardown wait out any in-flight RT deref
  before freeing the source; the caller falls back to `Stop()` on a false return.
  Wants VM runtime confirmation (stop MIDI record/monitor while playing).

**P1 — correctness (host-tested where kit-free; engine items cross-compiled):**
- `Exporter.cpp`: bus-stem silence fixed (mute skips node+path; a solo-excluded
  DEST still passes accumulated upstream; own material gated by `audible`);
  `ExportStems` clears `soloSafe`; offline FTZ.
- `Project.h` `CollectNotes`: note length clamped to the clip window.
- `ProjectIO.cpp`: `setprecision(9)`; escape-aware `Quote`/`Unquote` (embedded
  quote/newline no longer corrupts a line); `inputMonitor` serialized;
  `midiClips` sorted on load.
- `Reverb.cpp`: Freeverb fixed input gain (0.015) — was ~50× wet overload.
- `SmfIO.cpp`: `has()` guards `p<=end` (signed-underflow OOB).
- `Engine.cpp`: MIDI panic — `Stop()` clears voices after `fPlayer->Stop()`;
  underrun `fSkipDebt` resync (drops stale ring frames instead of desyncing).
- `Recorder.cpp`: xrun silence-pad preserves take length + advances the envelope.

**Phase X (export mastering) — in progress:**
- `WavWriter`: `OpenFormat(bits/float)` + `WriteFloat` with TPDF dither (16-bit),
  24-bit PCM, 32-bit float; NaN/clamp sanitize; 4 GB guard. `Exporter::ExportWav`
  gained `bitDepth` (16/24/32); 16-bit is dithered. Host test asserts the fmt
  chunk's format+bits match the requested depth.
- **Loudness normalization** ✅ — `ExportWav` takes an `ExportNormalize` option:
  measure integrated LUFS + true peak (kit-free `Loudness`, BS.1770), apply one
  gain to the target LUFS, backed off so the output never crosses the dBTP
  ceiling (true-peak-*safe* normalization). Host-tested (hits target; ceiling
  wins on a loud target).
- **Look-ahead true-peak limiter** ✅ — kit-free `dsp/Limiter`: offline,
  4x-oversampled true-peak detection (same FIR as `Loudness`), stereo-linked
  gain, look-ahead window-min + anticipatory backward-smoothed attack (zero
  added latency, so no PDC needed on the master sink) + exponential release;
  guarantees the output dBTP ceiling. Wired into `ExportWav` via
  `ExportNormalize::limiter` — with normalization on, the program is pushed to
  `targetLufs` and the limiter (not a whole-mix attenuation) holds the ceiling,
  so quiet material reaches target loudness; may also be used without
  normalization to peak-limit only. Host-tested (`tests/limiter_tests.cpp` +
  exporter integration cases: reaches a loud target the gain-backoff path
  couldn't, ceiling still held). ASan-clean.
- *Remaining:* export bit-depth/rate/format **selection UI** + worker-thread
  export with progress/cancel (both GUI, need VM runtime); FLAC/Ogg/MP3 encoders
  (bundled libs — no Media Kit decoders on target).

## What's next (priority order)

1. **VM runtime-verify the engine changes** — `sh scripts/vm.sh ssh`, build,
   launch `~/haiku-daw/build/daw`. Drive: play/stop/seek/loop, record + overdub
   (force an xrun with a tiny buffer to exercise the pad + resync), MIDI monitor
   note-held-at-stop (panic). Confirm no click/blast on seek/loop/mute and no xrun
   on a denormal-decay reverb tail. Also confirm the P0#9/#10 fixes at runtime
   (drag faders + mute during playback; stop MIDI record/monitor while playing).
   NOTE: 24/32-bit **export** has no selection UI yet — the app's File▸Export
   still hard-passes 16-bit — so bit-depth output is currently exercised by the
   `ExportWav(bitDepth)` API in `exporter_tests` (asserts fmt-chunk format+bits
   for 24/32), NOT by driving the GUI. GUI bit-depth verification waits on the
   Phase X selection UI (item 2).

2. **Finish Phase X** — bit-depth/rate/format **selection UI** in the export
   flow (`MainWindow.cpp` export path currently hard-passes `sampleRate`, 16-bit,
   no normalization/limiting — needs to surface `bitDepth` + `ExportNormalize`
   {enabled, targetLufs, truePeakCeil, limiter}); move export **off the UI
   looper** onto a worker thread with progress/cancel (`MainWindow.cpp:444,467`);
   **FLAC/Ogg/MP3 encoders** (need bundled libs — NO Media Kit decoders on the
   target; decide MP3/LAME licensing with Marc). (Loudness-normalize + true-peak
   ceiling + look-ahead true-peak limiter: done.)

3. **Phase Y — latency compensation**: record round-trip offset (`Recorder`/
   StartCapture; query device latency) + plugin delay compensation
   (`IEffect::LatencySamples()`, delay-align sibling buses in `Engine` + `Exporter`).

4. **Phase Z — MIDI depth**: `MidiClip` stores events not just notes (CC/PB/PC/
   sysex — hook the remaining `BMidiLocalConsumer` overrides in `MidiPort.cpp`,
   extend `MidiRecorder`); per-track MIDI input demux (`MainWindow.cpp:1201` uses
   one merged consumer for all armed tracks); CC lanes; MIDI clock/MTC.

5. **Phase I+ — LV2 hosting** on top of the existing native-`.so` `PluginHost`.

6. **P1 minors remaining**: autosave-during-recording (`MainWindow.cpp:1176`);
   denormal DC-bias belt-and-suspenders on the IIR state (deferred — FTZ already
   covers it, DC bias needs audio verification). (DONE: Eq FFT off the RT thread —
   the audio thread now only captures + double-buffer-publishes each 512-sample
   analyzer frame; the FFT runs lazily on the UI thread in `Eq::Spectrum()` (the
   meter poll), so the transform is off the RT path. Host-tested, ASan-clean.)
   (DONE: zipper-noise smoothing — ~10 ms coefficient glide in Biquad/Eq +
   makeup/range-floor glide in Compressor/Gate; a stepped/automated param ramps
   instead of clicking. RT-safe (arithmetic on preallocated members). Host-tested
   `tests/smoothing_tests.cpp`, ASan-clean.)
   (DONE: marker identity by frame+name; Resampler mono/channel-aware; undo
   history cap; WavWriter >4 GB guard; WavWriter NaN/clamp sanitize; WavSource
   zero-rate reject; SmfIO checked skip; ProjectIO strict header + escaped-format
   marker + required-track validation — all with regression tests.)

## Hard constraints (don't break)
- **RT audio callback**: no alloc / locks / file I/O — only arithmetic on rings +
  atomics. Disk threads do I/O.
- **No `BMediaFile`** — the target image has no Media Kit decoder plugins; we do
  our own WAV I/O. Compressed formats need a bundled/own decoder.
- Kit-free code is host-tested (`ctest`); engine/UI needs `haiku_syntax_check.sh`
  then VM runtime. **Every P0/P1 fix ships a regression test.**
- Commits: `marc@duskaudio.com`, conventional-commit subjects, **no AI trailer**,
  no GitHub remote (git-pull loop to the VM).

## Adversarial tests — DONE
Fuzz corpora for `WavSource` + `ProjectIO` (truncated/oversized/garbage → no
crash/OOM, clean failure — `tests/wavsource_fuzz_tests.cpp`,
`tests/projectio_fuzz_tests.cpp`, ASan-clean, CTest `TIMEOUT`); a NaN-injection
export test proving the master finite-sweep zeroes NaN/Inf (32-bit-float bounce
so the guard, not the quantizer, is what's under test); a malformed-SMF test
(bad track/meta length → rejected, no OOB); leaf-scratch / bus-stem export tests
proving large sessions stay memory-bounded and bus-routed stems aren't silent.
Build under ASan for teeth: `cmake -B b-asan -DDAW_SANITIZE=ON`.
