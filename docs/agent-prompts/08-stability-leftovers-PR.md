# fix(record): the stability leftovers (task R3)

Branch `fix/stability-leftovers` off `master` (`de863f4`, package 04 merged).
Three items, one commit each in substance; this is the record.

## 1. Device latency into `fRoundTripFrames` — the real one

`fRoundTripFrames` was always 0: the Media-Kit query the entry called for had
never been written, so the plain take path was compensating by nothing and the
punch and loop-record paths were not compensating at all (the code said so:
"its compensation is a follow-up alongside loop-record").

- **`src/engine/DeviceLatency.{h,cpp}`** (Haiku-only, in `daw_engine`) —
  `DeviceRoundTripFrames(fps)` asks the roster for the audio output and input
  nodes, sums `GetLatencyFor` for both, and converts. It answers **0** wherever
  the roster cannot (no device, no nodes), so a wrong answer degrades to "no
  compensation" rather than to a wild slide.
- **`LatencyUsToFrames`** (kit-free, in `RecordPlan.h`) — the microseconds →
  timeline-frames bridge, round-to-nearest, host-tested.
- **`LoopTakes` grows an optional `captureOffset`** — how many frames of the
  capture belong to the pass BEFORE the loop; dropping them is what makes each
  take start where the loop does. Defaulted, so every existing caller and test
  is unchanged.
- **`MainWindow`** asks once per take (the device can change), and uses it:
  the plain take as before, the punch region intersected against the capture's
  **slid origin** (frame *i* of the file is timeline frame
  `(recStart - roundTrip) + i`), and the loop takes told the offset.

**What is verified, and how.** Host: `recordplan_tests` grew the offset cases
(including an offset that eats the capture, and a negative one) and the
conversion. On the VM, `ui_functional_tests::test_device_latency` calls the
helper and then asks the roster ITSELF, comparing — so a helper that queried
the wrong node, dropped a term or returned nonsense fails there. On this VM the
emulated HDA answers **0 µs for both nodes**, so what is proven here is the
wiring and the arithmetic; the *effect* — a take landing where the player heard
it — needs a device with real latency, which is R5 on the hardware.

## 2. Autosave during recording — already safe, verified rather than changed

The entry offered two options; the code has taken the second since Phase H
(`e973d87`): the autosave handler is guarded

```cpp
if (!fRecMode && !fProject->Tracks().empty()) { ... ProjectIO::Save(...); }
```

and `fRecMode` is set in `StartRecording` and cleared in `StopRecording`, so it
covers the whole take — count-in, overdub, punch and the loop-record passes
alike. A recovery file therefore never references a half-written take, and
autosave resumes by itself the moment the take ends.

No code change: the option the entry named as simplest is the one already in
place, and "make the recovery file tolerate a half-written take" would have
been strictly more machinery for the same guarantee. This paragraph is the
documentation the entry asked for.

## 3. `scripts/vm.sh` builds with `-j2`

The VM has 2 vCPUs; `-j4` thrashed it. (The same script also had a stale
default IP and a `sync` that always pushed `master`; both were fixed on
2026-10-09 by the GitHub-sync commit, which `-j2` now joins.)

## What is NOT verified

| Claim | State |
| --- | --- |
| Takes land where they should on a device with real latency | **R5, hardware.** The VM's nodes report 0 µs, so its take placement is unchanged by construction. |
| The punch/loop compensation end to end | Host tests cover the math; the wiring is compile-verified and its inputs (the device number) are functionally tested. A recording cannot be driven from a test. |
| Autosave-during-recording at runtime | Read, not clicked: the guard is a two-term condition and `fRecMode`'s lifetime is the take. R5's checklist has the "record, wait past 30 s, recover" step. |
