# feat(plugin): observe and re-latch a plugin's latency (M4.6)

Spec: `46-dynamic-latency.md`. Decision recorded in `02-lv2-host-PR.md`
(Marc, 2026-10-09: follow the port; option 3 of the four that record listed).

```
e553276 feat(plugin): observe and re-latch a plugin's latency (M4.6)
5ea012e docs: spec dynamic LV2 latency (M4.6)
```

## The amended contract

`LatencySamples()` stays the COMMITTED value — constant for the graph built
from it, because every delay line (PDC edges, `RunInsertSlot`'s dry line, the
key routes) was sized once — and two calls carry the change:

| Call | Where | Who implements it |
| --- | --- | --- |
| `ObservedLatencySamples()` | off the RT thread, by the poll | `Lv2Effect`: the atomic `Process` publishes each block from the port the plugin last wrote |
| `RelatchLatency()` | off the RT thread, after a pre-roll | `Lv2Effect`: adopts the port's value |

`dsp/LatencyProbe.h`'s `SettleLatency()` is the pre-roll (3 × 1024 silent
frames) plus the relatch. Both hosts call it right after `Prepare`:

- `Engine::BuildGraph`, every bus chain and the master chain — which is also
  what makes a latency-triggered rebuild converge on the first try;
- `Exporter::chainLatency` (PDC solve + pad) and `Exporter::applyFx` (each
  insert's dry line) — the exporter's re-latch after its pre-roll.

`Engine::FxLatencyMoved()` is the poll: off the RT thread, called from the UI
pulse beside `UpdateMix()`, it pins the graph, compares observed against
committed per insert and records what a rebuild was already asked for. A true
answer reloads IN PLACE, so the builder re-solves while the old graph plays and
`GraphSwap` swaps the new delay lines in at a block boundary.

## RT safety (checked explicitly, as the item requires)

- `Lv2Effect::Process`'s new work per chunk: one float load out of host memory
  the plugin writes, a NaN/range guard, and an atomic store. No lock, no
  allocation, no call into the plugin, no I/O.
- The poll runs on the UI thread: a `GraphPin` (an atomic counter), loads and
  comparisons, and an insert into a vector the pulse alone touches.
- `SettleLatency` (which RUNS the effect) is called only from the graph build
  and from the exporter, both off the RT thread, both before the instance is
  visible to the audio thread.
- The relatch is never called from the audio path; nothing in `FillBuffer` or
  anything it calls learns about the new calls.

## The glitch, as required

Between the plugin starting to report a new latency and the rebuilt graph
landing, the graph still compensates the old value (a few ms at 60 Hz polling);
the swap itself re-aligns at a block boundary — a step, not a fade. No host can
compensate a latency it has not observed, and v1 stayed misaligned for as long
as the plugin kept the new value. The exporter has no such window.

## Tests and mutations

New host behaviour: `exporter_pdc_tests`' synthetic `MovingLatencyPlugin` (0
until it has run twice, then N — modelled on `LatencySamples` /
`ObservedLatencySamples` / `RelatchLatency` exactly), the fixture's new
`latency-step` (honest: 0 on run 1, 128 from run 2, reported AND applied),
`lv2_fixture_tests`' observe/relatch contract block, `lv2_render_tests`' bounce
through `latency-step`, and two invariants in `lv2_host_tests`.

| Mutation | Caught by |
| --- | --- |
| exporter settle dropped in `chainLatency` | `exporter_pdc_tests` (impulse lands late; tail cut) |
| `Process` no longer publishes the live value | `lv2_fixture_tests` (Observed never moves) |
| `RelatchLatency` made a no-op | `lv2_fixture_tests` (4 checks) |

The engine's own poll and in-place reload are Haiku-only and therefore not
host-testable; what they are built on (the two calls, the settle, the fixture's
honest move) is.

## Fixture change, and why it was necessary

`stereo-latent` used to report 999 from its second run while delaying 64 — the
double that proved "the host latches once and never re-reads". Under the
decided policy that plugin is simply lying (and is believed), so it would make
`lv2_render_tests`' PDC assertion assert nothing true. It is now honest (always
64), and `latency-step` covers the moving case.

## Verification

```
$ cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build build-host -j2
exit 0
$ ctest --test-dir build-host
100% tests passed out of 69
$ sh scripts/haiku_syntax_check.sh
75 OK, 4 FAIL (the pre-existing codec files); src/engine/Engine.cpp,
src/engine/Exporter.cpp, src/plugin/Lv2Host.cpp and src/ui/MainWindow.cpp all OK
```

`ui_functional_tests` and the VM pass are the orchestrator's; the UI change here
is the pulse calling the poll (no drawing change).
