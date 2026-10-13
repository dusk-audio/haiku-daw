# Agent Work Packages — Commercial-Parity Push

**Current entry prompt:** `24-continue-1.0-ENTRY.md` (the 1.0 push:
`docs/PLAN_1.0.md`). M0–M7 have landed; what remains is the release itself —
the tag, Marc's hardware pass, and the three items the plan's own status header
names (M1.5 unmeasured, M1.6's HVIF bytes, M5.3 not built). The status table
below is what shipped.

Prompts for parallel Claude (Opus) agents. Each prompt file is self-contained: hand one file to one agent as its task prompt, in this repo, on its own branch.

## Why these packages

Gap analysis vs Logic Pro / Bitwig / Reaper, **as written when the packages were
planned** (2026-07-20) — it is the reasoning behind the packages, not the state
of the tree; the status table at the end of this file is what shipped.

| Area | haiku-daw then | Commercial baseline | Package |
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

Explicit backlog (not in this wave): clip launcher/session view, VCA + folder
tracks, ripple/slip editing, track/project templates, FLAC/MP3 export, MTC/Link
sync, control surfaces + MIDI-learn, pitch-shift, scrub. Video stays a non-goal.
(Since planned: templates, FLAC export and slip editing landed in 1.0; MP3 and
the rest of that list are still out.)

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

## Status (2026-10-12)

| Package | State |
|---|---|
| 01 fx-inserts-core | Merged to master |
| 02 lv2-host | Merged to master (see `02-lv2-host-PR.md`) |
| 03 inserts-ui | Merged to master 2026-07-20 (`5a09823`, host suite 46/46; see `03-inserts-ui-PR.md`) |
| 07 lv2-live-editor | **Merged to master 2026-10-09** (branch tip `4c9b2b6`) — all four phases green on the VM (`build` 49/49, `build-off` 45/45, host 48/48) and **click-tested by the user: every step of the R0 list passed**. See `07-lv2-live-editor-PR.md` |
| 04 midi-tools | **Merged to master 2026-10-09** — quantize (strength/swing/triplets), humanize, legato, transpose, velocity, in the piano roll's MIDI menu and on `q`; host 49/49, ASan+UBSan clean, VM `build` 52/52 and `build-off` 48/48, and the transform paths are driven by `ui_functional_tests` on the VM. Record `04-midi-tools-PR.md` |
| 05 sidechain | **Merged to master 2026-10-12** (PR #6) — the four notes the spec asked for (the tap point, the cycle policy, the PDC alignment, appended param slots) and a key from any other track; host `sidechain_tests` 90 checks, VM `ui_functional_tests` 217 checks. The LV2-sidechain tail is on `feature/latency-sinc-sidechain` (`47-lv2-sidechain-PR.md`). Record `31-sidechain-PR.md` |
| 06 timestretch | **Built on `feature/timestretch`, in review** — WSOLA on the disk thread and the same `Stretcher` in the exporter, the ratio on the clip and a stretch-to-fit drag; host 70/70 (`stretch_tests` 81 checks, `exporter_stretch_tests` 27, `projectio_tests` 225). Its UI still needs the screenshot pass and the live half the VM. Record `06-timestretch-PR.md` |
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
| M1.4 layout + docking | **Merged 2026-10-09** — every dialog builds with the Layout Kit and sizes to its contents; the main window is a layout with an inspector/timeline `BSplitView` (**I** toggles) over a docked pane (**J**) that hosts the MIDI editor (double-click a region; **Pop out** gives it a window); pane state persists in `AppSettings`; the dialogs' flags were in the wrong argument (feel) and are fixed with `B_FLOATING_APP_WINDOW_FEEL`; the mixer is single-instance and pans horizontally with the wheel. Host 52/52, VM 54/54 + 50/50, ui_functional_tests 172 checks. Record `22-layout-docking-PR.md`. **Since:** the browsers dock and the effects window is single-instance (PR #2, `26-dock-browsers-PR.md`) |
| M1.4 visual pass | **Merged to master 2026-10-10** — what M1.3/M1.4 left on screen, found by screenshotting every functional test: the transport bar's dead band (no min/max height), the inspector's dB readout inside the Mute row and its stale rows after a pane resize, the dock rebuilt as a header strip ("Piano Roll — track", Pop out, ×) over the editor or an empty-state hint, splitters drawn the right way round, stock button labels centred (`DrawLabel` ignored its alignment), disabled kit buttons dimmed, kit controls on their parent's colour, the knob's clipped label, clip names truncated, the loudness readout fitting, file panels titled for their action with a prefilled name, the LV2 editor window fitted to the plugin's view, the parameter panel fitted to its content, a real piano keyboard in the roll, and Open/New resetting the inspector, dock and arms (the missing-media prompt now waits for the new project to be drawn). The suite installs the app's control look, `LockedAddTrack` posts `kMsgUiRefresh`, and `DAW_UI_SHOTS=<dir>` screenshots each test. VM ctest 54/54, ui_functional_tests 210 checks. **Rules for the next UI package: `docs/UI_GUIDELINES.md`** |
| M1.5 offscreen drawing | **Measured, not implemented** — play start on 32 tracks / 320 clips was **3.1 s** (budget 300 ms; M4.1 and M4.3 brought the engine's own build to 151 ms on the VM). The timeline draw time still has no number: the VM's screen was locked, so the app_server never asked the window to draw. The protocol and what to record are in `23-offscreen-and-icons.md`; the back-buffer needs the drawing helpers to take a target `BView*` (~500 call sites), so Marc decides after he sees the figures |
| M1.6 icons | **Partly merged 2026-10-09** — the tool palette and transport glyphs are drawn vectors (`widgets/DawIcons.h`), the MIDI toolbar is icon-only with hover tooltips. **Artwork and plumbing added 2026-10-12** — `data/icons/icon-app.svg`, `data/icons/icon-project.svg`, the two commented rdef blocks, and `RegisterProjectMimeType()`'s `SetIcon` call that consumes the file icon's resource; `data/icons/README.md` is the conversion recipe. **Still owed:** the HVIF bytes, which need Icon-O-Matic on a Haiku machine — nothing here can author one (`rc` compiles icons only from raw HVIF bytes), and then the Tracker/Deskbar check at 16, 32 and 64 px. See `23-offscreen-and-icons.md` |
| M1.3 widget kit | **Merged 2026-10-09** — the kit (`src/ui/widgets/`): `DawControl` base + `DawButton`/`DawToggle`/`DawTextField`/`DawSlider`/`DawCheckBox`/`DawMenuField`/`DawKnob`, the `DawControlLook` subclass for everything stock, and the app's system colours from the theme. Converted: Rename, Quantize, Sends, Instrument, Export, Export-progress, plugin/sample browsers, and the main window's master slider + tempo field. Host 52/52, VM build 54/54 + build-off 50/50, ui_functional_tests 165 checks. Record `21-widget-kit-PR.md` |
| M1.3 widget kit | **Slice 1 merged 2026-10-09** — `src/ui/widgets/DawControl.h` (hover/press/focus, the wheel hook, double-click reset, Shift-fine, tooltips, Themed geometry, and the window as the default target — the bug its first test found), `DawButton`/`DawToggle`, `DawTextField`, the app's system colours from the theme, and RenameWindow converted. Host 52/52, VM build 54/54 + build-off 50/50, ui_functional_tests 159 checks. Record `21-widget-kit-PR.md`; the remaining controls and window conversions are next |
| T1 theme modes | **Merged 2026-10-11 (PR #1)** — the app never calls `set_ui_color` (it had been rewriting the user's Appearance for every app on the machine); the palette is a kit-free pure function of the base colours (`src/app/ThemePalette.*`), System mode derives from `ui_color()` and Dark mode is the old palette, the Dark Mode toggle switches live (now in the Preferences window), the control look follows the mode, menus/panels/alerts are themed, and the screenshot pass runs once per mode (`DAW_UI_THEME`). Host 53/53, syntax check 66 files 0 FAIL, VM `build` 55/55 + `build-off` 51/51, `ui_functional_tests` 220 checks in both modes. Record `25-theme-modes-PR.md`; spec `25-theme-modes.md` |
| T2 docked browsers + single effects window | **Merged 2026-10-11 (PR #2)** — the dock has Editor/Samples/Plugins pages behind a segmented control in its strip (the browsers became views; the windows stay as hosts), the effects window is single-instance and retargetable, the parameter slider draws a handle at its value, and the effects title names the track. Also fixed: the timeline no longer pins the window's minimum width (a 1024-wide screen cut off the strip's own buttons). Host 53/53, syntax check 67 files 0 FAIL, VM `build` 55/55 + `build-off` 51/51, `ui_functional_tests` 305 checks in System, light and dark. Record `26-dock-browsers-PR.md`; spec `26-dock-browsers.md` |
| M7.4 LV2 state and presets | **Merged 2026-10-11 (PR #3)** — `EffectDesc.state` carries the plugin's own document (lilv's state API) in two new append-only lines (`fxstate`/`masterfxstate`, base64), the control ports deliberately staying out of the blob so automation cannot diverge from a second copy of the truth, plus a preset store at the insert slot. Host 56/56, syntax check 67 files 0 FAIL, VM `build` 58/58 + `build-off` 53/53, `ui_functional_tests` 240 checks. New suites `fx_state_io_tests` (56 checks), `preset_store_tests` (42), `lv2_state_tests` (56). Record `27-lv2-state-PR.md` |
| M4.1 the engine graph off the window thread | **Merged 2026-10-11 (PR #4)** — a request snapshots the project on the caller's thread and a builder thread publishes the finished graph with one atomic exchange; `BSoundPlayer` survives every rebuild, `LoadMode::NewPosition` detaches the old graph (silence until the new one lands) and `LoadMode::InPlace` joins the running one at the playhead, so a structural edit while playing is heard with no gap. Host 53/53, ASan 53/53, syntax check 0 FAIL, VM `build` 55/55 + `build-off` 51/51, `ui_functional_tests` 224 checks; new `engine_graph_tests` (47 checks). Record `29-engine-graph-PR.md` |
| M6 formats: AIFF, FLAC, Ogg Vorbis | **Merged 2026-10-11 (PR #5)** — `IAudioSource`/`IAudioSink` factories sniff the format from the header rather than the extension, the ExportWindow's bit-depth menu becomes one Format menu, and both codecs stay optional at build time (`DAW_FLAC`, `DAW_VORBIS`); the VM has the devel packages installed, so the packaged build has both. VM `build` 55/55 (`formats_tests` 274 checks) + `build-off` 51/51, `ui_functional_tests` 253 checks; host `formats_tests` 280 checks (six `/dev/full` legs are Linux-only). Record `28-formats-PR.md` |
| M7.1 sidechain (the built-ins) | **Merged 2026-10-12 (PR #6)** — the compressor and gate take a key from any other track: the tap is post-fader/post-FX (where a post-fader send taps), the cycle policy is all-or-nothing (the sends' precedent), PDC delay-aligns the key to the consumer's input, and one param slot each is appended so an existing project is untouched. Host 53/53 with `sidechain_tests` (90 checks) and `projectio_fuzz_tests` (3071 checks); VM `ui_functional_tests` 217 checks. Record `31-sidechain-PR.md` |
| M2.1–M2.3 arrange-window feel | **Merged 2026-10-11 (PR #7)** — one `HitTest` behind the pointer feedback (cursors, edge highlights), zoom anchored on the mouse, wheel scrolling and real scrollbars, the six-tool palette with the snap grid menu and its indicator (one kit-free definition of the grid), glue and slip commands, and Split at playhead on `S`. Host 58/58 (`snapgrid_tests` 65 + `arrange_cmd_tests` 64 checks), syntax check 67 files 0 FAIL, VM `build` 60/60 + `build-off` 55/55, `ui_functional_tests` 329 checks. Record `30-arrange-feel-PR.md` |
| M7.3 MIDI expression | **Merged 2026-10-11 (PR #8)** — pitch-bend as a per-block integrated ratio (so a seam never steps), the sustain latch (CC64) for a played *and* a recorded pedal, the mod wheel's 5 Hz vibrato, and a CC lane that offers any of the 128 controllers. Host 53/53 (`midi_expression_tests` 61 checks), ASan 53/53, VM `build` 55/55 + `build-off` 51/51, `ui_functional_tests` 226 checks. Known limits (live controllers are engine-wide, only six controllers act on a voice) are in the record. Record `32-midi-expression-PR.md` |
| M2.4–M2.5 range editing and clip depth | **Merged 2026-10-11 (PR #9)** — a time range independent of the clips with Cut/Copy/Paste/Delete/Insert Silence, the full Edit menu with Undo/Redo labels read from the stack, clip name/colour/mute, fade shapes (equal-power for auto-crossfades), slip edit and nudge. Host 59/59 (`range_edit_tests` 147 checks), syntax check 68 files 0 FAIL, VM `build` 60/60 + `build-off` 55/55 (both minus the UI suite), `ui_functional_tests` 411 checks in three modes. Record `33-arrange-depth-PR.md` |
| M3.2 keyboard map + M3.3 meters | **Merged 2026-10-11 (PR #10)** — one kit-free key table (`src/model/KeyMap.h`) drives the behaviour *and* the generated help window (Home/Enter return to zero, R records, L loops, C clicks; the never-firing Ctrl-D binding was found and fixed on the way), and the master meter gains a dB scale, RMS, peak hold, a click-to-reset clip lamp and a correlation strip, with the same `DrawMeterBar` on every other meter. Host 54/54 (`keymap_tests` 608 + `meter_tests` 59 checks), ASan 54/54, syntax check 66 OK 0 FAIL, VM 56/56 + 52/52, `ui_functional_tests` 330 checks. Record `35-keymap-meters-PR.md` |
| Haiku conformance audit | **Merged 2026-10-11 (PR #11)** — `docs/HAIKU_CONFORMANCE.md`, 1638 lines: every HIG chapter, the coding guidelines, publishing and the icon guidelines checked item by item — 92 items, 45 Met / 35 Deviates / 12 Not applicable, the five worst named, the upstream-only items separated, and a 30-item worklist with two proposed tasks (T14, T15). Docs only, no code. |
| M2.6 track depth + M2.7 markers | **Merged 2026-10-11 (PR #12)** — rename in place, lane-edge drag for height, a colour swatch and picker, Duplicate Track (fresh ids, reused on redo), exclusive solo, a per-track input selector, collapse/hide, and a markers window with a marker lane. Host 60/60 (`track_depth_tests` 60 checks), syntax check 69 files 0 FAIL, VM `build` 61/61 + `build-off` 56/56 (both minus the UI suite), `ui_functional_tests` 491 checks in three modes. Record `34-arrange-tracks-PR.md` |
| The shortcuts window in Dark mode | **Merged 2026-10-11 (PR #13)** — the window's text takes the theme's colours (`ShortcutsWindow`), with the check added to `ui_functional_tests`. Docs-only record: the merge and its two commits are the record. |
| M4.2 seamless loop | **Merged 2026-10-12** (PR #14) — the loop is walked inside the running graph: the RT splits its block at the loop end (sample-exact) and the disk threads produce each clip's audio in playback order across the seam, so a pass costs the loop's own length (VM: 1002 ms per pass of a 1 s loop) and `GraphsPublished`/`PlayersOpened`/`PlayerStarts` do not move. A loop-range edit re-positions the engine; loop recording still re-aligns per pass. Host 67/67, VM `build` 69/69 + `build-off` 64/64, `ui_functional_tests` 815 checks. Record `36-seamless-loop-PR.md` |
| M4.3 the disk-stream pool | **Merged 2026-10-12 (PR #15)** — a fixed set of fill threads (static assignment, lock-free) replaces one thread per clip; only the clips due inside the pre-roll are primed, EOF is "nothing more" rather than a retry, and the ring is sized to the pool's visit interval. The VM's build went 3295 ms → 151 ms for 329 streams, with sound 0 ms after the graph lands. Host 68/68 (`stream_pool_tests` 75 checks), syntax check 73 OK 4 FAIL (the pre-existing codec files), VM `build` 70/70 + `build-off` clean, `ui_functional_tests` 817 checks. Record `37-stream-start-PR.md` |
| M4.4 (first slice) peaks as a background job with a disk cache | **Merged 2026-10-12 (PR #16)** — waveform peaks load off the window thread and cache on disk keyed by path, modification time and size, so reopening a project is a hit rather than a decode. Host 68/68 (`peak_tests` 60 checks), syntax check 75 OK 4 FAIL (pre-existing), VM `build` 69/70 + `build-off` 63/64 — the single failure in each is the pre-existing FX-drag check, which fails the same way on master's binary (the record carries both runs). Record `38-peak-cache-PR.md` |
| README refresh | **Merged 2026-10-12 (PR #17)** — the front page rewritten (what it does, requirements, building, testing, status, limitations) with two screenshots. Docs only. |
| USER_GUIDE formats fix | **Merged 2026-10-12 (PR #18)** — the guide's import/export paragraph now matches what the build offers. Docs only. |
| M1.1 split MainWindow | **Merged to master 2026-10-09** — `ProjectDocument`, `TransportController`, `RecordController`, `RenderJobs` extracted (pure moves; MainWindow.cpp 3636 → 2802); the harness's locks are all bounded and the wedge lessons are in the spec. Record `18-split-mainwindow-PR.md` |
| M0.6 tracker integration | **Merged to master 2026-10-09** — `DawApplication` opens a `.dawproj` handed to it (Tracker double-click, `open`, argv) through the file panel's own `MSG_OPEN_REF`; the MIME type is registered with a sniffer rule and declared in the rdef with `B_SINGLE_LAUNCH`; the build runs `mimeset` so attributes match resources. Includes the second review pass. Record `12-tracker-integration-PR.md` |

**The 1.0 close-out: branches cut from `master`, in review, each with its own
record and its own counts** (counts are per branch, against `master` — not
against each other, and not a merged total):

- **`feature/m3-shell`** — M3.1 (the transport bar), M3.3–M3.7 (meter polish,
  mixer strips, the Preferences window, Project Settings, New Project
  templates) and M4.4's region jobs. Records `39-transport-bar-PR.md`,
  `40-mixer-strips-PR.md`, `41-preferences-PR.md`,
  `42-project-settings-templates-PR.md`, `43-region-jobs-PR.md`, plus
  `48-pr1-integration-fixes.md` for the integration pass. Host 71/71, VM
  `build` 73/73 + `build-off` 68/68, `ui_functional_tests` 960 checks with the
  shots opened in both theme modes.
- **`feature/multichannel-input`** — M5: a device and channel pair per track,
  mono/stereo tracks, one take per armed track with per-take de-interleave, and
  16/24-bit/float takes. Record `44-multichannel-input-PR.md` (host 69/69,
  syntax check 74 OK 4 FAIL). Multichannel capture itself has no test — no
  audio device exists on the host or the VM — so its click list is Marc's.
- **`feature/latency-sinc-sidechain`** — M4.5 (the windowed-sinc resampler),
  M4.6 (dynamic LV2 latency) and M7.1's LV2 sidechain tail. Records
  `45-sinc-resampler-PR.md`, `46-dynamic-latency-PR.md`,
  `47-lv2-sidechain-PR.md` (host 69/69 each, syntax check 75 OK 4 FAIL).
- M7.2 time-stretch is the `feature/timestretch` row above. These five rows and
  PR 1's integration record (`48-pr1-integration-fixes.md` — 48, not 44: the
  multichannel package owns 44) landed together in PRs #19–#21.

Each agent: branch `feature/<package-name>`, conventional commits (`feat(fx): ...`), all kit-free code must pass `ctest --test-dir build-host` on the Linux host. Haiku-only code (`src/ui/`, `src/engine/` targets, `src/plugin/PluginHost.cpp`) cannot compile on the Linux host — pattern-faithful edits, Marc verifies on the Haiku VM.

**UI work — whoever implements it (Claude, DeepSeek or any other model) — follows `docs/UI_GUIDELINES.md`.** A change to what a window draws or how it is laid out is done only when the implementer has run the `DAW_UI_SHOTS` screenshot pass on the VM, opened every shot of the windows it touched, and fixed what it saw. Green `ui_functional_tests` is necessary, not sufficient: M1.3/M1.4 merged at 172 checks with the main window visibly broken, because the looking was left to a click list. A click list is for ears and hardware, not for the screen.

Commits and PR text carry **no** `Co-Authored-By:` or other AI-attribution trailer (`docs/HANDOFF.md`, commit rules).
