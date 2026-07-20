# Handoff into Package 02 (LV2 host) — read BEFORE `02-lv2-host.md`

Package 01 (`fx-inserts-core`) is merged. `02-lv2-host.md` was written before 01
existed, so a few of its statements are now stale or wrong. **This file is
authoritative where the two disagree.** Everything not mentioned here still
stands.

## Where things are

Master is at `b7d95d3`, three commits:

```
b7d95d3 refactor(fx): one shared definition of what an insert slot does
c30b4dc fix(fx): serialize the LV2 plugin URI, and harden the insert path
898b80e feat(fx): per-insert bypass and wet/dry mix
```

Verified: 42/42 ctest on the Linux host, 42/42 under `-DDAW_SANITIZE=ON`
(ASan+UBSan), and on the Haiku VM a full GUI+engine build with **0 errors, 0
warnings** and 42/42. `docs/agent-prompts/01-fx-inserts-core-PR.md` records every
decision, including a correction to a claim made in 01's own task doc — read it if
01's behavior ever surprises you.

## The contract as BUILT — this is what you implement against

**The hook returns a RAW pointer.** 02's prose just says "registered through
`SetLv2Factory`"; the actual signature in `src/dsp/EffectFactory.h` is:

```cpp
using Lv2FactoryFn = IEffect* (*)(const EffectDesc& desc, double sampleRate);
void SetLv2Factory(Lv2FactoryFn fn);

std::unique_ptr<IEffect> MakeEffect(const EffectDesc& desc, double sampleRate = 0.0);
```

- It returns `IEffect*`, **not** `std::unique_ptr` — deliberately, because that is
  how the pinned contract literally spelled it. It is inconsistent with
  `PluginFactoryFn` (which returns `unique_ptr`), and the user has been told it is
  still open. **Do not change it unilaterally**; if you want it changed, ask.
  `MakeEffect` wraps your pointer immediately, so ownership transfers on return.
- You receive the **whole descriptor** (URI is in `desc.pluginName`) plus the
  target sample rate, since an LV2 instance binds to a rate at instantiation.
- **`MakeEffect` already applies `desc.params` for you** via `SetParam(i, v)` for
  every `i` in `[0, params.size())`, *after* your hook returns. So your factory
  only has to cover the slots the descriptor does NOT carry (port defaults).
  Re-applying them yourself is harmless, just redundant.
- **Guard `sampleRate <= 0`.** The parameter is defaulted, and a few existing call
  sites (`tests/effect_tests.cpp`, `tests/lookahead_limiter_tests.cpp`) still call
  `MakeEffect(desc)` with no rate. The engine and Exporter always pass a real one.

**The soft-fallback path is already built and tested — verify it, don't rebuild
it.** A null hook or a null return makes `MakeEffect` yield `nullptr`; both the
engine and the Exporter keep an index-aligned `nullptr` hole in the chain and skip
it. Covered in `tests/fx_insert_tests.cpp`.

**URI persistence is already done.** `EffectHasPluginName(EffectType)` in
`src/model/Effect.h` gates the `pluginName` field in all four ProjectIO
read/write sites, so `Lv2` URIs round-trip on both the track and master chains,
with tests. 02's work item 4 is therefore **verify-only**. This was a real bug
caught in review — please don't regress it.

## Direct corrections to `02-lv2-host.md`

**`kMaxLv2BlockFrames`: there is no upper bound to "cover".** 02 tells you to
check `Engine::SetBufferFrames`'s accepted range and pick a constant that covers
it. That setter is `if (n >= 32) fBufferFrames = n;` (`src/engine/Engine.h:203`) —
**unbounded above**. So no compile-time constant can be proven sufficient, and the
chunking loop in `Process` is the *only* correct option, not merely the
"preferred" one. Size your scratch to the constant, loop the input in chunks of at
most that, and unit-test the over-max path. For reference the Exporter's offline
block is 8192 frames, so pick `>= 8192` or you will chunk on every offline block.

**`LatencySamples()` has a second consumer now.** Beyond PDC, `RunInsertSlot`
(`src/engine/InsertSlot.h`) sizes a per-insert dry-delay line from it once, when
the chain is built. A plugin whose latency moved after `Prepare` would silently
misalign both soft bypass and the wet/dry blend. 02 already tells you to latch it
at `Prepare` — this is the extra reason it matters.

## New shared code from 01 you should know about

- **`src/engine/InsertSlot.h` — `RunInsertSlot()`** is the single definition of
  what an insert does (soft bypass, wet/dry blend, dry-path delay), called by both
  the RT engine and the offline Exporter so the bounce stays sample-identical to
  playback. If LV2 ever needs something from the insert layer, it belongs in that
  one function — **do not add LV2 special-cases to either caller.**
- `ClampFxMix()` and `EffectHasPluginName()` in `src/model/Effect.h`.
- `FrameDelay::Push()` in `src/engine/FrameDelay.h` (clocks a delay ring without
  summing anywhere).
- **Your plugin's `Process` will be skipped entirely for some blocks** when the
  user bypasses that insert, then resume later. The plugin will see a state
  discontinuity on un-bypass. That is accepted behavior, not a bug for you to fix.

## UI hook you are enabling (mostly 03's problem, but shape for it)

`KnobsForDesc` in `src/ui/EffectsWindow.cpp` currently returns **no knobs** for
`EffectType::Lv2`, on purpose: falling through to the built-in table would have
drawn the Biquad row and written Hz-scale values into whatever LV2 ports sit at
slots 1 and 2. The natural extension is for it to query `Lv2Host` exactly the way
it queries `PluginHost` for native add-ons — so keep your listing API
(name / URI / param count / param name+min+max+default) shaped like
`PluginHost`'s, as 02 already asks.

## Environment facts, so you don't rediscover them

- **lilv 0.28.0 is installed on this Linux host** (`pkg-config --modversion
  lilv-0`), so you can build and test the real LV2 path here.
- **Real LV2 plugins ARE available here — but not where you'd first look.** This
  is openSUSE, so the system bundle dir is `/usr/lib64/lv2` (`/usr/lib/lv2` is
  empty). It holds mostly LV2 *spec* bundles (core, atom, urid, …) plus the
  `eg-fifths` / `eg-params` examples, neither of which is a plain stereo audio
  effect. The useful ones are the 15 bundles in **`~/.lv2`** — `4K EQ`,
  `Multi-Comp`, `Multi-Q`, `Tape Echo`, `TapeMachine`, `Convolution Reverb`,
  `DuskAmp`, `DuskVerb`, and others. `LV2_PATH` is unset, so lilv's default search
  path applies. A genuine instantiate-and-process test is therefore possible —
  but keep 02's skip-if-absent design, since these live in a home directory and
  are machine-specific.
- **lilv is NOT installed on the Haiku VM**, though HaikuPorts has it:
  `pkgman install lilv lilv_devel lv2`. Until it is, the VM verifies only the
  `-DDAW_LV2=OFF` path — which is a required DoD item anyway, so still run it.
  Ask the user before installing packages on their VM.

## Verification workflow for this repo

- Host: `cmake -S . -B build-host && cmake --build build-host -j && ctest --test-dir build-host`
- Sanitizers: `cmake -S . -B build-asan -DDAW_SANITIZE=ON` — then **delete the
  directory**, it is not in `.gitignore`.
- Haiku VM: `sh scripts/vm.sh test`. **Gotcha that will bite you:** that script
  hardcodes `master` in its `git bundle create`, so running it on a feature branch
  silently builds *stale master* and still reports success. To verify a branch,
  bundle the ref explicitly:
  `git bundle create /tmp/b.bundle <branch>` → `scp` it → on the VM
  `git fetch /tmp/b.bundle <branch> && git reset --hard FETCH_HEAD`.
  VM is `ssh -i ~/.ssh/haiku_vm user@192.168.122.232`, repo at `~/haiku-daw`,
  host is authoritative and the VM never commits.
- `src/engine/Engine.*` and `src/ui/` **cannot compile on Linux** — the VM is the
  only compile check for them. `src/main.cpp` (your wiring step) is in that group.

## Process notes that earned their keep in 01

- **Mutation-test your key assertions**: break the code deliberately, confirm the
  test fails, revert. In 01 this caught a test that was asserting the wrong thing
  entirely, and proved three separate regressions were actually covered.
- **Assert the thing you actually control.** Where 01 could not run an old binary,
  it tested the property the compatibility claim rested on instead.
- Report honestly what is unverified. For 02 that will include anything needing
  lilv on Haiku, and any plugin class you could not obtain a sample of.
