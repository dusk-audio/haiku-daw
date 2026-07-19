# Task: LV2 plugin hosting (lilv) behind IEffect

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch `feature/lv2-host` off master, **after** `feature/fx-inserts-core` has merged — it defines the contract you implement against: `EffectType::Lv2 = 10` (URI in `EffectDesc.pluginName`) and the `SetLv2Factory` hook in `src/dsp/EffectFactory.h`.

## Codebase orientation (read before coding)

- Layering: kit-free static lib `daw_model` (model + DSP + Exporter) builds on any host and carries the ctest suite; GUI and realtime engine build only under `if(HAIKU)` in `CMakeLists.txt`. Build/test loop on this Linux host: `cmake -S . -B build-host && cmake --build build-host -j && ctest --test-dir build-host`.
- RT rules (`src/engine/Engine.h:8`): audio callback reads only rings and atomics — no allocation, locks, or I/O. Anything an LV2 plugin needs allocated must happen at instantiation/`Prepare`, off-RT.
- Effect interface to implement: `daw::IEffect` (`src/dsp/IEffect.h`) — `Prepare(double sampleRate)` off-RT; `Process(float* stereo, int frames)` RT, in-place interleaved stereo; `SetParam(int slot, float value)` RT-safe; `LatencySamples()` feeds plugin-delay compensation; `Name()`.
- Existing native plugin host to mirror API-wise: `src/plugin/PluginHost.h/.cpp` (Haiku `load_add_on` ABI, `ScanDir`, dedupe by name, param-info query, installs its factory via `SetPluginFactory` — see wiring in `src/main.cpp:99-114`). The UI queries plugin param names/ranges from the host, since `FxParamRange` (`src/model/Effect.h:121`) only covers built-ins.
- Effects are described in the model as `EffectDesc {type, vector<float> params, pluginName}` (`src/model/Effect.h:33`); params are a flat float vector serialized automatically — LV2 control-port values live there, slot index = your stable port ordering.

## Goal

Host LV2 audio-effect plugins as inserts. v1 scope, deliberately bounded:

- Audio ports: stereo in/out. Accept plugins with 2 audio ins + 2 audio outs; also accept mono in/mono out by running one instance per channel. Reject anything else (report as unsupported, skip in listings).
- Control ports: input control ports become param slots (stable order = port index order). Read min/max/default via lilv. Output control ports: only the latency port is consumed, wired to `LatencySamples()`.
  - These are two DIFFERENT RDF mechanisms and must not be conflated. `lv2:latency` is a **designation** (`lv2:designation lv2:latency` on an output control port) — this is the current, correct one, and the one to find the port by. `lv2:reportsLatency` is an older **port property** (`lv2:portProperty lv2:reportsLatency`), deprecated in favour of the designation. Query them separately: match the designation first, and only fall back to the port property for older plugins if you choose to support them (state which way you went in the PR).
  - `LatencySamples()` must be constant across a `Prepare()`/`Process()` lifetime (`src/dsp/IEffect.h:54`) — the graph reads it once after `Prepare()`. So latch the port's value at `Prepare`/instantiation time; do NOT let a plugin that varies its latency port at runtime move the value under a graph that has already been solved.
- Required features: instantiate with no features (or the trivial ones you choose to provide, e.g. `urid:map`); skip any plugin whose `lilv_plugin_get_required_features` you don't satisfy. No worker extension, no state extension, no custom UIs in v1 (the generic slider UI is a separate agent's task).

## Work items

1. **Build system** (`CMakeLists.txt`): option `DAW_LV2` (default ON when lilv is found). Detect via `pkg_check_modules(LILV lilv-0)`. Lilv is available on Haiku via HaikuPorts and on this Linux host via the distro — the host code itself should be kit-free so it builds and tests on Linux too. When lilv is absent, everything compiles with LV2 support stubbed out (factory never registered).
2. **`src/plugin/Lv2Host.h/.cpp`** (kit-free apart from lilv):
   - `Lv2Host::Instance()` or an owned instance in main — match how `PluginHost` is owned and wired; scan once at startup (`lilv_world_load_all`).
   - Listing API shaped like `PluginHost`'s: name, URI, param count, param info (name, min, max, default) — so the UI agent can treat both hosts uniformly.
   - `class Lv2Effect : public daw::IEffect`: owns the `LilvInstance`(s), pre-allocated deinterleave/interleave scratch buffers and control-port value storage; `Process` deinterleaves, `lilv_instance_run`, reinterleaves; `SetParam` writes the control-port float (plain float write is acceptable — single writer, and LV2 control ports are read per run()); `Prepare` (re)instantiates at the new sample rate; `Reset` calls deactivate/activate.
   - Factory function registered through `SetLv2Factory`: looks up by URI from `EffectDesc.pluginName`, instantiates, applies `EffectDesc.params` to control ports (missing slots → port defaults), returns null for unknown URI / unsupported plugin (caller already falls back gracefully — verify against the merged 01 behavior).
3. **Wiring** (`src/main.cpp`): register the factory next to the existing `SetPluginFactory` call, guarded by the CMake flag.
4. **Persistence sanity**: saving an `EffectDesc{Lv2, params, uri}` and loading on a machine without that plugin must degrade the same way a missing native plugin does today — chain slot preserved in the model, instantiation fails soft. Confirm and test at the factory level.

## RT-safety notes (do not skip)

- `lilv_instance_run` on the audio thread is standard practice; everything lilv-world-related (scan, node creation) is NOT RT-safe — instantiation and lookup happen only in the factory/`Prepare`, never in `Process`.
- No allocation in `Process`: size scratch buffers in `Prepare` to an explicit maximum block size. `IEffect::Prepare(double sampleRate)` takes ONLY the sample rate (`src/dsp/IEffect.h:23`) — there is no block-size argument to read, so "size to whatever the caller will pass" is not implementable as written. Instead:
  - Define one shared compile-time constant (e.g. `kMaxLv2BlockFrames`) that both callers are known to respect, and size every scratch buffer to it in `Prepare`. It must be `>=` both the exporter's 8192-frame blocks (`applyFx`) and any live device buffer the engine can be configured for — check `Engine::SetBufferFrames`'s accepted range and pick a bound that covers it, rather than assuming the current default.
  - `Process` must then defensively handle `frames > kMaxLv2BlockFrames` WITHOUT allocating: either process in chunks of at most the max in a loop (preferred — correct for any caller), or bail out leaving the buffer untouched. Never grow a scratch buffer inside `Process`; that is the RT rule this whole section exists to protect.
  - State the constant and which behavior you chose in the PR, and unit-test the over-max path so the guard is proven rather than assumed.

## Definition of done

- Builds warning-clean on Linux with and without lilv installed (`-DDAW_LV2=OFF` path too).
- New ctest target `lv2_host_tests` (guarded: compiled only when lilv found): scans the system bundles; if a known-simple plugin is present (check for one of the `http://lv2plug.in/plugins/eg-amp` example or anything from `lv2-examples`/`mda-lv2` commonly installed), instantiate it, push a param, process a block of a known signal, assert output changed plausibly and no NaNs; assert unsupported-port-topology plugins are filtered out of listings. If no plugin is found at runtime, the test prints a skip notice and passes (CI machines vary).
- Port-mapping logic (port classification, mono-pair handling decision, param slot ordering) factored so its pure parts are unit-tested without lilv where feasible.
- PR description: list of supported/unsupported plugin classes, features provided at instantiation, and any deviation from the pinned contract.
