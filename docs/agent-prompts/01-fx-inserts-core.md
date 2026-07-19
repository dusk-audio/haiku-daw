# Task: Standardize FX as insert slots — model, engine, serialization (core)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch `feature/fx-inserts-core` off master. Use conventional commits (`feat(fx): ...`).

## Codebase orientation (read before coding)

- Layering: the kit-free static lib `daw_model` (model + DSP + synth + Exporter, STL only) builds on any host and carries the entire ctest suite. The GUI (`src/ui/`) and realtime engine target build only under `if(HAIKU)` in `CMakeLists.txt` and **cannot be compiled on this Linux host** — edit them pattern-faithfully; the user verifies on a Haiku VM. Build/test loop: `cmake -S . -B build-host && cmake --build build-host -j && ctest --test-dir build-host`.
- Mutation funnel: UI → `Command` subclass (`src/model/Commands.h`) → `Project` mutators, executed via `CommandStack` (`src/model/Command.h`). Commands never touch the engine; `MainWindow` re-syncs the engine after `Execute` (in-place via `Engine::SyncFx` when the chain structure matches, else full rebuild via `Engine::Load`).
- RT rules (`src/engine/Engine.h:8`): the audio callback `Engine::FillBuffer` (`src/engine/Engine.cpp:794`) reads only rings and atomics — no allocation, locks, or I/O. Live param changes go through `IEffect::SetParam`, which must be RT-safe.
- Effects: `daw::IEffect` (`src/dsp/IEffect.h`) — `Prepare` off-RT, `Process(float* stereo, int frames)` in-place interleaved stereo, `SetParam(slot, value)` RT-safe, `LatencySamples()` feeds PDC. Concrete effects in `src/dsp/` (Biquad, Delay, Reverb, Compressor, Eq, Saturator, Gate, Widener, LookaheadLimiter).
- Model: `EffectDesc {EffectType type; std::vector<float> params; std::string pluginName;}` (`src/model/Effect.h:33`). `Track.fx` is an ordered `std::vector<EffectDesc>` (`src/model/Project.h:130`); `Project.masterFx` is the master chain. `MakeEffect` (`src/dsp/EffectFactory.cpp:18`) instantiates; native add-on plugins route through the `SetPluginFactory` hook (`src/dsp/EffectFactory.h:18`) so the kit-free lib never links Haiku.
- Engine FX path: each track becomes a `Bus` (`src/engine/Engine.h:306`) holding `std::vector<std::unique_ptr<IEffect>> fx` + parallel `fxTypes`, built in `Engine::Load` (`Engine.cpp:379-384`). Per block the chain runs in `FillBuffer` (`Engine.cpp:952-953`) after FX-param automation (`:947-951`). `Engine::SyncFx` (`Engine.cpp:583`) pushes params in place when structure matches; `SetFxParamLive` (`:611`) handles single knob drags. `Exporter::ExportWav` (`src/engine/Exporter.cpp:133`) runs the identical graph offline in 8192-frame blocks (`applyFx`, `:311-337`) — **the bounce must stay sample-identical to live playback**.
- Serialization: line-based text (`src/model/ProjectIO.cpp`), append-only versioning — new data goes in optional trailing tokens or new optional lines; enums clamp against `kMax*` on load; parse into a temp `Project`, commit only on clean parse. FX lines: `fx <typeInt> <count> <params...>` (+ quoted pluginName when type==Plugin), written at `ProjectIO.cpp:175` (track) and `:118` (master).
- Tests: each test file is a standalone `int main()` with a hand-rolled `CHECK` macro, one `add_executable` + `add_test` in `CMakeLists.txt` (see lines 76-215), linking `daw_model` only. `tests/instrument_io_tests.cpp` shows the `RoundTrip(desc)` and `LoadPatched(desc, from, to)` patterns for forward/backward-compat IO tests — copy that approach.

## Goal

Bring the per-track FX chain to commercial insert-slot semantics: per-insert **bypass** and **wet/dry mix**, plus the shared plumbing that the LV2 host (separate agent) and slot UI (separate agent) will build on. Pinned contract — do not deviate, other agents depend on these exact values:

1. `EffectType::Lv2 = 10` appended after `Limiter = 9`; `kMaxEffectTypeId = 10`. For Lv2, `pluginName` holds the plugin URI.
2. `EffectDesc` gains `bool bypassed = false; float mix = 1.0f;`.
3. New optional serialization line per non-default insert: `fxin <index> <bypassed 0|1> <mix>` after the track's `fx` lines, and `masterfxin ...` for the master chain.
4. New kit-free hook in `EffectFactory`: `SetLv2Factory(IEffect* (*)(const EffectDesc&, double sampleRate))`, mirroring `SetPluginFactory`. In `MakeEffect`, `case EffectType::Lv2` calls the hook; a null hook or null return falls back the same way an unknown native plugin does today (inspect and match the existing Plugin fallback behavior).

## Work items

1. **Model** (`src/model/Effect.h`): the enum value, `kMaxEffectTypeId`, the two new `EffectDesc` fields. Keep the struct aggregate-friendly; check all existing brace-init sites still compile.
2. **Serialization** (`src/model/ProjectIO.cpp`): write `fxin`/`masterfxin` only when `bypassed || mix != 1.0f`. On load, clamp `mix` to [0,1], ignore out-of-range indices. Note that `mev`-style records fail the load on an out-of-range field rather than clamping — follow whichever of the two the field warrants (a `mix` outside [0,1] is meaningless but harmless, so clamp; an index that can't address a slot is skipped).

   The two compatibility directions are NOT the same claim — keep them apart, and only the first is testable in this repo:
   - **New build reads an old file** (no `fxin` line): the fields take their `EffectDesc` defaults (`bypassed = false`, `mix = 1.0f`). Test this with `LoadPatched`.
   - **Old build reads a new file** (with `fxin`): verified to be safe — the loader's final `else` chain ignores any keyword it doesn't recognize (`ProjectIO.cpp:551-553`, `"endtrack" and unknown keywords: ignored`), so an old build skips `fxin` lines and parses the rest cleanly. You do not need to make the loader tolerant; it already is. You cannot write a real regression test for this direction (there is no old binary to run), so assert the property you actually control instead: that an unknown keyword does not fail `Load`.

   **Document this limitation:** an old build silently DROPS the `fxin` data, so round-tripping a project through one is lossy — bypass/mix revert to defaults on its next save. That is acceptable (it degrades to the pre-feature behavior) but it must be stated in the PR description, not discovered by a user.
3. **Factory** (`src/dsp/EffectFactory.h/.cpp`): the Lv2 hook and case.
4. **Engine** (`src/engine/Engine.cpp/h` — Haiku-only, no host compile):
   - Per-insert atomic bypass flag and mix value on `Bus` (parallel arrays next to `fx`/`fxTypes`), set at `Load` and updated by `SyncFx`.
   - In the FX loop: skip `Process` when bypassed (**soft bypass**: keep reporting the effect's `LatencySamples()` so PDC and graph latency stay constant — no rebuild, no click; `IEffect.h:54` requires that value to be constant across a `Prepare()`/`Process()` lifetime anyway, so this is the only correct option). Document the choice in a comment.
   - **Bypass must preserve the effect's actual latency, not just its reported one.** Skipping `Process` on an insert with `LatencySamples() == N` removes N samples of real delay while PDC still compensates for N, so the bypassed track lands N samples EARLY against the rest of the mix — an audible flam on bypass, i.e. exactly the click the soft bypass was meant to avoid. When bypassed and `LatencySamples() > 0`, route the signal through a delay-only path of N frames (`FrameDelay` from `src/engine/FrameDelay.h`, same line you already need for the dry path below) instead of skipping outright. When `LatencySamples() == 0`, bypass stays a plain skip with no delay line and no cost — that is every built-in effect today.
   - Wet/dry: when `mix < 1`, copy the buffer to a per-bus scratch (preallocated in `Load`, sized to buffer frames), run the effect, blend `out = dry*(1-mix) + wet*mix`. For effects with nonzero `LatencySamples()` the dry path must be delayed by the same amount before blending (reuse `FrameDelay` from `src/engine/FrameDelay.h`) or the blend comb-filters — implement that, and add the delay only when latency > 0.
   - `SyncFx` structural match ignores `bypassed`/`mix` (they are pushable, like params) and pushes them atomically.
5. **Exporter** (`src/engine/Exporter.cpp` — this one IS kit-free and host-testable): mirror bypass + wet/dry (+ dry-path latency delay) exactly in `applyFx`.
6. **Commands** (`src/model/Commands.h/.cpp`): bypass/mix flow through the existing `SetFxCommand` chain-replace (verify). Add a small `SetFxBypassCommand(track, fxIndex, bypassed)` with a proper undo name so toggles read well in the Edit menu; no coalescing.
7. **UI minimal touch** (`src/ui/EffectsWindow.cpp` — Haiku-only): do NOT build the slot UI (separate agent). Only ensure the snapshot/apply path (`kMsgApplyFx`) round-trips the new fields so nothing strips them.

## Definition of done

- All existing ctest targets pass on the Linux host; new tests added and passing:
  - `EffectDesc` bypass/mix round-trip through `ProjectIO` (track and master), including patched-file compat both directions (old file → defaults; file with `fxin` → old-style load skips it) via the `LoadPatched` pattern.
  - `MakeEffect` with `EffectType::Lv2` and no factory registered → same fallback as unknown native plugin; with a stub factory registered → stub instance returned.
  - Exporter-level test: a bypassed **zero-latency** effect renders bit-identical to the chain without it; `mix = 0.0` likewise (zero-latency effect); `mix = 0.5` equals the hand-computed average.
  - Exporter-level test, latency cases — assert against a manually delayed dry reference, never against the undelayed one: a bypassed `LookaheadLimiter` renders bit-identical to the dry signal delayed by its `LatencySamples()` (NOT to the undelayed dry signal — that assertion would pass only with the flam bug present, so writing it the lazy way defeats the test); a `LookaheadLimiter` at `mix = 0.5` stays time-aligned with no comb offset.
- `git diff --stat` in the PR description, plus a note listing every behavior decision you made that wasn't pinned above.
