# Task: Insert-slot UI + unified plugin browser

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch `feature/inserts-ui` off master, **after** `feature/fx-inserts-core` and `feature/lv2-host` have merged. Everything in this package is Haiku-only UI code — it **cannot be compiled on this Linux host**. That raises the bar: mirror existing patterns exactly, keep diffs reviewable, and flag anything you could not verify. The user compile-tests and runs on a Haiku VM.

## Codebase orientation (read before coding)

- UI toolkit: raw Haiku Interface Kit (BeAPI) — `BWindow`/`BView`, fully custom-drawn dark widgets. Shared primitives: knob/fader/VU/button in `src/ui/Widgets.h`, palette/metrics in `src/ui/UiMetrics.h`. No native OS controls in the main surfaces.
- Threading/message pattern (critical): auxiliary windows (Mixer, Effects, Sends, Instrument, PianoRoll) run their own looper threads, edit a **local snapshot** of model state, and post the whole edited state to `MainWindow` via `BMessage` (`kMsgApplyFx`, `kMsgApplyMix`, ...). `MainWindow` owns ALL model mutation through the `CommandStack` (undo/redo). Live single-param preview during a drag uses `kMsgFxLive` → `Engine::SetFxParamLive`. Study `src/ui/EffectsWindow.h:26-44` and the `kMsgApplyFx` handler in `src/ui/MainWindow.cpp` before writing anything.
- Space-bar transport passthrough: every aux window overrides `DispatchMessage` (e.g. `src/ui/EffectsWindow.h:122`) — replicate in any new window.
- Current FX UI: `src/ui/EffectsWindow.cpp` (~800 loc) — per-track (or master, target `kMasterFxTarget`) chain editor with custom knobs, EQ curve with draggable handles, compressor transfer curve, add/remove/reorder, live GR/spectrum meters. Opened from: inspector FX box (`src/ui/InspectorView.cpp:245`), mixer strip fx button (`src/ui/MixerWindow.cpp:228` → `kMsgMixFx` → `MainWindow.cpp:576`), View menu Master Effects (`MainWindow.cpp:174`).
- What merged before you (the contract you build on):
  - `EffectDesc` now has `bypassed` (bool) and `mix` (float 0..1); `EffectType::Lv2 = 10` with plugin URI in `pluginName`.
  - Kit-free factory falls back soft on missing plugins; engine soft-bypasses (latency kept) and blends wet/dry.
  - `Lv2Host` exposes listings + param info (name/min/max/default) with the same API shape as the native `PluginHost` (`src/plugin/PluginHost.h`); built-ins describe their params via `FxParamRange` (`src/model/Effect.h:121`) and the param layout comments in `Effect.h:37-47`.

## Goal

Make inserts feel like a commercial channel strip: visible slot list on the strip, searchable browser to fill slots, per-slot bypass + wet/dry, generic editor for plugins that have no bespoke panel.

## Work items

1. **Insert slot list on the channel strip** — in `InspectorView` (replacing the single "FX" box) and `MixerWindow` strips:
   - One row per insert: effect name, bypass dot (click toggles → post a targeted apply that becomes `SetFxBypassCommand`), dim the row when bypassed.
   - Click row → open `EffectsWindow` scrolled/focused to that slot. Click empty slot → open the plugin browser.
   - Drag to reorder within the list (chain-replace apply via existing `kMsgApplyFx` path — order is just the vector order).
   - Keep strips slim; follow the inspector's existing row metrics (`InspectorView.cpp` layout) and mixer strip layout conventions.
2. **Plugin browser** — new `src/ui/PluginBrowser.h/.cpp` (`BWindow`, own looper, snapshot pattern):
   - Three sections: Built-in (the 10 `EffectType` entries with friendly names), Add-ons (`PluginHost` listing), LV2 (`Lv2Host` listing). Text filter box at top, filters across all sections.
   - Double-click (or Enter) inserts the selection into the target track's chain at the chosen slot → post through the apply path so it lands as one undoable command.
   - Replace the current add-effect menu inside `EffectsWindow` with a button that opens this browser (same target routing).
3. **EffectsWindow upgrades** (`src/ui/EffectsWindow.cpp`):
   - Per-effect header: bypass toggle + small wet/dry knob. **Both are commit-only on mouse-up — there is no live preview for these two.** The live path is `kMsgFxLive` → `Engine::SetFxParamLive(track, master, fxIndex, slot, value)` (`src/engine/Engine.h:149`), which addresses a numbered **param slot**; package 01 models `bypassed`/`mix` as separate per-insert atomics pushed by `SyncFx`, not as param slots, so there is no setter to call. Adding one would be an engine change, which this package forbids (see Constraints). Ordinary effect params keep their existing live-preview-during-drag behavior — this restriction is only about bypass and wet/dry.
     - Consequence to accept, not work around: dragging the wet/dry knob updates the drawing continuously but the audio changes once, on release. If that proves unacceptable in VM testing, write it up as a request for a live bypass/mix setter in a follow-up package — do not reach into the engine from here.
   - Generic parameter panel for `EffectType::Plugin` and `EffectType::Lv2`: vertical list of labeled sliders/knobs from host param info (name, min, max, default; double-click resets to default). Reuse the existing knob widget and wheel-adjust behavior.
   - Make sure the snapshot round-trips `bypassed`/`mix` and Lv2 entries without stripping them (01 already guarded this — don't regress it).
4. **Soundfont/instrument slot** — no work: instrument editing stays in `InstrumentWindow`. Only ensure the inspector visually distinguishes the instrument slot (MIDI tracks) from insert slots, matching the one-instrument + N-inserts model.

## Constraints

- No model or engine changes in this package. If you need one, stop and write it up instead of hacking it in UI.
- All mutation goes through `MainWindow` + commands — no direct `Project` writes from any new window.
- Match the custom-drawn aesthetic (UiMetrics palette); no `BButton`/`BListView` on the strip itself (browser window may use a list view if existing windows do — check `SampleBrowser.cpp` for precedent and follow it).

## Definition of done

- Code compiles by inspection: every message constant declared once, every handler registered, snapshot structs updated everywhere they're copied. List each file you changed and the message flow (window → constant → MainWindow handler → command) in the PR description.
- A short manual test script for the Haiku VM: add each plugin kind from the browser, reorder by drag, toggle bypass from strip and editor, wet/dry blends audibly **on mouse-up** (no mid-drag audio change — that is the specified behavior, not a bug; see work item 3), undo steps back through each action singly, save/reload preserves everything, missing-LV2 project loads soft.
- No new compile-time dependency in `daw` target beyond what 02 added.
