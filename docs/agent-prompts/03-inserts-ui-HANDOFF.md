# Handoff into Package 03 (inserts UI) — read BEFORE `03-inserts-ui.md`

Package 02 (`lv2-host`) is merged. `03-inserts-ui.md` was written before it
existed, so a few of its statements are now stale. **This file is authoritative
where the two disagree.** Everything not mentioned here still stands.

Master is at `408607b`, 11 commits on top of 01. Verified: 45/45 ctest on Linux,
45/45 under `-DDAW_SANITIZE=ON`, 43/43 with `-DDAW_LV2=OFF`, and on the Haiku VM a
full GUI+engine build with **0 errors, 0 warnings** and 45/45.
`docs/agent-prompts/02-lv2-host-PR.md` records every decision.

## The Lv2Host API as BUILT

03 says the listing has "the same API shape as the native `PluginHost`". It does.
`src/plugin/Lv2Host.h`:

```cpp
struct Lv2ParamInfo { std::string name; float mn, mx, def; };   // == PluginParamInfo
struct Lv2PluginInfo {
    std::string name;                     // display name (URI if none declared)
    std::string uri;                      // persisted id -> EffectDesc.pluginName
    std::vector<Lv2ParamInfo> params;     // input control ports, in SLOT ORDER
    bool monoDual;                        // hosted as 2 instances, one per channel
};
class Lv2Host {
    static Lv2Host& Instance();
    void ScanAll();                                  // idempotent; main.cpp calls it
    const std::vector<Lv2PluginInfo>& Plugins() const;
    const std::vector<Lv2RejectInfo>& Rejected() const;   // uri, name, reason
    const Lv2PluginInfo* Find(const std::string& uri) const;
    std::unique_ptr<IEffect> Create(const std::string& uri, double sampleRate);
};
```

`Lv2ParamInfo` has the *same field names* as `PluginParamInfo`, so a template or a
small adapter can read either without knowing which it has — which is what
`KnobsForDesc` needs.

**lilv is kept out of the header (pimpl), so the UI needs no lilv include path.**
It does need the `daw_lv2` target, which the `daw` target already links.

## Three things that will bite you

**1. `Lv2Host` only exists when LV2 is compiled in.** Guard every use:

```cpp
#ifdef DAW_HAVE_LV2
#include "../plugin/Lv2Host.h"
#endif
```

`DAW_HAVE_LV2` is `PUBLIC` on the `daw_lv2` CMake target and is defined only when
CMake found lilv. It is currently **defined on the Haiku VM** (lilv was installed
during 02) and on the Linux host. It is NOT defined under `-DDAW_LV2=OFF`, which
is a required build configuration — so an unguarded include breaks that build.
`src/main.cpp` shows the pattern.

**2. Do NOT zero-fill `EffectDesc.params` when the browser inserts an LV2 plugin.**
This is the one that will look like a broken host. Every JUCE-generated LV2 plugin
installed here exposes exactly two control ports:

```
4K EQ    : [Free Wheeling mn=0 mx=1 def=0] [Enabled mn=0 mx=1 def=1]
DuskVerb : [Free Wheeling mn=0 mx=1 def=0] [Enabled mn=0 mx=1 def=1]
```

Slot 1 is `Enabled`, **default 1**. A fresh descriptor with `params = {0, 0}` sets
`Enabled = 0` and the plugin outputs silence. Either leave `params` **empty** (the
factory then applies the plugin's own port defaults — this is the safe default) or
seed it from `Lv2Host::Find(uri)->params[i].def`. The same applies to native
add-ons via `PluginHost`.

**3. The generic param panel will look useless for those plugins, and that is not
your bug.** Their real parameters travel as LV2 `patch:Set` atom messages, which
v1 deliberately does not author, so the only control ports are the two above.
Plugins that expose genuine control ports work correctly — the DAW's own test
fixture has 1 and 2 params, and 4K EQ 2 built DSP-only exposes 26 with proper
names and ranges. Do not conclude the mapping is broken.

## Smaller corrections to `03-inserts-ui.md`

- **"the 10 `EffectType` entries with friendly names"** — there are **11**
  enumerators (`Biquad … Lv2`), of which `Plugin` and `Lv2` are not built-ins, so
  the Built-in section holds at most **9**. The existing add-effect row in
  `EffectsWindow.cpp` offers **8** (it omits `Biquad`, kept only for loading old
  projects). Following that precedent gives 8; including legacy Biquad gives 9.
  Pick one and say which.
- **"cannot be compiled on this Linux host"** still true for `src/ui/`, but the
  Haiku VM now has `lilv`, `lilv_devel` and `lv2` installed, so it builds the LV2
  path too — you can verify the real thing there, not just the stubbed build.
  **Superseded**: `scripts/vm.sh` now syncs the host's CURRENT branch (or
  `VM_REF=<ref>`) by itself — a pushed commit is fetched by the VM from GitHub,
  an unpushed one goes over in a bundle — so the manual bundle/scp recipe below
  the line is in the git history, not needed. VM is
  `ssh -i ~/.ssh/haiku_vm user@192.168.122.48`, repo at `~/haiku-daw`.
- `KnobsForDesc` in `EffectsWindow.cpp` currently returns **no knobs** for
  `EffectType::Lv2` on purpose (falling through would draw the Biquad row and
  write Hz-scale values into whatever ports sit at slots 1 and 2). Replacing that
  with a real query is work item 3 — the placeholder comment there explains it.

## What is available to test against on the VM

`/boot/system/lib/lv2` ships five compiled example plugins; four are hostable
(`eg-amp` = "Simple Amplifier", 1 param, mono; two scopes; a MIDI gate). A
DSP-only build of the user's 4K EQ 2 (26 real params, stereo) was installed at
`/boot/home/config/non-packaged/lib/lv2/` and is hosted correctly — that is the
one worth pointing the generic param panel at, since it is the only plugin
available with a rich, properly-labelled control set.

The DAW also builds its own LV2 fixture bundle (`tests/lv2fixture/`) for tests; it
is never installed and will not appear in the browser.

## Open decision that touches the UI

A plugin may report a latency that **moves at runtime** (4K EQ 2 measured going
0 → 27 → 0). The host latches the value once at `Prepare` and never re-reads it,
which `IEffect` requires. No UI work is blocked by this, but if you surface plugin
latency anywhere in the strip, know that the number shown is the latched one and
may not equal what the plugin currently believes. See
`docs/agent-prompts/02-lv2-host-PR.md`.

## Process notes that earned their keep in 01 and 02

- **Mutation-test your key assertions**: break the code deliberately, confirm the
  test fails, revert. In 02 this proved the chunking guard, the latency latch, the
  MonoDual per-channel routing and the param-survival path were all really covered
  — and two of them were only *apparently* covered before.
- **Build on the target.** 02's worst bug (LV2 silently finding zero plugins on
  Haiku, because lilv's default search path names no directory that exists there)
  passed every Linux test and every Haiku test, because "no plugins installed" is a
  legitimate pass. Only running it on Haiku with plugins present exposed it.
- Report honestly what is unverified. For 03 that will include anything you could
  not click through on the VM.
