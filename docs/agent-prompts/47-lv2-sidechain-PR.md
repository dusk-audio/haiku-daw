# feat(plugin): feed the LV2 sidechain the engine's key (M7.1 tail)

Spec: `47-lv2-sidechain.md`.

```
c201c5b feat(plugin): feed the LV2 sidechain the engine's key (M7.1 tail)
aaac01f docs: correct what the sidechain gate buys (M7.1 tail)
fee2e5c docs: spec the LV2 sidechain ports (M7.1 tail)
```

## What was built

`Lv2PortMap`'s comment ("silence, which is exactly what the sidechain package
will later replace with a real signal") is now the real thing: the first two
`connectionOptional` audio INPUTS of a plugin are its key pair
(`Lv2PortLayout::sidechainPorts`), further optional inputs keep the silence
buffer, and a MonoDual plugin's optional input is its own channel's key.

`Lv2Effect` connects the pair ONCE, at instantiate, to `fKey[0]/fKey[1]`;
`SetSidechain` stores the block's pointer for the call that follows and
`Process` deinterleaves it (or zeroes the buffers when no key is routed) —
never retaining a pointer, never reconnecting a port from the audio thread.

Who takes a key is a property of the PLUGIN, so `EffectFactory` gains a probe
hook beside its factory hook: `EffectTakesSidechain(desc)` answers from the
model for the built-ins and from `Lv2PluginInfo.sidechain` for an LV2 insert.
The engine's key edges, the exporter's key edges and key routes, and the
editor's three picker decisions all read that one predicate.

## What the gate actually buys (corrected in aaac01f)

The spec first claimed a bogus key edge on a portless plugin would shift the
mix. Measured, it does not: PDC compensates latency transparently, so an extra
key edge is invisible in a bounce. The gate's observable value is the editor
not offering a key for a plugin that has none, plus the edges, key buffers and
topo/cycle constraints it does not create. The render assertion stays as the
honest form of that claim; the predicate assertions are its teeth.

## Tests and mutations

`tests/lv2fixture/` gains `sidechain-gain` (2 required in/out, 2 optional key
inputs, `out = in * key * gain`) so the key is measurable: with a source routed
the consumer's keyed output enters the mix, without one it contributes exactly
silence (the buffers are connected and zeroed — not NULL, not a stale key), and
a self-key changes nothing. `lv2_portmap_tests` (+62 checks) covers the pair
split, extra optional inputs and the mono case; `sidechain_tests` (+8) covers
`EffectTakesSidechain` with and without a probe installed, kit-free.

| Mutation | Caught by |
| --- | --- |
| `Process` never fills the key buffers | `lv2_render_tests` — the keyed bounce collapses onto the key track alone |
| the probe always answers "no" | `lv2_render_tests` (2 checks: the keyed bounce, the predicate) |
| the probe always answers "yes" | `lv2_render_tests` (2 predicate checks) |

## RT review (the item where this is easiest to break)

The per-block work is a deinterleave of the caller's block into two
preallocated buffers, or two memsets when no key is routed. No `connect_port`,
no allocation, no locks, no I/O; the key pointer is not retained past the
`Process` it was supplied for (the `IEffect::SetSidechain` contract).

## UI note

The effects editor's "Key:" row now appears for an LV2 insert whose plugin has
a sidechain pair (`EffectsWindow.cpp`, three call sites, plus the missing
`EffectFactory.h` include the syntax check caught). No drawing change; the VM's
`ui_functional_tests` and the screenshot pass are the orchestrator's, per
AGENTS.md.

## Verification

```
$ cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build build-host -j2
exit 0
$ ctest --test-dir build-host
100% tests passed out of 69
$ sh scripts/haiku_syntax_check.sh
75 OK, 4 FAIL (the pre-existing codec files). src/plugin/Lv2Host.cpp,
src/plugin/Lv2PortMap.h and src/ui/EffectsWindow.cpp all compile.

Suite counts after the package: sidechain_tests 106, lv2_portmap_tests 167,
lv2_fixture_tests 108, lv2_render_tests 29, exporter_pdc_tests 27,
sinc_resampler_tests 27.
```

Pre-existing, observed and NOT touched here: with the fixture bundle on
`LV2_PATH`, `lv2_host_tests` fails one check — `urn:haiku-daw:test:two-ui` is
declared in `manifest.ttl` but has never had a C++ descriptor in
`daw_test_plugins.cpp`, so "everything listed instantiates" fails. Under ctest
that test runs without `LV2_PATH` and is unaffected.
