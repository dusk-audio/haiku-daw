# Agent Work Packages — Commercial-Parity Push

Prompts for parallel Claude (Opus) agents. Each prompt file is self-contained: hand one file to one agent as its task prompt, in this repo, on its own branch.

## Why these packages

Gap analysis vs Logic Pro / Bitwig / Reaper (2026):

| Area | haiku-daw today | Commercial baseline | Package |
|---|---|---|---|
| Insert FX | Per-track `vector<EffectDesc>` chain, 10 built-ins, native add-on ABI. No per-insert bypass, no wet/dry, no third-party standard | Ordered inserts, per-slot bypass + wet/dry, huge plugin ecosystems | 01, 02, 03 |
| Third-party plugins | Custom Haiku add-on ABI only | VST3/AU/CLAP/LV2 | 02 (LV2 via lilv — the format actually available on Haiku) |
| Plugin browser / slot UI | Floating EffectsWindow, add via menu | Channel-strip insert slots, searchable browser | 03 |
| LV2 plugin editors | Plugin's own GUI opens and drives the live insert — control-port UIs directly, DIRECT_ACCESS UIs through a mediated instance (package 07) | Editor drives the live insert | 07 (done on branch, pending click tests) |
| MIDI tools | Piano roll + CC lanes, no quantize/swing/humanize | All have full MIDI transform suites | 04 |
| Sidechain | None | All three | 05 |
| Time-stretch | None (only rate-match resampling) | Elastic Audio / Stretch markers / Warp | 06 |
| Instruments | Built-in synth + SFZ/SF2 sampler behind `IInstrument` (WIP on master) | Instrument slot per track | Already done — keep |

Already competitive (do not rebuild): buses/sends, tempo map with ramps, gain/pan/FX automation, PDC, comping/takes, loop/punch record, freeze, stems, BS.1770 loudness export, markers, SMF I/O, undo everywhere, autosave.

Explicit backlog (not in this wave): clip launcher/session view, VCA + folder tracks, ripple/slip editing, track/project templates, FLAC/MP3 export, MTC/Link sync, control surfaces + MIDI-learn, pitch-shift, scrub. Video stays a non-goal.

## Terminology decision (applies to all packages)

Commercial model: each track has **one instrument slot** (source) plus **N insert FX slots** (processors). Soundfonts (SFZ/SF2) belong to the instrument slot — that work already exists on master as uncommitted WIP (`IInstrument`, `InstrumentFactory`, `Sampler`, `SoundfontCache`). Inserts are FX only: built-in suite, native Haiku add-ons, and LV2. "Choose multiple" = multiple inserts per track, which the model already supports (`Track.fx` is an ordered vector); what's missing is per-insert bypass/mix, LV2, and slot-based UI.

## Pinned shared contract (agents must not diverge from this)

- `EffectType::Lv2 = 10`, appended after `Limiter = 9` in `src/model/Effect.h`; `kMaxEffectTypeId = 10`. For Lv2, `EffectDesc.pluginName` holds the LV2 plugin URI.
- `EffectDesc` gains `bool bypassed = false; float mix = 1.0f;` (wet/dry, 1 = full wet).
- Serialization: new optional per-insert line `fxin <index> <bypassed 0|1> <mix>` (track scope) / `masterfxin ...` (master scope), written only when non-default. The `fx` line format itself does not change.
- LV2 instantiation reaches the kit-free `EffectFactory` through a registered hook (`SetLv2Factory`), same pattern as the existing `SetPluginFactory` (`src/dsp/EffectFactory.h:18`).

## Dispatch plan

**Before wave 1:** commit the instrument-abstraction WIP currently sitting uncommitted on master (all of `src/synth/`, model/IO/engine/UI edits, new tests). Every agent branches from that commit.

- **Wave 1 (parallel):** 01-fx-inserts-core, 04-midi-tools, 06-timestretch
- **Wave 2 (parallel, after 01 merges):** 02-lv2-host, 05-sidechain
- **Wave 3 (after 01 + 02 merge):** 03-inserts-ui

Merge in numeric order. 04 and 06 touch disjoint files from 01 except small `Engine.cpp` regions — resolve trivially.

## Status (2026-07-20)

| Package | State |
|---|---|
| 01 fx-inserts-core | Merged to master |
| 02 lv2-host | Merged to master (see `02-lv2-host-PR.md`) |
| 03 inserts-ui | Merged to master 2026-07-20 (`5a09823`, host suite 46/46; see `03-inserts-ui-PR.md`) |
| 07 lv2-live-editor | **Merged to master 2026-10-09** (branch tip `4c9b2b6`) — all four phases green on the VM (`build` 49/49, `build-off` 45/45, host 48/48) and **click-tested by the user: every step of the R0 list passed**. See `07-lv2-live-editor-PR.md` |
| 04 midi-tools | **Merged to master 2026-10-09** — quantize (strength/swing/triplets), humanize, legato, transpose, velocity, in the piano roll's MIDI menu and on `q`; host 49/49, ASan+UBSan clean, VM `build` 52/52 and `build-off` 48/48, and the transform paths are driven by `ui_functional_tests` on the VM. Record `04-midi-tools-PR.md` |
| 05 sidechain | Unstarted; **unblocked** — 07 merged 2026-10-09, so the shared engine FX-loop regions are in master |
| 06 timestretch | Unstarted; DSP/exporter half host-testable, engine half can use the VM now that 07 has merged |
| R1 export dialog | **Merged to master 2026-10-09** — the bounce options dialog (format / rate / dither / loudness / true-peak / range / stems), the render moved off the UI looper with progress + Cancel, and the generic panel's wheel edit finally flushed before a save or a render. Record `08-export-dialog-PR.md` |

Each agent: branch `feature/<package-name>`, conventional commits (`feat(fx): ...`), all kit-free code must pass `ctest --test-dir build-host` on the Linux host. Haiku-only code (`src/ui/`, `src/engine/` targets, `src/plugin/PluginHost.cpp`) cannot compile on the Linux host — pattern-faithful edits, Marc verifies on the Haiku VM.
