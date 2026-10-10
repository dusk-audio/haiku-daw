# feat(fx): external sidechain keys — Compressor and Gate

Package 05, first half of T7 (`24-continue-1.0-ENTRY.md` §5). Branch
`feature/sidechain` off `master` `6d88063`.

The spec (`05-sidechain.md`) is the definition of done; its PR-notes section
asks for four things, so they are first: the **tap point**, the **cycle
policy**, the **PDC alignment guarantee**, and the **appended param-slot
indices**. "What is NOT verified" is the honest list, and "Remaining in M7.1"
says what the plan's M7.1 wants and this branch does not do.

## The four notes the spec asks for

**Tap point — post-fader, post-FX, exactly where a post-fader aux send taps.**
Both hosts copy the source node's output buffer at the point the post-fader
sends are added (Exporter: after `applyFx` + `addSends(post)`; engine: the same
buffer the `sendTargets` read at that spot). Consequences, both pinned by tests:

- the key is the source's **fader output**, so the key track's fader scales it
  (a fader pulled 40 dB moves the key below the threshold and the duck
  disappears);
- **mute silences the key**. A muted key track produces no output, so a keyed
  Compressor does NOT reduce (the detector reads silence: -100 dB, below any
  threshold) and a keyed Gate closes. It is "silence", not "no key": the
  difference matters, because the fail-soft rule for a *missing* source is
  internal detection. Engine and Exporter agree because the engine zeroes every
  node buffer per block and skips the same nodes the Exporter skips;
- solo-excluding the key track does the same (a soloed consumer's key is
  silence).

**Cycle policy — all-or-nothing, matching the send edges' precedent.** The key
edges join the SAME edge set as the output edges and the aux-send edges, and
feed both `ResolveOrderWithEdges` (topo order) and `ComputePdc` (latency solve).
The key only becomes audibly a key when both solved:

- a cycle in that edge set (A keys B keys A; or A keys B while B routes into
  A's bus) fails the solve, the order falls back to flat, and `ok`/
  `sidechainSolvable` stay false, so **no key is routed anywhere** and every
  keyed insert detects internally. `test_key_cycle_fails_soft` asserts the
  render is bit-identical to the same project with keying off;
- a source that names a track the project does not have is unrouted the same
  way (fail soft, never a failed render, never silence);
- a **self-key** (`sidechainSource == the insert's own track`) is unrouted by
  contract — no order can read a node's own output before that node runs — and
  no edge is added for it, so it cannot manufacture a cycle. The editor's
  picker does not offer it.

Rationale for the all-or-nothing choice: partial routing would mean an effect
whose detector depends on track creation order, which is not something a user
can reason about. This is also why a *cycle* is the ONLY condition that makes a
routed key vanish silently.

**PDC alignment guarantee — the key is delay-aligned to the consumer's input,
frame for frame, by the same machinery a send uses.** `EdgeDelay(source,
consumer)` is applied to the key (`FrameDelay` per insert in the engine; a
pre-shifted key buffer in the Exporter — both give exactly N frames of delay).
For that number to be meaningful the consumer's OWN material has to be placed
at its `InLat`, because a keyed node is the first node that has both its own
material and an input: the engine delays it with `Bus::inDelay`, the Exporter
places clips/notes at that offset. `InLat == 0` for every ordinary track, so
nothing changes for projects that use no key, and a keyed insert never changes
the MIX's latency (the key source's latency already reached the master through
its own routing, so `masterInLat` does not move).

The guarantee, as tested: a `LookaheadLimiter` (240 frames at 48 k) upstream on
the key path leaves the whole bounce **bit-identical** to the same project
without it (`test_key_pdc_alignment`), including the ducking onset frame.
`test_key_edge_delay` covers the other direction — a keyed insert on a BUS with
a latent feeder, where `EdgeDelay` is 240 and the key must actually move — and
the same bit-identity holds.

**Appended param-slot indices.** One slot each, appended after the effect's own
params, so every older project's stored slots keep their meaning (an older file
simply has no slot 5 = `p()` reads 0 = internal detection):

| Effect | Slots | Appended |
| --- | --- | --- |
| Compressor | 0 thr dB, 1 ratio, 2 attack ms, 3 release ms, 4 makeup dB | **5 = `extKey` 0/1** |
| Gate | 0 thr dB, 1 ratio, 2 attack ms, 3 release ms, 4 range dB | **5 = `extKey` 0/1** |

`FxParamRange` gained the matching `{0,1}` entry for both, the layout comment in
`model/Effect.h` names slot 5, and the constant the effects and the editor share
is `kExtKeySlot` in `src/dsp/SidechainKey.h`. The toggle is a **detector**
switch: with `extKey` on, the gain computer still acts on the insert's own
signal; only what the detector listens to changes (the key's mono sum, halved).

Design decision worth naming: the key is **mono-summed and halved** as the spec
says, so a centre-panned key at the same amplitude reads exactly what the same
track's internal detector would read (`max(|L|,|R|)` of a centre-panned signal
== `0.5*(L+R)`). An off-centre key therefore reads below its own level — a
hard-panned kick keys at 6 dB less — and the test fixtures are built on that
number. `SidechainKey.h` and the test file both state it; if it should be
pan-invariant instead, the change is one expression.

## What changed

| File | What |
| --- | --- |
| `src/model/Effect.h` | `EffectDesc::sidechainSource` (a `TrackId`, declared last so existing brace-inits still compile), `EffectSupportsSidechain()` (the ONE list of keyed types), the slot-5 layout comments, `FxParamRange` entries. |
| `src/dsp/SidechainKey.h` (new) | `kExtKeySlot`, the per-block key holder (`Set`/`Active`), and `DetectorLevel()` — the ONE detector reduction both paths run. |
| `src/dsp/IEffect.h` | `SetSidechain(const float*, int)` with the full contract (one block, RT, fail soft when absent). Default no-op. |
| `src/dsp/Compressor.{h,cpp}`, `Gate.{h,cpp}` | `extKey` slot; `SetSidechain`; Process detects on the key when one is present, still applies gain to its own signal; consumes the key each block; `Reset()` clears it; the detector is refactored through `DetectorLevel` (no duplicated detection code). |
| `src/dsp/EffectFactory.cpp` | Applies the appended slot for Compressor/Gate, the same way automation and the editor do. |
| `src/model/ProjectIO.cpp` | Optional `fxsc <fxIndex> <trackId>` / `masterfxsc` lines (written only when set), parsed with the malformed-record policy `fxin` uses. |
| `src/engine/Engine.{h,cpp}` | Key edges in the topo+PDC edge set; a node synthesised for a key source that has none; per-insert `fxKeySrc`/`fxKeyNode`/`fxKeyDelay`; `Bus::inDelay`; the per-block key staging + `SetSidechain` before each slot; master-chain keys; `SyncFx` treats a source change as structural. |
| `src/engine/Exporter.cpp` | The same edges and the same alignment (`nodeIn` for own material, pre-shifted per-insert key buffers filled at the source's turn), plus `SetSidechain` before each `RunInsertSlot`. |
| `src/ui/EffectsWindow.{h,cpp}` | A "Key:" source row on Compressor/Gate panels (a `BPopUpMenu` of "None" + track names) that sets `sidechainSource` and the `extKey` slot, committed through the normal `kMsgApplyFx` snapshot; `EncodeFxChain`/`DecodeFxChain` carry the source (int64 "es"). |
| `src/ui/MainWindow.cpp` | `SidechainCandidates()` and the three `EffectsWindow` call sites. |
| `tests/sidechain_tests.cpp` (new) | 90 checks — the DoD's four cases plus the cycle case, the keyed bus, the pre-feature descriptor, and block-size invariance. |
| `tests/ui_functional_tests.cpp` | `test_sidechain_picker`: the picker's commit path through `kMsgApplyFx` (source in, source cleared), and a shot of the panel. |
| `scripts/haiku_syntax_check.sh` | Adds `os/add-ons/*/` to the include path. Pre-existing: `EffectsWindow.cpp` includes `<Screen.h>`, which includes `<Accelerant.h>`, which lives two levels below the `os/*/` glob the script added — so the ONE file this package touches was also the one file the check could never pass. One line; no product behaviour. |

Also: `CMakeLists.txt` registers `sidechain_tests` right after `exporter_pdc_tests`
(one contiguous hunk), and the three unmerged sibling branches touch
`Effect.h`/`ProjectIO`/`Engine.{h,cpp}`/`EffectsWindow.cpp` too, so the
`ProjectIO` additions are append-only one-liners and the CMake change is a
single block.

## What is verified, and how

```
cmake --build build-host                       # exit 0
ctest --test-dir build-host                    # 53/53  (52 on master + sidechain_tests)
./build-host/sidechain_tests                   # 90 checks, 0 failures
cmake -B b-asan -DDAW_SANITIZE=ON && cmake --build b-asan -j8 && ctest --test-dir b-asan  # 53/53, clean
sh scripts/haiku_syntax_check.sh               # 10 OK, 0 FAIL
# On the VM (2 vCPU, -j2), configure and build exit 0:
ctest --test-dir build                         # <fill in>  (LV2 on)
ctest --test-dir build-off                     # <fill in>  (-DDAW_LV2=OFF)
DAW_UI_SHOTS=/tmp/shots ./ui_functional_tests  # shots reviewed: <list>
```

**Mutation testing** — every mutation was applied, the suite run, and the code
restored. "checks" is how many failed while mutated:

| Mutation | Result |
| --- | --- |
| Compressor ignores the key (`keyed = false`) | caught (13) |
| Compressor always detects its own input (`det = stereo`) | caught (13) |
| Gate ignores the key (`keyed = false`) | caught (3) |
| key reduced with the internal rule (`DetectorLevel` mono-sum branch removed) | caught (4) |
| key edge delay forced to 0 | caught (2) |
| key alignment shift direction flipped (`buf[q+edge] = nb[q]` -> `buf[q] = nb[q+edge]`) | caught (2) |
| consumer's own material not delayed to its input latency (`nodeIn = 0`) | caught (1) |
| key tapped pre-fader (the fill loop moved above the fader) | caught (7) |
| keys routed even when the graph fails to solve (`keysSolvable = true`) | caught (1) |
| `fxsc` line never written | caught (4) |
| `fxsc` line parsed but dropped | caught (3) |
| factory ignores the appended `extKey` slot | caught (12) |

Two mutations initially SURVIVED and are worth recording, because they found
real gaps in the first cut of the tests: "key edge delay forced to 0" and the
sign flip both survive when every test scenario has `EdgeDelay == 0` (the key
source is the slowest path, so nothing has to move). `test_key_edge_delay` —
the keyed bus with a latent feeder — was added for exactly that, and both
mutations then fail. Both were re-run after the change.

**Live-vs-bounce sample identity.** The engine's own callback cannot be
host-tested, so what is proven here is the part of the promise that can be:
`test_block_invariance` runs each keyed effect in one call, in 512-frame blocks
(the engine's size) and in 8192-frame blocks (the Exporter's), handing the key
over per block exactly as `IEffect::SetSidechain` specifies, and asserts the
outputs are bit-identical — plus that a key supplied for one block does not
leak into later blocks. Both hosts then feed the effect through the same
`RunInsertSlot` (already covered by `insertslot_tests`). What remains a code
reading, not a measurement, is the engine's own block loop (see below).

## What is NOT verified

- **The RT engine's key path, live.** `Engine.cpp` is Haiku-only: it compiles
  under the cross-compiler and in the VM build, but no test drives
  `FillBuffer` with a key. The engine's alignment maths mirrors the Exporter's
  (same `EdgeDelay`, same `InLat` for own material) and both were written
  against the same derivation, but the live/bounce identity for a keyed insert
  is a structural argument plus the block-invariance test above, not a
  measured comparison. A VM click test is in M4.1's coverage gap, not this
  package's.
- **The engine-only behaviours** — `SyncFx` treating a source change as
  structural, the synthesised node for a key source with no material, the
  engine's cycle gate — have no host test (Haiku-only), so they carry no
  mutation check. They are named here rather than implied.
- **The picker's menu itself.** A `BPopUpMenu` is modal, so the functional test
  drives what the picker POSTS (`kMsgApplyFx`), not the click that chooses an
  item; the shot shows the row, its label and its dot.
- **Audible result**: nothing is audible on the VM. The ducking is measured
  (numbers, not ears).

## Remaining in M7.1 (not this branch)

`PLAN_1.0.md` M7.1 is package 05 "**then** extend it to LV2 sidechain ports …
and to a sidechain source picker on the insert slot". This branch is the
package-05 half:

- **LV2 sidechain ports** (`Lv2PortMap.h:105`) are still connected to silence:
  `Lv2PortLayout::silencePorts` holds a plugin's optional audio inputs and
  `Lv2Host.cpp` points them at `fSilence`. Feeding them the key means
  overriding `SetSidechain` in `Lv2Effect` and connecting the first two
  silence ports to key buffers instead — the interface this package adds is
  what makes that a local change.
- **The insert-slot picker** (the channel strip's slot row, package 03's UI) is
  not touched; `03-inserts-ui.md` owns strip polish and this package's UI item
  was explicitly the `EffectsWindow` panel.

## Known, not fixed

- A key source with no material of its own gets a synthesised empty node in the
  engine (see the code comment) — otherwise the engine would treat the key as
  unrouted while the Exporter would route the track's silence, and the two
  hosts would disagree about a keyed Compressor's detector.
- The key tap reads the source's post-fader output *as rendered for this
  render window*: a range export's first `EdgeDelay` frames of key are silence,
  because nothing before `winStart` is rendered offline. A whole-project bounce
  (the default) has no such window.
- Sweeping the key's own level (an automation lane on the key track's fader) is
  applied per block in the engine and per sample in the Exporter, the same
  asymmetry every other fader-driven path has.
