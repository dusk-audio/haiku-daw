# T3 M7.4 LV2 state and presets — PR record

Branch `feature/lv2-state`, off `feature/dock-browsers` (T1/T2 are unmerged
prerequisites for the files this touches). Spec: `27-lv2-state.md`.

The bug: a plugin with internal state (a synth's patch, an amp sim's settings)
lost it on save. The model carried only the control-port values, and everything
a plugin keeps to itself was gone when the project was reopened — data loss,
which is why this ran ahead of the polish milestones.

## What changed

### The state travels with the descriptor

`EffectDesc` gains `state`: the plugin's own document, opaque to the model.
`IEffect` gains `SaveState`/`LoadState` (defaults: "no such notion", so every
built-in and the add-on host are untouched), and `MakeEffect` applies a
descriptor's state to a freshly built LV2 effect **before handing it out** — so
a load comes up on the saved patch, with no audio graph running yet.

`Lv2Effect` implements both with lilv's state API. Port values are deliberately
**not** part of the document (`get_value == NULL`): the control ports are the
model's `params`, and a second copy inside the blob would be a second truth that
automation — which writes the instance and never the model — would diverge from.
The save passes the host's URID map as a feature, because that is how a plugin
names the properties it stores.

### The file format

Two NEW append-only one-line records, written after the `fxin`/`masterfxin`
lines so the index they name already addresses a loaded slot:

```
fxstate <index> <base64>
masterfxstate <index> <base64>
```

Base64 (new `src/model/Base64.h`, kit-free and host-tested) is what keeps a
multi-line TTL document on one line. The `fx` line's format is untouched, an
insert with no state writes nothing at all, and a line whose index addresses no
slot or whose token is not valid base64 is *skipped* rather than failing the
load — the `fxin` policy, since a damaged blob can only be dropped and dropping
it leaves that insert at its plugin's defaults.

### Capture: the part that was actually missing

A format alone changes nothing; nothing was pulling the state into the model.
`MainWindow::FlushFxEditors` — the one point every save, render and autosave
passes through — now captures it:

1. **from the running engine** (`Engine::CaptureFxStates` asks each live
   instance), then
2. **from the open native editors**, whose flush reply now carries the state of
   the instance they own. A direct-access editor writes straight into that
   instance, so the editor wins where both exist.

The capture is applied outside the command stack — the same category as the
playhead: derived session data captured from the object that holds it, with no
visible representation for a user to undo. A plugin with nothing to save leaves
the stored state alone rather than clearing it.

### A changed state is structural

`Engine::SyncFx` pushes parameters and slot state live; a plugin's own state is
applied at **instantiation** and cannot be pushed. So each chain slot carries a
hash of its descriptor's state, and a mismatch reports the chain as structurally
different — without it, loading a preset onto a playing insert would report
"matched" and silently keep the old patch.

The same per-slot hash covers the insert's **identity** (type *and* plugin id).
Two LV2 inserts at one index share an `EffectType`, so the type alone could not
tell a replaced plugin from the one the chain was built with: SyncFx would push
one plugin's params into another, and a state capture would write one insert's
patch onto another's descriptor. Both are fixed by the identity check (a
pre-existing hole the capture made reachable).

### Presets

- **Listing**: bundle presets through `lilv_plugin_get_related` + `lv2:Preset`
  (`lilv_state_new_from_world`, label = name), then the user's own. The scan now
  records each control port's `lv2:symbol`, because a state document addresses
  ports by symbol, not by label.
- **Loading**: `Lv2Host::PresetParams` maps a preset's port values onto slots
  and clamps them through the same domain rules `SetParam` uses. The panel's
  Preset box (an LV2 insert's header, next to Byp) rewrites that insert's params
  and state and commits the whole chain through the usual `kMsgApplyFx` — one
  undoable command, the same discipline as every other edit on that panel.
- **Saving**: "Save Preset..." asks for a name (`RenameWindow`, opened by
  MainWindow; the panel only posts `kMsgFxPresetPrompt`), and MainWindow
  captures the patch the way a save does before writing it. The store is the
  DAW's own one-file-per-preset format (new `src/plugin/Lv2PresetStore.h`,
  kit-free so the format is host-tested without lilv), keyed by the plugin URI
  *inside* the file, so two plugins can both have a "Bright" preset.
  `DAW_LV2_PRESET_DIR` overrides the directory (the tests use it); the default
  is `~/config/settings/HaikuDAW/presets`.

### The chain codec and the plugin's own editor

`EncodeFxChain`/`DecodeFxChain` carry the state. Without it the panel — which
holds a copy and commits the whole chain — would have destroyed every insert's
patch on the next knob move.

`Lv2UiWindow::Open` takes the insert's state and restores it into its own
instance, so the editor opens on the patch the project holds instead of the
plugin's factory default; a preset saved from it is then the patch the user
sees.

`Lv2Host` serializes its lilv use with one recursive mutex: the insert panel's
preset menu is the first caller from a *second* looper (the effects window), and
lilv's world is not thread-safe.

## What is verified, and how

| Check | Result |
|---|---|
| Host build (exit 0) + `ctest --test-dir build-host` | 56/56 (was 53) |
| ASan+UBSan `b-asan` | 56/56 |
| `scripts/haiku_syntax_check.sh` | 0 FAIL (67 files) |
| VM `build` ctest | TBD |
| VM `build-off` ctest | TBD |
| `ui_functional_tests` + screenshot pass | TBD |

New suites: `fx_state_io_tests` (56 checks — the base64 codec and the
`fxstate`/`masterfxstate` lines, round trip and patched-file compat both
directions), `preset_store_tests` (42 checks — the preset file format, plugin
isolation, path-hostile names, torn/foreign files skipped), `lv2_state_tests`
(56 checks — save/restore asserted on the AUDIO the fixture's stateful plugin
produces, the factory's load path, the bundled preset's discovery, and the user
store through the host API). `lv2_fixture_tests` grew the new fixture plugin in
its counts; the fixture bundle grew `urn:haiku-daw:test:stateful` (a multiplier
that only `state:interface` can carry) and one bundled preset.

### Found while verifying: the syntax check was blind to the LV2 branches

The first VM build failed on the new UI test, and the reason is worth keeping:
`DAW_HAVE_LV2` is a PUBLIC definition of the `daw_lv2` target, so a real build
compiles the LV2 branches of `src/ui/` and `tests/` while
`scripts/haiku_syntax_check.sh` — which only added lilv's *include path* —
compiled them out. A use-before-declaration inside a `DAW_HAVE_LV2`-only test
function therefore passed "0 FAIL" and broke the VM. The script now defines the
macro when lilv and lv2 are both present (the same condition CMake uses); the
check is still 0 FAIL, 67 files, and it caught the two real errors in the new
test the moment it could see them.

The lilv string contract, learned the hard way and worth writing down: the
string form of a state must hold exactly ONE subject typed `pset:Preset` with an
`lv2:appliesTo`, and its saved properties hang off the `state:state` object. A
document missing any of that is not "an empty state" — lilv refuses it, which is
why `LoadState` can report failure at all.

### Mutation checks

Each broken on purpose, seen to fail, restored:

1. `ApplyFxState` returning without storing the state → 5 failures.
2. `Base64Decode` accepting a character after `=` → 2 failures (a test case was
   added first: the existing malformed inputs were caught by the length rule and
   would have hidden it).
3. `Lv2Effect::LoadState` returning false without restoring → 10 failures.
4. `Lv2Effect::SaveState` returning false without capturing → 6 failures.
5. `PresetParams` matching every port value to slot 0 → 1 failure.
6. `ParsePreset` ignoring the plugin URI → 7 failures.

## Known, not fixed

- A captured state is written into the live model outside the undo stack (see
  above), while the undo stack's chain snapshots still hold the previous state.
  Undoing an unrelated chain edit after a capture therefore restores the older
  state — which the engine then applies on its rebuild, because a state change
  is structural. The next capture puts the live patch back, so nothing is lost;
  the sequence is just surprising. Left alone deliberately: the alternative is
  capturing through the command stack (an undo entry on every save) or a state
  field the undo stack ignores, and both are worse than this.
- Capture calls a plugin's `state:interface` save from the UI thread while the
  audio thread may be running that instance. LV2 permits this only for a plugin
  that declared `state:threadSafe`; a plugin that mutates its patch from `run()`
  could in principle be read mid-change. The alternatives (stopping the
  transport on every save, or capturing on the audio thread) are both worse for
  users than the risk, and no plugin here does it.
- State documents reference external files (a sampler's WAV) through
  `state:mapPath`/`makePath`, which this host does not pass. Such a plugin's
  state properties still save and restore; its file references may not resolve.
- A `LoadPreset`ed bundle preset's port values are written into the model, but
  the panel's `params` list is not re-ordered: a preset carrying values for
  ports beyond the insert's stored count grows the vector with the plugin's
  defaults for the slots in between.

## For Marc

- The engine-side rules (a state change is structural; capture reaches the model
  from a live instance) are covered by `lv2_live_editor_tests` on the VM, since
  `Engine.cpp` links the Media Kit.
- Untestable without a person: the preset menu itself (it blocks in
  `BPopUpMenu::Go`), so what a preset *choice* does is host-tested while the
  panel's look is a screenshot.
- Still a click list: an installed plugin whose editor edits a real patch
  (4K EQ 2), save → quit → reopen, and a preset saved and reloaded.
