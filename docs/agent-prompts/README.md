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
| UI functional tests | **Merged to master 2026-10-09** — the windows are driven by posted messages on the VM (`daw_ui` + `tests/ui_functional_tests.cpp`, in the ctest suite, skips without a display). Click lists shrink to what needs eyes or ears; see `09-ui-functional-tests.md` |
| R3 stability leftovers | **Merged to master 2026-10-09** — takes compensated by the device's real round trip (plain, punch and loop-record paths), autosave-during-recording verified, `vm.sh` builds with `-j2`. Record `08-stability-leftovers-PR.md` |
| R4 release engineering | **Merged to master 2026-10-09** — the version in one place (`Version.h` + the app resource), About and `--version`, README, CHANGELOG, USER_GUIDE, the HaikuPorts recipe, and the stale-docs pass. Record `08-release-engineering-PR.md` |
| R5 release checklist | **Written 2026-10-09** — `docs/RELEASE_CHECKLIST.md`: the hardware-only pass (sound, latency, plugins, export). Marc runs it on 192.168.1.186; anything a test can reach moves into `ui_functional_tests` instead. |
| M0.1 unsaved changes | **Merged to master 2026-10-09** — never-reused undo-entry serials replace the planned clean-index, the title carries the dirty marker, Quit/window-close/Open prompt Save–Discard–Cancel, and the recovery file is deleted only after a clean save, an explicit Discard or a clean quit. Record `10-unsaved-changes-PR.md` (host 51/51 mutation-checked, ASan 51/51, VM 53/53 + 49/49) |
| M0.3 unique takes | **Merged to master 2026-10-09** — takes and renders land on the first free `<name>-N.wav` (the per-session counter is gone) and `WavWriter` refuses to clobber (`O_CREAT|O_EXCL`). Includes the first review pass. Record `11-unique-takes-PR.md` |
| M0.2 file menu | **Merged to master 2026-10-09** — New (Cmd-N), Open Recent (last 10 in AppSettings, pruned when a file vanishes), Save silent once the project has a path, Save As, Close (the window's quit path, so the prompt applies). Record `13-file-menu-PR.md` |
| M0.4 error reporting | **Merged to master 2026-10-09** — one `ReportError` alert helper; Save/Open, audio-device and recorder failures reach it (the Recorder's via an atomic code the pulse polls, stopping the take and keeping what was written); missing media after a load gets one dialog with Locate…/Skip and a kit-free `RelinkMediaCommand`; an uninstalled plugin's insert reads "(missing)". Record `14-errors-PR.md` |
| M0.5 undo gaps | **Merged to master 2026-10-09** — the tempo field, master fader (coalescing), solo-safe and the mixer strip apply (one macro with the mute-group cascade) are `CommandStack` edits now: undoable, and they mark the project dirty. Record `15-undo-gaps-PR.md` |
| M0.7 redraw + focus | **Merged to master 2026-10-09** — lanes culled against the update rect, off-view grid lines skipped, the waveform loop clipped and batched (`BeginLineArray`), the 60 Hz peak tick invalidating the header column only, crossfades cached per track; keys reach the timeline whenever a text field is not editing, a timeline click takes focus, and `DAW_TIMELINE_TIMING=1` logs draw times for the hardware measurement. Record `16-redraw-focus-PR.md` |
| M1.2 theme | **Merged to master 2026-10-09** — slice A: `Theme.h` (colour tokens, type tokens from `be_plain_font`, `kDesignFontSize`/`ThemeScale()`/`Themed()`, one meter threshold set that ends a real 1.0/0.7-vs-0.9/0.6 disagreement). Slice B: every shared metric is an accessor over `Themed()` (`RulerHeight()`, `TrackHeight()`, `HeaderWidth()` …), the arrangement view's rects, the shared drawn widgets and the transport bar scale with the font, and `SetThemeScaleOverride()` lets `ui_functional_tests` drive a 150% layout. Per-window geometry is M1.4's (record `20-theme-PR.md`; host 52/52, VM build 54/54 + build-off 50/50, ui_functional_tests 155 checks) |
| M1.3 widget kit | **Slice 1 merged 2026-10-09** — `src/ui/widgets/DawControl.h` (hover/press/focus, the wheel hook, double-click reset, Shift-fine, tooltips, Themed geometry, and the window as the default target — the bug its first test found), `DawButton`/`DawToggle`, `DawTextField`, the app's system colours from the theme, and RenameWindow converted. Host 52/52, VM build 54/54 + build-off 50/50, ui_functional_tests 159 checks. Record `21-widget-kit-PR.md`; the remaining controls and window conversions are next |
| M1.1 split MainWindow | **Merged to master 2026-10-09** — `ProjectDocument`, `TransportController`, `RecordController`, `RenderJobs` extracted (pure moves; MainWindow.cpp 3636 → 2802); the harness's locks are all bounded and the wedge lessons are in the spec. Record `18-split-mainwindow-PR.md` |
| M0.6 tracker integration | **Merged to master 2026-10-09** — `DawApplication` opens a `.dawproj` handed to it (Tracker double-click, `open`, argv) through the file panel's own `MSG_OPEN_REF`; the MIME type is registered with a sniffer rule and declared in the rdef with `B_SINGLE_LAUNCH`; the build runs `mimeset` so attributes match resources. Includes the second review pass. Record `12-tracker-integration-PR.md` |

Each agent: branch `feature/<package-name>`, conventional commits (`feat(fx): ...`), all kit-free code must pass `ctest --test-dir build-host` on the Linux host. Haiku-only code (`src/ui/`, `src/engine/` targets, `src/plugin/PluginHost.cpp`) cannot compile on the Linux host — pattern-faithful edits, Marc verifies on the Haiku VM.
