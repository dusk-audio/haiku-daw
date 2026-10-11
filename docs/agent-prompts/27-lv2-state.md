# Task: LV2 state and presets — stop losing a plugin's own patch on save

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/lv2-state` off `feature/dock-browsers` (T1/T2 are unmerged prerequisites
for the files this touches; the chain is master → feature/theme-modes →
feature/dock-browsers → yours). Same rules as every package: spec first, one
verification bar per branch, a record (`27-lv2-state-PR.md`) kept as you go.

## Codebase orientation (read before coding)

- Layering. Kit-free `daw_model` (model + DSP + Exporter + the LV2 *decision*
  headers) builds and is host-tested on Linux; `src/engine/Engine.cpp`,
  `src/ui/` and `src/plugin/Lv2Host.cpp` are Haiku-side (Lv2Host.cpp only because
  it links lilv, which the VM has). `tests/lv2fixture/` is a real LV2 bundle this
  repo builds purely for tests, LV2_PATH-scoped; `tests/lv2_fixture_tests.cpp`
  links `daw_lv2` and asserts VALUES against it. lilv 0.28 state API is available
  on both the host and the VM (`lilv_state_new_from_instance`, `_to_string`,
  `_from_string`, `_restore`, `_new_from_world`, `_emit_port_values`, `_save`).
- The insert descriptor is `EffectDesc` (`src/model/Effect.h`): type + flat
  `params` + `pluginName` (the URI for `EffectType::Lv2`) + bypass/mix. The engine
  builds live effects from it with `MakeEffect` (`src/dsp/EffectFactory.cpp`),
  which for LV2 calls the `Lv2Host` hook; `Lv2Effect` (`src/plugin/Lv2Host.cpp`)
  owns the lilv instances, the control-port storage and the scratch buffers.
- Persistence is append-only text (`src/model/ProjectIO.cpp`): a new field is a
  NEW optional line; the `fx` line's format must not change. Compat tests both
  directions use the `LoadPatched` pattern in `tests/instrument_io_tests.cpp`
  (save a real project, rewrite the text to stand in for another build, load).
- Engine chain edits: `Engine::SyncFx` (`src/engine/Engine.cpp:623`) pushes params
  when the chain's *types/count* still match and returns false when it must be
  rebuilt; `MainWindow::SyncFxToEngine` reloads on false. `Engine::Load` builds
  the chain (`:364-402`).
- The effects editor (`src/ui/EffectsWindow.cpp`, fully custom-drawn) holds a
  snapshot of the chain and commits the WHOLE chain back with `kMsgApplyFx`
  (one undoable `SetFxCommand`); the chain travels as `EncodeFxChain`/
  `DecodeFxChain` fields, so anything the descriptor carries and the codec drops
  is silently destroyed on the next knob move. The single-instance window and
  retarget are T2's; the panel header rows are drawn in `Draw`.
- The plugin's own GUI (`src/ui/Lv2UiWindow.cpp`) gets its OWN instance, never
  the engine's, and mirrors values through `kMsgFxLive`/`kMsgFxParamCommit` and
  the engine watch. `MainWindow::FlushFxEditors` asks each open editor for its
  pending values before a save/render (bounded reply, 200 ms).
- RT rules (`docs/agent-prompts/24-continue-1.0-ENTRY.md` §7): the audio callback
  allocates nothing, locks nothing, does no I/O. State save/restore is off-RT by
  definition (lilv allocates).

## Goal

A plugin with internal state (a synth patch, an amp sim's settings) keeps it in
the project: save it with lilv's state API, restore it on load, and give the
insert slot a way to list, load and save presets. Today the state is simply lost
on save — a data-loss bug, which is why this runs ahead of the polish milestones.

## Work items

1. **Model** — `EffectDesc` gains `std::string state` (declared last, like
   bypass/mix): the plugin's own state as produced by `IEffect::SaveState`.
   Empty for anything but LV2.
2. **A codec** — a kit-free Base64 (encode + strict decode) in
   `src/model/Base64.h`, header-only and host-tested: the state is a multi-line
   TTL document, and the project format is one line per insert.
3. **Persistence** — two new append-only lines, after the `fxin`/`masterfxin`
   lines so the index they name already addresses a loaded slot:
   `fxstate <index> <base64>` and `masterfxstate <index> <base64>`. Written only
   for a NON-EMPTY state; a line whose index addresses nothing, or whose base64
   is malformed, is skipped (the `fxin` malformed-input policy: nothing to attach
   it to is not a corrupt file). The `fx` line itself is untouched.
4. **IEffect** — `virtual bool SaveState(std::string* out) const` and
   `virtual bool LoadState(const std::string& in)`, both defaulting to
   "no such notion". `Lv2Effect` implements them with
   `lilv_state_new_from_instance` (+ `lilv_state_to_string`) and
   `lilv_state_new_from_string` (+ `lilv_state_restore`). Port values are NOT
   part of the host's save (`get_value` NULL): the model's `params` already own
   the control ports, so the blob carries only what the model cannot.
   `MakeEffect` applies `desc.state` to a freshly created LV2 effect *before it
   is returned*, so a load restores state before the audio graph runs.
5. **Rebuild discipline** — a changed `state` is STRUCTURAL for `SyncFx` (a
   hash per slot; a mismatch makes it return false so the chain rebuilds with the
   new state), because internal state cannot be pushed through `SetParam`. The
   same per-slot record carries the insert's IDENTITY (type and plugin id), not
   just its type: two LV2 inserts at one index share an `EffectType`, and the
   type alone would let a same-index plugin swap push one plugin's params into
   another and capture one insert's state onto another's descriptor.
6. **LV2 host** — scan records each control-input port's SYMBOL (needed to map a
   preset's port values onto slots); `Lv2Effect` keeps the world + URI it needs
   to serialize; `Lv2Host` gains preset discovery/IO:
   - `Presets(uri)` — bundle presets via `lilv_plugin_get_related` +
     `lv2:Preset` + `lilv_state_new_from_world` (label = name, state string),
     then user presets;
   - `SavePreset(uri, name, state, params)` — our own one-file-per-preset store
     (`src/plugin/Lv2PresetStore.h`, kit-free and host-tested), default
     `~/config/settings/HaikuDAW/presets`, overridable for tests;
   - `PresetParams(uri, state)` — the port values a state carries, mapped
     symbol → param slot and clamped with `ClampLv2Param`, so loading a preset
     writes real values into `EffectDesc.params`.
7. **Capture at save** — the state lives in a live instance, not in the model
   (that is the bug). At every point the model is about to be serialized or
   snapshotted (`MainWindow::FlushFxEditors`, which every save/render/autosave
   path already calls): first ask the running engine
   (`Engine::CaptureFxStates`), then the open native editors (their flush reply
   carries the state), each write landing in the model's descriptor. The
   editor wins where both exist: it is the instance the user is editing.
8. **Presets in the UI** — a `Preset` box on an LV2 insert's panel header in
   `EffectsView` (custom-drawn, next to Byp): a popup listing the plugin's
   presets plus "Save Preset...", which asks for a name through the existing
   `RenameWindow` prompt and posts `kMsgSaveFxPreset` to MainWindow. Choosing a
   preset rewrites that insert's `params` + `state` in the panel's copy and
   commits the whole chain through the normal `kMsgApplyFx` path — one undoable
   command, the same discipline as every other edit on that panel.
9. **Chain codec** — `EncodeFxChain`/`DecodeFxChain` carry the state (`es`
   string). Without this a panel knob move would wipe every insert's state.
10. **The plugin's own editor** — `Lv2UiWindow::Open` takes the insert's state
    and restores it into its own instance (so the editor opens on the patch the
    project holds, and a preset saved from it is the patch the user sees); its
    flush reply carries the instance's current state so the capture above has
    something to take.

## Definition of done

- All existing suites green; new host tests for the codec (`Base64` round trip
  incl. empty/newlines/quotes and malformed input), the preset store
  (save/list/load, name and plugin isolation, a torn file ignored), and the
  ProjectIO round trip + patched-file compat both directions (no `fxstate` line →
  empty state, `fxstate` line → state back, bad index / bad base64 skipped, and
  the `fx` line byte-identical to the old format).
- `lv2_fixture_tests` covers, against a new fixture plugin with a real
  `state:interface`: save produces a blob, load reaches the plugin (audio
  changes), save→load→save round-trips, a preset fixture bundle lists with its
  label, and `PresetParams` maps a preset's port value onto the right slot.
- Every new behaviour is mutation-checked (break it, watch the new assertion
  fail, restore) and the mutation is named in the commit body.
- Verification for the record: host build exit 0 + ctest, ASan, syntax check
  0 FAIL, VM `build` and `build-off` ctest, and the `DAW_UI_SHOTS` pass for the
  panel change (every shot showing the effects window opened and checked).
