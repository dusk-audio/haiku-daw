# feat(fx): standardize FX as insert slots — bypass, wet/dry, LV2 hook

Brings the per-track and master FX chains to commercial insert-slot semantics:
per-insert **soft bypass** and **wet/dry mix**, plus the shared plumbing the LV2
host and the slot UI will build on.

Three commits:

```
b7d95d3 refactor(fx): one shared definition of what an insert slot does
c30b4dc fix(fx): serialize the LV2 plugin URI, and harden the insert path
898b80e feat(fx): per-insert bypass and wet/dry mix
```

## The pinned contract, as implemented

1. `EffectType::Lv2 = 10`, appended after `Limiter = 9`; `kMaxEffectTypeId = 10`.
   For `Lv2`, `pluginName` holds the plugin URI. Asserted directly in
   `fx_insert_tests`, so a later reorder breaks a test rather than every saved file.
2. `EffectDesc` gains `bool bypassed = false; float mix = 1.0f;`, declared **last**
   so every existing brace-init of the leading fields still compiles.
3. New optional lines `fxin <index> <bypassed 0|1> <mix>` (after a track's `fx`
   lines) and `masterfxin ...` (after the `masterfx` lines), written only when
   `bypassed || mix != 1.0f`.
4. `SetLv2Factory(IEffect* (*)(const EffectDesc&, double sampleRate))` in
   `EffectFactory`. `MakeEffect`'s `case EffectType::Lv2` calls it; a null hook or
   a null return yields `nullptr` — the same fallback as an unavailable native
   plugin, leaving an index-aligned hole in the chain.

## Bypass is soft, and soft bypass must preserve REAL latency

A bypassed insert keeps **reporting** its `LatencySamples()` — required anyway,
since `IEffect` guarantees that value is constant across a `Prepare()`/`Process()`
lifetime — so PDC and every delay line sized from it stay valid, and a toggle
needs no graph rebuild and produces no seam.

Keeping the reported latency obliges us to keep the **real** one. Merely skipping
`Process` on an insert reporting N would drop N samples of actual delay while the
graph still compensates for N, landing that path N samples **early** against the
rest of the mix — an audible flam on every toggle, precisely the artifact soft
bypass exists to prevent. So when `N > 0` a bypassed insert routes its signal
through a delay-only `FrameDelay` path; when `N == 0` (every built-in effect
today) bypass stays a plain skip with no delay line and no cost.

The same line serves the wet/dry blend, whose dry leg must be delayed by N or the
two legs comb-filter. The uses are mutually exclusive and carry the same dry
signal, so one line per insert suffices.

## Correction to the task's stated latency assertion

The task asks the latency tests to assert against *a manually delayed dry
reference*, warning that comparing to the *undelayed* dry "would pass only with
the flam bug present". **Empirically it is the other way round**, and the task's
own reasoning is why: the exporter renders into a buffer padded by the graph's
total latency and then **trims that pad**, so a latent effect is made transparent
rather than shifting the bounce. A correctly bypassed latent insert therefore
keeps its N-sample delay, the trim removes it again, and the render lands
bit-identical to the **undelayed** dry. It is the flam bug that shifts the track N
frames early.

Measured on a 240-frame `LookaheadLimiter` (5 ms @ 48 kHz), one bypassed insert
vs. a no-fx render:

| implementation | matches dry at shift 0 | matches dry at shift +240 |
|---|---|---|
| soft bypass through the delay line (this PR) | yes, bit-identical | no |
| plain-skip bypass (the flam bug) | no | yes, bit-identical |

So each latency case asserts **both** halves — equality at the aligned offset and
inequality at the shifted one — and cannot pass by accident under either
behavior. Both mutants were built and run to confirm the suite catches them.
Asserting against a delayed reference is what would have passed only with the bug.

## Behavior decisions that were NOT pinned

- **`MakeEffect` gained a defaulted `double sampleRate = 0.0` parameter.** The
  pinned LV2 hook takes a sample rate, but `MakeEffect` had none to give it (an
  LV2 instance binds to a rate at instantiation, unlike native add-ons, which are
  configured in `Prepare`). The default keeps every existing call site valid; the
  engine and exporter pass their real output rate.
- **⚠️ The LV2 hook returns a raw `IEffect*`, not `std::unique_ptr`,** matching the
  signature as literally written in the pinned contract. It is inconsistent with
  `PluginFactoryFn` (which returns `unique_ptr`); `MakeEffect` wraps the raw
  pointer immediately, so ownership is unambiguous at the single call site.
  **Still open** — the LV2 agent codes against this, so it is cheap to change now
  and a cross-agent break later.
- **`SetFxBypassCommand(track, fxIndex, bypassed)` is track-only**, exactly the
  3-arg signature in the task, so it has no `master` flag the way `SetFxCommand`
  does. Master-chain bypass therefore rides on `SetFxCommand`'s chain replace,
  which carries the fields for free (tested). Undo names: "Bypass Effect" /
  "Enable Effect". No coalescing, as specified.
- **Malformed-`fxin` policy, split per field** as the task's `mev` note invites: an
  out-of-range **index** is skipped (nothing to attach the state to), an
  out-of-range **mix** is clamped to [0,1], and neither fails the load.
- **The clamp is `!(m >= 0)`** so a NaN lands on 0 instead of passing every
  ordinary comparison. It lives once as `ClampFxMix()` beside `EffectDesc`, shared
  by ProjectIO, the engine, the Exporter and the `kMsgApplyFx` handler, because a
  chain assembled in memory never went through `ProjectIO` at all.
- **The master chain runs through the exporter's `applyFx`** instead of its own
  near-duplicate loop, so per-insert behavior is identical on the master by
  construction.
- **`Engine::fScratch` was repurposed** (it was dead, marked "unused after
  routing") as the one preallocated dry-copy buffer. Nodes and the master run
  sequentially inside a callback, so one buffer serves them all and the RT path
  allocates nothing.
- **UI touch is minimal**, as instructed: `kMsgApplyFx` round-trips `eb` (bypass)
  and `em` (mix) so a knob move cannot strip insert state; `EffName` returns "LV2";
  and an Lv2 insert draws **no** knobs rather than the built-in Biquad row, which
  would otherwise write Hz-scale values into arbitrary LV2 ports. No slot UI —
  that is the other agent's.
- **Known wart, left for the slot UI:** a bypassed effect's meters (e.g. a
  compressor's gain reduction) freeze at their last value rather than reading 0,
  because `Process` no longer runs. Cosmetic, no audio effect.

## Post-review fixes (second commit)

A high-effort review of the branch caught four issues:

1. **🔴 The LV2 plugin URI was never serialized.** All four ProjectIO sites that
   read/write `pluginName` gated on `type == Plugin`, so an `Lv2` insert lost its
   URI on save → load and became a typed hole that could never be re-instantiated
   — a direct break of pinned contract item 1. The round-trip test had asserted
   type, params and mix but **not** the name, which is exactly how it got through.
   Fixed with `EffectHasPluginName()`; the test now pins the URI on both the track
   and the master chain, and reverting the fix fails it.
2. **🟠 The effects editor drew Biquad knobs for an LV2 insert**, wired to param
   slots 1 and 2 with Hz-scale ranges, so a drag would have written garbage into
   arbitrary LV2 ports. It now draws no knobs until the LV2 host can supply real
   port metadata — the same way a Plugin with no loaded add-on already renders.
3. **🟡 The NaN-safe wet/dry clamp had been copy-pasted into four files.** Now one
   `ClampFxMix()`, so the four call sites cannot drift on policy.
4. **🟡 `ProcessAdd` at level 0 could turn a non-finite sample into NaN.** The
   engine kept a latent fully-wet insert's ring current that way, so `0 * Inf`
   would have contributed NaN where the Exporter contributed nothing. New
   `FrameDelay::Push` clocks the ring without summing anywhere.

## The insert path is now defined once (third commit)

`Engine::RunInsert` and the Exporter's inner loop each carried their own copy of
the insert semantics. For this feature that is not ordinary duplication: the
bounce is *required* to be sample-identical to live playback, and two
hand-maintained copies can only stay identical by luck — they had already drifted
in one place. Both now call `RunInsertSlot` in the kit-free
`src/engine/InsertSlot.h`, making that identity structural instead of a claim.

Being kit-free also makes the live path host-testable for the first time. The new
`insertslot_tests` drive it block by block and cover the two things a whole-render
test structurally cannot reach:

- **delay-line state carried across block boundaries**, and
- **a bypass toggle during playback** — an offline render has fixed slot state, so
  no bounce can ever exercise a toggle.

The toggle case is exactly where the delay ring must already be primed. Deleting
the fully-wet ring clocking now fails `insertslot_tests` **while every render test
still passes** — a direct demonstration that this coverage was missing and that no
amount of bounce-level testing would have found it.

No behavior changed: the bit-exact render tests pass unmodified.

## `git diff --stat`

Against the merge base, across all three commits:

```
 CMakeLists.txt                   |  19 ++
 src/dsp/EffectFactory.cpp        |  16 +-
 src/dsp/EffectFactory.h          |  20 +-
 src/engine/Engine.cpp            |  76 ++++++-
 src/engine/Engine.h              |  31 ++-
 src/engine/Exporter.cpp          |  65 +++---
 src/engine/FrameDelay.h          |  18 ++
 src/engine/InsertSlot.h          |  87 ++++++++
 src/model/Commands.cpp           |  14 ++
 src/model/Commands.h             |  22 ++
 src/model/Effect.h               |  44 +++-
 src/model/ProjectIO.cpp          |  56 ++++-
 src/ui/EffectsWindow.cpp         |  16 ++
 src/ui/MainWindow.cpp            |  10 +
 tests/fx_insert_render_tests.cpp | 281 ++++++++++++++++++++++++
 tests/fx_insert_tests.cpp        | 454 +++++++++++++++++++++++++++++++++++++++
 tests/insertslot_tests.cpp       | 238 ++++++++++++++++++++
 tests/projectio_fuzz_tests.cpp   |   9 +-
 18 files changed, 1428 insertions(+), 48 deletions(-)
```

## Tests

Three new ctest targets, all host-buildable:

**`insertslot_tests`** (146 checks) — `RunInsertSlot` block by block: fully wet,
bypassed, `mix = 0`, `mix = 0.5`, and a null insert, at zero latency; then with a
latent effect, that bypass really delays by N, that the delay tail carries across
block boundaries, that the wet/dry legs stay phase-aligned, and that a
**mid-stream bypass toggle** is continuous. Plus: an `Inf` in the dry signal never
surfaces as NaN.

**`fx_insert_tests`** (106 checks) — the pinned enum ids; `EffectDesc` defaults and
brace-init compatibility; `fxin`/`masterfxin` round trip for track and master; no
line written when the chain is all-default; the LV2 **and** native-plugin names
round-tripping; both compat directions via `LoadPatched`; mix clamping and
bad/negative/truncated index handling; `MakeEffect(Lv2)` fallbacks (no hook, stub
hook, unresolvable URI); `SetFxCommand` carrying bypass/mix through do/undo on
both chains; `SetFxBypassCommand` do/undo/redo, non-coalescing, and rejection of
an index that addresses no slot.

**`fx_insert_render_tests`** (29 checks) — real bounces compared **sample-exactly**
(32-bit float, so no dither or quantization): zero-latency bypass / `mix = 0` /
`mix = 0.5` / `mix = 1`; the latency cases with their aligned-equality **and**
shifted-inequality halves; the master chain; and a bypassed insert not disturbing
its neighbours.

**Mutation-tested.** Every claim above was checked by breaking the code and
confirming the suite fails:

| deliberate regression | caught by |
|---|---|
| plain-skip bypass (flam bug) | render 5 failures, insertslot 38 |
| undelayed dry leg (comb bug) | render 4 failures, insertslot 5 |
| stale ring on fully-wet latent insert | insertslot 9 failures, **render 0** |
| LV2 URI gated on `Plugin` only | fx_insert 2 failures |

The third row is the interesting one: it is invisible to every render test, which
is precisely why `insertslot_tests` had to exist.

**Fuzz coverage.** `projectio_fuzz_tests` builds a base project, serializes it,
then poisons every numeric token on every line (huge counts, negatives, `NaN`,
hex, empty). Its base project now carries non-default insert state on both chains,
so the corpus emits `fxin` / `masterfxin` lines and the poisoning reaches their
index, bypass and mix fields — including `NaN` into the mix, which is exactly what
`ClampFxMix` exists for. 3109 checks, clean under ASan/UBSan.

## Compatibility, including the lossy direction

- **New build reads an old file** (no `fxin`): the fields take their `EffectDesc`
  defaults, i.e. exactly the pre-feature behavior. Tested with `LoadPatched`.
- **Old build reads a new file** (with `fxin`): safe. The loader's final `else`
  chain ignores any keyword it does not recognize, so an old build skips the line
  and parses the rest cleanly; no loader change was needed. Tested by re-reading a
  real save with the keyword renamed to one this build does not know, which sends
  it down the identical ignore-unknown path a pre-feature build takes.
- ⚠️ **That direction is LOSSY.** An old build silently **drops** the `fxin` /
  `masterfxin` data, so round-tripping a project through one reverts every
  insert's bypass and mix to the defaults on its next save. It degrades to
  pre-feature behavior rather than corrupting anything, but a user who opens a
  session in an older build and saves it there **will lose their insert settings
  without warning**.
- A project that never touches an insert writes no `fxin` line at all, so its file
  stays byte-identical to a pre-feature save (tested).

## Platform verification

`src/engine/Engine.{h,cpp}` and `src/ui/` build only under `if(HAIKU)`, so they
were written blind on the Linux host and verified on the Haiku VM over SSH:

| where | result |
|---|---|
| Linux host, normal build | 42/42 ctest pass |
| Linux host, `-DDAW_SANITIZE=ON` (ASan + UBSan) | 42/42 pass, clean |
| Haiku VM, full GUI + engine build | **0 errors, 0 warnings** |
| Haiku VM, ctest | 42/42 pass |

The insert **logic** is now fully covered on the host, including the live-only
bypass toggle — sharing `RunInsertSlot` is what made that testable at all.

**What remains unverified is the live WIRING, not the DSP:** `Load` / `SyncFx` /
`FillBuffer` driving the per-insert atomics under a real `BSoundPlayer` — that the
right bypass/mix value reaches the right slot, and that a `SyncFx` push from the
UI thread lands correctly while audio is running. Closing that needs a human at
the VM with audio: put the limiter on a track, play, and toggle its bypass
mid-playback, listening for a click or a timing flam.
