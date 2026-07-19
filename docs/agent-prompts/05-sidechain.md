# Task: Sidechain routing — external key for Compressor and Gate

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch `feature/sidechain` off master, **after** `feature/fx-inserts-core` has merged (it reshapes the same files: `Effect.h`, `EffectFactory`, engine FX loop, `applyFx`).

## Codebase orientation (read before coding)

- Layering: kit-free `daw_model` (model + DSP + Exporter) builds and tests on this Linux host (`cmake -S . -B build-host && cmake --build build-host -j && ctest --test-dir build-host`); `src/engine/Engine.cpp` (realtime engine) is Haiku-only — edit pattern-faithfully, no host compile. **`src/engine/Exporter.cpp` is kit-free and host-testable — it is your executable spec.** Live and bounced output must stay sample-identical.
- Engine graph: each track/bus is a `Bus` node (`src/engine/Engine.h:306`); `Engine::Load` (`Engine.cpp:211`) builds nodes, resolves routing + aux-send edges to node indices (`:397-415`), computes a topological order `fOrder` over output + send edges (`ResolveOrderWithEdges`, `:420-442`), and sizes plugin-delay-compensation (PDC) delay lines per edge (`ComputePdc`, `:451-480`, using `src/model/Pdc.h` + `src/engine/FrameDelay.h`). Per block, `FillBuffer` (`Engine.cpp:794`) walks `fOrder`: sources → fader → FX chain (`:952`) → meters → aux sends via `FrameDelay::ProcessAdd` (`:976`) → route to output bus/master.
- The Exporter mirrors all of it offline: same topo order (`Exporter.cpp:179-192`), same PDC solve with pad/trim (`:193-223, 481-485`), FX in 8192-frame blocks (`applyFx`, `:311-337`), sends pre/post-fader (`:284-304`).
- Effects: `daw::IEffect` (`src/dsp/IEffect.h`) — `Process(float* stereo, int frames)` in-place, `SetParam` RT-safe, `LatencySamples()` for PDC. `Compressor` (`src/dsp/Compressor.h`) and `Gate` (`src/dsp/Gate.h`) are the sidechain consumers. Param layouts are flat float vectors documented at `src/model/Effect.h:37-47`; `FxParamRange` (`Effect.h:121`) maps slots to editor ranges.
- Model: `EffectDesc` (`src/model/Effect.h:33`) per insert; `Track.sends` (`src/model/Project.h:99`) shows how cross-track references are modeled (`TrackId dest`). Serialization is append-only line-based text (`src/model/ProjectIO.cpp`) — new data = new optional lines; see the `fxin` line added by the fx-inserts-core package and the `LoadPatched` compat-test pattern in `tests/instrument_io_tests.cpp`.
- Commands funnel: UI → `Command` (`src/model/Commands.h`) → `Project`; `SetFxCommand` replaces a whole chain. RT rules (`Engine.h:8`): callback reads only rings/atomics, no allocation/locks/I-O.

## Goal

Any insert can take an external sidechain key from another track: the classic kick-ducks-bass compressor and keyed gate. Design once at the `IEffect` level so future effects (ducking delay, vocoder) inherit it.

## Work items

1. **Model** (`src/model/Effect.h`): `EffectDesc` gains `TrackId sidechainSource = 0` (0 = none; include what's needed for `TrackId`, or use `uint64_t` to keep Effect.h free of model includes — match existing include discipline). Serialization: new optional line `fxsc <fxIndex> <trackId>` per track (and `masterfxsc` if you allow master-chain sidechains — allow it; the key is still a track). Compat tests both directions via the `LoadPatched` pattern.
2. **IEffect extension** (`src/dsp/IEffect.h`): add `virtual void SetSidechain(const float* stereo, int frames) {}` — called by the engine immediately before `Process` on the same block when a key is routed; default no-op keeps every existing effect source-compatible. Document the contract: pointer valid only during the call, RT context, may be null/never-called when unrouted.
3. **DSP** (`src/dsp/Compressor.cpp/h`, `src/dsp/Gate.cpp/h`): append one param slot each — `extKey` (0/1) — to the flat param layout (append-only, update the layout comment in `Effect.h:37-47` and `FxParamRange`). When `extKey` is on and a sidechain block was provided, the detector runs on the key signal (mono-sum it) while gain is applied to the main signal; when on but no key routed, behave as internal (fail soft). Keep the detector code shared between the two paths — refactor, don't duplicate.
4. **Engine** (`src/engine/Engine.cpp/h`, Haiku-only):
   - `Load`: resolve `sidechainSource` TrackId → node index per insert; add these as edges into `ResolveOrderWithEdges` so the key track renders before the consumer (document what you do on cycles — the existing send-edge cycle policy is the precedent; find it and match it).
   - PDC: the key signal must be time-aligned with the consumer's input; run the key through the same per-edge `FrameDelay` machinery as sends (`ComputePdc`). State explicitly in a comment what alignment you achieve.
   - `FillBuffer`: before the consumer's `Process`, call `SetSidechain` with the resolved source node buffer (post-fader tap — same tap point as a post-fader send). Preallocate any staging buffer in `Load`.
   - `SyncFx` structural match must treat a sidechain-source change as structural (edges change → rebuild), not pushable.
5. **Exporter** (`src/engine/Exporter.cpp`, kit-free): mirror everything — edge into the topo order, PDC alignment, per-block `SetSidechain` before `Process` inside `applyFx`'s caller. This is where your correctness is actually proven by tests.
6. **UI minimal** (`src/ui/EffectsWindow.cpp`, Haiku-only): on Compressor/Gate panels, a small source picker (menu of track names + "None") that sets `sidechainSource` and toggles `extKey`, committed through the normal `kMsgApplyFx` snapshot path. Nothing fancier; the inserts-ui package owns strip polish.

## Definition of done

- All existing ctests pass; new `sidechain_tests` (exporter-level, kit-free) covering: kick-on-track-A keying compressor-on-track-B ducks B exactly when A plays (assert gain envelope timing against a hand-built expectation); `extKey` on with no source behaves as internal detection; key track muted/soloed semantics (decide and document: key taps post-fader, so mute silences the key — state it in the test); PDC alignment — put a `LookaheadLimiter` (the only latency effect) upstream on the key path and assert the ducking onset does not shift relative to the no-latency render; ProjectIO round-trip + patched-file compat for `fxsc`.
- PR description: cycle policy, tap point, PDC alignment guarantee, and the appended param-slot indices for Compressor/Gate.
