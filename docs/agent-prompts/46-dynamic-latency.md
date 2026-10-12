# Task: Dynamic LV2 latency — re-solve PDC when a plugin's port moves (M4.6)

Item M4.6 of `docs/PLAN_1.0.md`: "`Lv2Host` polls the latency port off the RT
thread (the meter tick reads a latched atomic the RT thread writes). On a
change, the engine re-solves PDC on the worker thread and swaps the delay lines
in at a block boundary through item 1's graph swap. The exporter re-latches
after its pre-roll. Document the brief re-alignment glitch. Host-test with a
synthetic effect whose latency changes. Update `02-lv2-host-PR.md` with the
decision."

## The decision this implements

Marc, 2026-10-09 (`docs/PLAN_1.0.md:26`, and option 3 of
`02-lv2-host-PR.md`'s open question): **re-solve plugin delay compensation
when a plugin's reported latency changes.** The alternative — latching once at
activate — was the v1 behaviour, and it leaves a plugin that moves its port
(4K EQ 2 was measured 0 → 27 → 0) misaligned by whatever it moved to, for as
long as it stays there, with nothing anywhere to say why.

## What "the latency" means now — the amended IEffect contract

Three questions, three answers, so no host has to guess:

| Question | Call | When | Contract |
| --- | --- | --- | --- |
| What has the host COMMITTED to? | `LatencySamples()` | every graph build | Constant for the graph built from it. Every delay line (PDC edges, `RunInsertSlot`'s dry delay, the key routes) is sized from it ONCE, so it may only change through `RelatchLatency()`. |
| What would the effect report NOW? | `ObservedLatencySamples()` (new; default = `LatencySamples()`) | off the RT thread, by the poll | The live value, which the RT thread has published. An effect that cannot move (every built-in) answers its committed value and no poll ever fires. |
| Adopt that, this build is sized from it. | `RelatchLatency()` (new; default returns `LatencySamples()`) | off the RT thread, after the effect has RUN | Commits the observed value and returns it. |

`Process()` may NOT change what `LatencySamples()` returns: the RT side never
picks a new latency (that is what would silently desynchronise delay lines
already built from the old one). It may only publish what it sees into storage
the poll reads — for LV2, an atomic written once per block from the port the
plugin last wrote.

## The mechanism

1. **Observe (Lv2Host).** `Lv2Effect::Process` publishes the validated
   latency-port reading into `std::atomic<int> fLatencyLive` each block — a
   float load, a range/NaN guard and an atomic store, no allocation, no locks,
   no calls into the plugin. `ObservedLatencySamples()` reads that atomic, so
   the poll never touches the plugin from a foreign thread.
2. **Settle (both hosts, one helper).** `src/dsp/LatencyProbe.h`:
   `SettleLatency(IEffect&)` runs the effect over a short silent PRE-ROLL
   (3 × 1024 frames) and then calls `RelatchLatency()`, returning the settled
   value. A plugin's port is only authoritative once it has RUN — reading it
   straight after activation is what made v1 report 0 for a plugin that is
   really running at 27 — and the pre-roll is what makes the engine's next
   build (and the exporter's render) use the value the plugin is actually
   working with. It is called right after `Prepare()`:
   - engine `BuildGraph`, bus chains and the master chain;
   - exporter `chainLatency` (which sizes the PDC solve and the pad) and
     `applyFx` (which sizes each insert's dry delay) — this is "the exporter
     re-latches after its pre-roll".
3. **Poll and re-solve (engine).** `Engine::FxLatencyMoved()` walks the active
   graph (holding a `GraphPin`), compares each effect's
   `ObservedLatencySamples()` with its `LatencySamples()`, and reports whether
   a re-solve is due. It is called from the UI's 60 Hz pulse — off the RT
   thread by construction — next to the `UpdateMix()` that already runs there;
   a true answer reloads the engine IN PLACE (`LoadMode::InPlace`), so the
   running graph keeps playing while the builder re-solves on its thread and
   `GraphSwap` publishes the new delay lines at a block boundary. No RT thread
   ever waits, allocates or relatches.
   A per-insert guard remembers the observed value a rebuild was already asked
   for, so a plugin whose report never matches what a fresh instance settles
   at cannot ask for a rebuild on every pulse.
4. **The glitch, documented.** Between the plugin starting to report a new
   latency and the rebuilt graph landing, the graph compensates the OLD value:
   the effect's path is misaligned by the difference for those few
   milliseconds, and the swap itself re-aligns it at a block boundary (a step,
   not a fade). That is inherent — no host can compensate a latency it has not
   yet observed — and it is strictly better than v1, which stayed misaligned
   for as long as the plugin kept the new value. The exporter has no such
   window: it settles before it sizes anything, so its bounce is aligned from
   the first sample.

## Work items

1. `IEffect` contract (above) — the amendment is part of this package.
2. `Lv2Effect`: publish, observe, relatch (`Lv2Host.cpp`); `Lv2PluginInfo`
   gains nothing — a plugin need not declare a latency port to be affected,
   and a plugin without one simply never moves.
3. `src/dsp/LatencyProbe.h` + call sites in `Exporter.cpp` and `Engine.cpp`.
4. `Engine::FxLatencyMoved()` + the pulse hook that uses it.
5. Fixture: `stereo-latent` becomes HONEST (it always reported 64 while
   delaying 64, then started announcing 999 after its first run to catch a
   host that re-read the port — under the new policy that plugin is simply
   lying, and trusting the port is the policy). A new
   `urn:haiku-daw:test:latency-step` reports and applies 0 on its first run
   and 128 from its second, which is what a real dynamic-latency plugin looks
   like.
6. Tests:
   - `exporter_pdc_tests`: a synthetic effect via `SetPluginFactory` that
     moves 0 → N on its second run. Bounced with the settle, its impulse must
     land where the dry render's does; with the settle bypassed it must land
     N frames late. (Mutation: skip `SettleLatency` — the test fails.)
   - `lv2_fixture_tests`: with `latency-step`, `LatencySamples()` stays at the
     activated value across `Process`/`Reset` while `ObservedLatencySamples()`
     follows the plugin, and `RelatchLatency()` adopts it — the poll/commit
     contract, against a real plugin.
   - `lv2_render_tests`: a bounce through `latency-step` comes out aligned
     bit-identically to dry — PDC across the LV2 boundary with a latency that
     moves, i.e. the settle end to end.
7. `02-lv2-host-PR.md`: record the decision, the amended contract, and what it
   costs (a lying plugin is now believed; the re-alignment glitch).

## Definition of done

- Host suite green (count in the record), `scripts/haiku_syntax_check.sh`
  0 FAIL for the Haiku-only engine/UI halves.
- Mutations named in the record: no settle; poll always false; poll always
  true (must not wedge the pulse); relatch ignored.
- RT review stated explicitly: the poll reads an atomic; `Process`'s new work
  is a load, a guard and a store; the relatch and the pre-roll happen on the
  builder/offline path only.
