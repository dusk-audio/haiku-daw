# Task: LV2 sidechain ports — feed the key the host already routes (M7.1 tail)

Item M7.1 of `docs/PLAN_1.0.md`: "Sidechain (package 05): build exactly as
specified in `docs/agent-prompts/05-sidechain.md`. **Then extend it to LV2
sidechain ports, which are currently left unconnected (`Lv2PortMap.h:105`)**, and
to a sidechain source picker on the insert slot."

Package 05 built the whole mechanism — the model's `EffectDesc::sidechainSource`
and its `fxsc` line, the routing edges, the PDC alignment, the key tap in both
hosts, the `IEffect::SetSidechain` contract — for the two built-in dynamics
effects. This package makes an LV2 plugin a consumer of it.

## Where the key goes

`Lv2PortMap.h` already classifies a plugin's optional audio ports:
`lv2:connectionOptional` audio inputs do not count toward the topology (that is
what makes Multi-Comp, 4-in / 2-out, hostable as a stereo insert), and v1
connected them to a **silent buffer** — deliberately: the spec allows NULL, and
JUCE plugins memmove every audio port they declare without checking (the crash
that produced the rule, `02-lv2-host-PR.md`). The comment there says the
silence buffer is "exactly what the sidechain package will later replace with a
real signal". This is that replacement.

The mapping is by convention, because LV2 has no sidechain designation of its
own:

- the first two optional audio **inputs** (in port order) are the plugin's
  stereo KEY pair — the layout gains `sidechainPorts` for them;
- any further optional input stays on the silence buffer (`silencePorts`);
- **MonoDual** (1 in / 1 out, two instances): instance k connects its optional
  input to channel k's key, which is the mono channel's own key.

They are connected to a host-side `fKey[2]` pair ONCE, at instantiate time, and
the buffer contents — not the connection — change per block. Nothing calls
`connect_port` from the audio thread.

## What the host does per block

`Lv2Effect::SetSidechain` stores the pointer for the call that follows, exactly
as `IEffect` documents (valid only until the next `Process` returns, may be
null, never retained), and `Process` deinterleaves this chunk's key into
`fKey[0]/fKey[1]` — or zeroes them when no key is routed, which is what "nothing
is plugged into the sidechain" means and what makes an unkeyed insert fail soft
rather than hold a stale key. Both are arithmetic over preallocated buffers: no
allocation, no locks, no plugin calls.

## Deciding who gets a key

The hosts route a key only for inserts whose effect CONSUMES one —
`EffectSupportsSidechain(type)` today, which is Compressor/Gate. For an LV2
insert the answer is a property of the installed plugin (does its topology have
optional inputs?), which the kit-free model and the kit-free Exporter cannot
know: `daw_model` never links lilv. So the answer arrives the way the factory
does — a hook:

```cpp
// EffectFactory.h
using Lv2SidechainProbeFn = bool (*)(const std::string& uri);
void SetLv2SidechainProbe(Lv2SidechainProbeFn fn);
bool EffectTakesSidechain(const EffectDesc& desc);   // built-ins + LV2
```

`Lv2Host::ScanAll` installs it beside `SetLv2Factory`, and
`Lv2PluginInfo` gains `sidechain` (its layout has a key pair). A null probe —
LV2 not compiled in, or never scanned — answers "no", the same degradation as an
unavailable plugin type: no picker entry, no key edge, no extra PDC.

The five call sites switch from `EffectSupportsSidechain(d.type)` to
`EffectTakesSidechain(d)`: the engine's key edges and its master-chain keys, the
exporter's key edges and key routes, and the effects editor's three
picker decisions (so the "Key:" row appears on an LV2 insert whose plugin has
a sidechain, and stays away from one that does not).

## Work items

1. `Lv2PortMap.h`: `sidechainPorts`; `BuildLv2PortLayout` splits optional audio
   inputs between it and `silencePorts`; comments at the two places that say
   "silence, for now" updated.
2. `Lv2Host.{h,cpp}`: the `fKey` buffers + `SetSidechain` + per-chunk fill;
   `ConnectPorts` wiring; `Lv2PluginInfo.sidechain`; the probe trampoline.
3. `EffectFactory.{h,cpp}`: the probe hook and `EffectTakesSidechain`.
4. Engine / Exporter / EffectsWindow call sites (above).
5. Fixture: `urn:haiku-daw:test:sidechain-gain` — 2 required in / 2 out, 2
   optional KEY inputs, one gain port, and DSP `out = in * key * gain`. Its
   output is exactly computable, so a test asserts VALUES: with a key routed
   the bounce is `dry * dry` sample-exactly (the key is the same material), with
   none it is silence.
6. Tests:
   - `lv2_portmap_tests`: the key pair in port order, extras on the silence
     buffer, and a MonoDual plugin with one optional input;
   - `lv2_render_tests`: the keyed bounce above, silence with no source, and an
     LV2 insert with NO sidechain ports naming a source (must behave exactly
     like one that names none — the probe is what stops a bogus key edge from
     moving the PDC);
   - `lv2_fixture_tests`: the plugin is hostable and reports `sidechain`;
   - `sidechain_tests`: `EffectTakesSidechain` with and without a probe
     installed — kit-free, so this part is proven on any machine.

## Definition of done

- Host suite green; `scripts/haiku_syntax_check.sh` reports no new FAIL.
- Mutations named in the record: key buffers not zeroed (silence case fails),
  key pointer ignored in Process (the keyed bounce fails), the probe always
  false (the picker/routing tests fail), the probe always true (the
  no-sidechain-ports case fails).
- RT review stated: the per-block work is a deinterleave into preallocated
  buffers (or a memset); no `connect_port`, no allocation, no locks.
- UI note: the picker's "Key:" row now appears for LV2 inserts with a
  sidechain. No drawing change; the VM's `ui_functional_tests` and a screenshot
  pass are still the orchestrator's, per AGENTS.md.
