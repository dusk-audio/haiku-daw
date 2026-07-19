# feat(lv2): host LV2 plugins as inserts behind IEffect

Package 02. Branch `feature/lv2-host` off `b7d95d3`:

```
efdb8e2 test(lv2): add a mandatory-CV-port fixture, closing the last unit-test-only reject path
e542b53 test(lv2): build a real LV2 bundle as a fixture, and assert on values
7c53349 test(lv2): report each hostable plugin's topology and whether the monoDual path ran
046e65d fix(lv2): find plugins on Haiku, where lilv's default search path finds none
52604e6 fix(lv2): make the control-array invariant real, and don't deactivate an unactivated instance
8d25f6c feat(lv2): host LV2 plugins as inserts behind IEffect
```

Scans installed LV2 bundles with lilv, filters them to what can actually be
hosted, and registers the `EffectType::Lv2` factory hook so an LV2 plugin sits in
a track or master insert chain like any built-in effect.

## Supported and unsupported plugin classes

**Hosted:**

| Shape | Handling |
| --- | --- |
| 2 required audio in / 2 required audio out | one instance |
| 1 required audio in / 1 required audio out | **two** instances, one per channel, sharing one control-value array |
| Extra audio ports marked `lv2:connectionOptional` | not counted toward topology; connected to silence (in) / scratch (out) |
| Atom ports | connected to an inert empty `LV2_Atom_Sequence`; never read or written |
| Input control ports | param slots, in ascending port-index order |
| Output control ports | ignored except the latency port |

**Declined, with the reason recorded in `Lv2Host::Rejected()`:**

| Reason | Real examples on the dev host |
| --- | --- |
| Requires a feature we do not implement | sfizz, sfizz-multi (`worker:schedule`) |
| Audio topology out of scope | DuskAmp (1 in / 2 out), Multi-Synth (0 in / 2 out), GrooveMind, Chord Analyzer Headless / MIDI, eg-fifths, eg-params, eg-metro |
| Required port of a class we cannot size | *(no example on either machine — unit-tested only)* |
| Malformed bundle | tape_machine_2 (TTL syntax error at line 388) |

Result on the Linux dev host: **9 hostable, 10 rejected** out of 19 plugins lilv finds.
On the Haiku VM: **4 hostable, 3 rejected** out of 7 (the LV2 example plugins).
Note `eg-amp` is *hosted* on Haiku (mono 1-in/1-out) — it is not installed on Linux.

## Features provided at instantiation

`urid:map`, `urid:unmap`, `options:options`, `bufsz:boundedBlockLength`.

The task doc suggested "no features (or the trivial ones you choose to provide,
e.g. `urid:map`)". Providing only `urid:map` would have rejected **every real
plugin on this machine** — all 15 in `~/.lv2` additionally require
`options:options`, and most require `bufsz:boundedBlockLength`. The options block
supplies `param:sampleRate`, `bufsz:minBlockLength`, `bufsz:maxBlockLength` and
`bufsz:sequenceSize`.

`boundedBlockLength` is on that list only because `Process` chunks. Without the
chunk loop it would be a claim we make at instantiation and then break the first
time someone configured a large device buffer.

## Deviations from the pinned contract

**None.** The raw-pointer `Lv2FactoryFn` was implemented as pinned:

```cpp
using Lv2FactoryFn = IEffect* (*)(const EffectDesc& desc, double sampleRate);
```

The handoff flagged it as still open and asked not to change it unilaterally. It
was not changed. It is confined to one trampoline line — `Lv2Host::Create` returns
`std::unique_ptr` like `PluginHost::Create`, and the trampoline `.release()`s it —
so if you do want it changed to match `PluginFactoryFn`, that is a one-line edit
here plus the signature.

`sampleRate <= 0` is guarded: it stands in 44100 and lets `Prepare` re-instantiate,
the same shape as `PluginTrampoline`. `tests/effect_tests.cpp` and
`lookahead_limiter_tests.cpp` still call `MakeEffect(desc)` with no rate.

## Two decisions the code would not have survived without

Both were found by running against real plugins, not by reading the spec.

**Optional audio ports are connected, not left NULL.** The LV2 spec allows a host
to leave a `connectionOptional` port unconnected, and the first version did.
Multi-Comp segfaulted immediately:

```
#1 juce::lv2_client::LV2PluginInstance::run(unsigned int) () from ~/.lv2/Multi-Comp.lv2/libMulti-Comp.so
#2 lilv_instance_run (instance=0x4f7620, sample_count=512)
#3 daw::Lv2Effect::Process (this=0x7ae5b0, stereo=0x8456f0, frames=512)
```

JUCE's LV2 wrapper `memmove`s every audio port it declares into its internal
layout without checking for NULL. Since JUCE-generated plugins are a large share
of the real world, "spec-legal" is not the bar. Unrouted inputs now get a silent
buffer and unrouted outputs a scratch buffer — equally legal, survivable, and the
right semantics for "nothing is plugged into the sidechain". This is also what
makes Multi-Comp (4 audio in / 2 out) hostable as a plain stereo insert, and it
leaves package 05 a place to connect a real signal.

**Plugins are filtered through `lilv_plugin_verify`.** `tape_machine_2` has a TTL
syntax error partway through its bundle, so lilv publishes the fragment it managed
to parse: a valid URI, a plausible 2-in/2-out port list, no `doap:name`. It passed
every topology and feature check and then failed to instantiate. The test
asserting "everything listed can be instantiated" caught it. `lilv_plugin_verify`
is lilv's own well-formedness check and separates exactly that plugin from the 18
sound ones.

## RT safety

- Every buffer is allocated in the constructor / `Prepare`, never in `Process`.
- `Process` deinterleaves, runs, reinterleaves, and resets atom headers — arithmetic
  and `memset` over preallocated state only.
- Everything lilv-world-related (scan, node creation, instantiate) happens in
  `ScanAll` / the factory / `Prepare`, off the RT thread.
- `SetParam` is a plain float store into the buffer the control port is already
  connected to (single writer; LV2 control ports are re-read every `run()`),
  clamped to the port's declared range and NaN-guarded.

### `kMaxLv2BlockFrames = 8192`, and `Process` chunks

The task doc said to check `Engine::SetBufferFrames`'s accepted range and pick a
covering constant. As the handoff corrected: that setter is `if (n >= 32)
fBufferFrames = n;` — **unbounded above**, so no constant can be proven sufficient.
Chunking is the only correct option, and it is what makes the
`bufsz:maxBlockLength` we advertise true by construction. 8192 matches the
Exporter's offline block, so an offline bounce never chunks.

The chunk loop is proven, not assumed — see the mutation test below.

### Latency

Read once after `activate`, from the output control port found by **`lv2:designation
lv2:latency` first**, falling back to the deprecated `lv2:portProperty
lv2:reportsLatency` only if no port carries the designation. The two are queried
separately, as the task required. Every plugin installed here declares both, on the
same port.

`Reset()` deliberately does **not** re-read it. The engine calls `Reset` mid-playback
on a seek, and both the PDC solve and `RunInsertSlot`'s per-insert dry-delay line
have already been sized from the latched value.

## Build system

`option(DAW_LV2 ... ON)`, detected via `pkg_check_modules(LILV lilv-0)`. When lilv
is missing the option auto-disables with a status message. `daw_lv2` is its own
target so **`daw_model` never links lilv** and the kit-free model layer stays
dependency-free. `DAW_HAVE_LV2` is `PUBLIC` on that target, which is what gates the
registration in `main.cpp` and will gate the UI's listing query in package 03.

With LV2 off, `MakeEffect` yields `nullptr` for an `Lv2` descriptor and the chain
keeps an index-aligned hole — the same degradation as an unavailable native add-on,
already covered by package 01's tests.

## Tests

`lv2_portmap_tests` (**127 checks**) — built and run **unconditionally**, including
where no LV2 exists, because that is the machine where a regression here would
otherwise never run. Covers topology classification, optional-port handling,
required-unknown rejection, latency designation-over-property precedence, param
slot ordering, chunk arithmetic, param clamping, feature filtering.

`lv2_host_tests` (**197 checks**) — built only when lilv is found; prints a skip
notice and passes when no hostable plugin is installed. Asserts invariants over
whatever plugins exist rather than facts about specific URIs:

- everything listed instantiates, and nothing rejected is reachable via `Find`/`Create`
- no NaN/Inf, bounded output, silence-in stays stable
- latency constant across `Reset()` and `Process()`
- **chunking equals the equivalent call sequence**: one `Process(18578)` is asserted
  bit-for-bit identical to the same input fed in the chunk sizes the loop chooses
- unknown URI and empty URI both degrade to `nullptr` (work item 4)
- degenerate calls: 0 frames, negative frames, null buffer, `Prepare(0)`, `Prepare(-1)`

### Mutation testing

Each guard was deliberately broken to confirm the test fails, then reverted:

| Mutation | Result |
| --- | --- |
| `Lv2ChunkFrames` cap removed | **aborts** — `__n < this->size()` OOB write into the scratch buffer |
| latency precedence → naive first-match-wins | 1 failure (`L.latencyOut == 2`) |
| param slot ordering reversed | 5 failures |
| optional ports NULL instead of silence | segfault in Multi-Comp *(the original bug)* |
| `lilv_plugin_verify` filter removed | 1 failure — tape_machine_2 listed but uncreatable *(the original bug)* |

The last two were not synthetic; they are the bugs that produced those fixes.

## Second commit: two latent defects

Neither is reachable with the plugins installed here; both are reachable in
principle, and both are on paths where failure would be silent.

- **`SetParam` bounds-checked `slot` against `fControlIn` but indexed `fCtrl`.**
  The two are built from the same scan and should always agree — but nothing
  enforced it, and the constructor's own `i < fCtrl.size()` guard shows the
  divergence had already been considered possible. Automation drives this path, so
  a divergence would be an out-of-bounds read on the audio thread. `fCtrl` is now
  resized to match, making the agreement an invariant instead of an assumption.
- **`Teardown` deactivated instances that were never activated.** `Instantiate`
  activates nothing until every instance exists, so a MonoDual plugin whose
  *second* instance fails to instantiate left a live-but-unactivated first instance
  to be deactivated — outside the LV2 contract. Guarded with an explicit flag.

## Verification

| Environment | Result |
| --- | --- |
| Linux host, lilv 0.28.0 | **45/45** ctest, 0 warnings |
| Linux host, `-DDAW_LV2=OFF` | **43/43** ctest, 0 warnings |
| Linux host, `-DDAW_SANITIZE=ON` (ASan+UBSan) | **45/45**, including 9 real third-party plugins instantiated and run |
| Haiku VM, LV2 **enabled**, lilv 0.24.20 | **45/45**, **0 errors, 0 warnings**, real plugins hosted, `daw` built |
| Haiku VM, LV2 auto-disabled (before lilv was installed) | **43/43**, 0 errors, 0 warnings |

(Counts are 45/44/43 depending on how many LV2 test targets the configuration
builds; every configuration is fully green.)

The VM matters because `src/main.cpp` and `src/ui/` cannot compile on Linux at all —
the VM is their only compile check, and `main.cpp` is where the factory gets
registered. `scripts/vm.sh` hardcodes `master` in its bundle, so the branch was
bundled explicitly per the handoff.

Note the lilv version gap: **0.28.0 on Linux, 0.24.20 on Haiku.** The code compiles
warning-clean and behaves identically on both; no version-conditional code was needed.

### The Haiku search-path bug (`046e65d`)

Worth reading even if you skip the rest. Once lilv was installed on the VM, the
suite still reported `0 hostable plugin(s)` — while `/boot/system/lib/lv2` visibly
contained `eg-amp` and four other *compiled* example plugins.

lilv's built-in default search path is POSIX-shaped (`~/.lv2`,
`/usr/local/lib/lv2`, `/usr/lib/lv2`) and names **no directory that exists on
Haiku**. lilv reports an empty plugin list rather than an error, so the entire LV2
feature would have shipped **silently dead on the platform this DAW targets**, with
nothing in any log to explain it. Every test would still have passed, because the
skip-if-absent path is a legitimate pass.

Fixed by supplying Haiku's real bundle directories via `setenv(..., 0)`, so an
explicitly-set `LV2_PATH` still wins. This is the strongest argument in this package
for building on the target platform rather than trusting a Linux green tick.

## Coverage: the fixture, and the two machines

`tests/lv2fixture/` is a real LV2 bundle **built by this repo** — five plugins of
exactly-known behaviour, never installed, with CTest pointing `LV2_PATH` at the
build tree. It exists because `lv2_host_tests` can only assert INVARIANTS
("everything listed instantiates") against whatever a machine happens to have, and
the two dev machines have entirely different plugin sets while CI would have
neither. The fixture asserts **values**, identically everywhere.

| Path | Fixture | Linux (9 plugins) | Haiku (4 plugins) |
| --- | --- | --- | --- |
| Stereo, one instance | stereo-latent | 9 plugins | Example Scope (Stereo) |
| **MonoDual, two instances** | mono-gain, with per-channel routing asserted | *none installed* | 3 plugins |
| **Non-zero latency** | 64 frames, reported AND applied | *none — all report 0* | *none — all report 0* |
| **Latency must not move** | reports 999 from run 2 while delaying 64 | — | — |
| Topology rejection | 3-in/1-out | 5 plugins | 3 plugins |
| Feature rejection | worker:schedule | sfizz, sfizz-multi | — |
| **Required CV port rejection** | cv-port | *no example* | *no example* |
| Malformed-bundle rejection | — | tape_machine_2 | — |
| Optional-audio silence path | — | Multi-Comp | — |
| Param slot order / ranges / clamping | exact values | 2 JUCE boilerplate ports | 0–1 params |

Two assertions the fixture buys that nothing else could, both mutation-verified:

- **MonoDual routing.** L and R are fed *different* signals, so a host that wired
  both instances to channel 0 fails. Mutating `ConnectPorts` to do exactly that
  yields `got L=2.000 R=2.000 (want 2.000 / 1.000)` — and every invariant-style
  test still passes under that mutation.
- **Latency latching.** The latent plugin reports 64 at activate then 999 from its
  second run onward, while still delaying 64 — which is what real plugins do (4K
  EQ 2 was measured going 0 → 27 → 0). Mutating `LatencySamples()` to re-read the
  port live fails two checks. Against any plugin that always reports the same
  number, that rule is simply untestable.

A third thing the fixture surfaced: with `LV2_PATH` pointing at the fixture dir
*alone*, lilv has no lv2core to resolve the plugin-class hierarchy against and
leaks a node inside `lilv_world_load_plugin_classes` — caught by ASan, gone as
soon as the spec dir is on the path. `LV2_PATH` therefore includes the system spec
bundles (located with `pkg_get_variable(... lv2 lv2dir)`, which resolves correctly
on Haiku's packagefs too). That makes the machine's own plugins visible, so every
count in the fixture test is scoped to its own five URIs rather than to the world
total.

## What is still NOT verified

- **Dynamic-latency policy is an open decision, not a gap.** 4K EQ 2 genuinely
  moves its reported latency at runtime (0 → 27 → 0). The host latches at activate
  and so reports 0 for it, leaving playback ~27 samples out whenever that EQ is
  doing work with oversampling on. No fixed value is correct for such a plugin;
  see the options in the handoff notes. The *mechanism* is now well tested — the
  *policy* needs a call.
- **No third-party plugin with non-zero latency has ever been hosted.** The
  fixture proves the mechanism end to end, but every real plugin on both machines
  reports 0, so the interaction with a real latent plugin is still unexercised.
- ~~Sample-rate change mid-session.~~ **Covered.** `Prepare` at a changed rate
  re-instantiates, and the fixture now asserts the user's parameters survive it
  (mutation-verified: re-applying defaults on re-instantiate fails three checks).
  A bug was found and fixed doing this — output control values were being carried
  across, so a plugin that does not write its latency port at activate would have
  had us reporting the latency from the OLD rate.
- ~~Reset() was never checked to do anything.~~ **Covered.** The engine calls it
  on every seek to stop stale audio bleeding across the jump; the fixture fills
  the latent plugin's delay line, resets, and asserts the buffered signal never
  emerges (mutation-verified: a no-op Reset fails).
- **Param mapping against a rich REAL plugin.** The fixture covers slot order,
  ranges and clamping exactly; of the installed plugins, only 4K EQ 2 (26 ports,
  and only after being built DSP-only for Haiku) has a meaningful control set.

## For package 03 (inserts UI) — two things you need to know

**1. These plugins expose almost nothing as control ports.** All 9 hostable plugins
report exactly 2 input control ports, and they are JUCE boilerplate:

```
4K EQ    : [Free Wheeling mn=0 mx=1 def=0] [Enabled mn=0 mx=1 def=1]
DuskVerb : [Free Wheeling mn=0 mx=1 def=0] [Enabled mn=0 mx=1 def=1]
```

Their real parameters travel as `patch:Set` atom messages, which v1 deliberately
does not author. So `KnobsForDesc` querying `Lv2Host` will draw two useless knobs
for these plugins, not a useful editor. That is a property of the plugins, not a bug
in the listing API — but it means the LV2 knob row will look wrong on this machine,
and you should not conclude the mapping is broken. Plugins that expose genuine
control ports will work correctly.

**2. Do not zero-fill `EffectDesc.params` when adding an LV2 insert.** Slot 1 is
`Enabled` with `def=1` on every one of these plugins. A fresh descriptor with
`params = {0, 0}` sets `Enabled = 0` and the plugin outputs silence, which will look
exactly like a broken host. Either leave `params` empty (the factory then applies
port defaults) or seed it from `Lv2Host::Find(uri)->params[i].def`.

The listing API is shaped to mirror `PluginHost`'s deliberately —
`Lv2ParamInfo{name, mn, mx, def}` has the same fields as `PluginParamInfo` — so the
editor can read either without knowing which it has. lilv is kept out of `Lv2Host.h`
via pimpl, so the UI needs no lilv include path.

## Natural next steps (not done here)

- **A mono-gain LV2 fixture in `tests/`** would make `lv2_host_tests` deterministic
  instead of dependent on what each machine happens to have installed. Haiku's
  example plugins now cover the monoDual path, but only by luck of what ships there.
- **Sidechain (package 05):** optional audio inputs already have a designated place
  in the layout (`silencePorts`) and are connected to a real buffer. Routing a
  signal there is a small change, not a redesign.
- **CV ports:** currently rejected when required. They are audio-rate, so they could
  be handled exactly like the silence/discard audio buffers. Left out because no
  installed plugin has one, and untested code in a memory-safety path is worse than
  no code.
- **`patch:Set` parameter messages**, which is what these JUCE plugins actually need
  for real parameter control.
- **`lv2:freeWheeling`** could be set during offline export, where it is currently
  always 0.

## `git diff --stat`

```
 CMakeLists.txt              |  57 ++++
 src/main.cpp                |  20 ++
 src/plugin/Lv2Host.cpp      | 650 ++++++++++++++++++++++++++++++++++++++++++++
 src/plugin/Lv2Host.h        |  88 ++++++
 src/plugin/Lv2PortMap.h     | 245 +++++++++++++++++
 tests/lv2_host_tests.cpp    | 283 +++++++++++++++++++
 tests/lv2_portmap_tests.cpp | 329 ++++++++++++++++++++++
 7 files changed, 1672 insertions(+)
```

No existing file's behavior changed; `CMakeLists.txt` and `main.cpp` are additive.
