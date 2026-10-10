<!-- Copied into the repo 2026-10-10 from the planning session's file
     (~/.claude/plans/bright-juggling-bird.md), so every implementer can read
     it. This copy is now the plan of record; edit it here. -->

> **Status (2026-10-10).** Done: M0.1–M0.7, M1.1, M1.2, M1.3, M1.4 (minus the
> docked browser pane and a single-instance effects window) and its visual pass.
> M1.5 is measured, not built; M1.6 has the vector toolbar icons but no HVIF
> app/file icon. M2–M8 are not started. Per-item records and the live status
> table: `docs/agent-prompts/README.md`. Where to start:
> `docs/agent-prompts/24-continue-1.0-ENTRY.md`.
>
> **Decided 2026-10-10 (Marc), overriding M1.2/M1.3 below where they differ:**
> the app never recolours the OS (`set_ui_color` goes); the default look is
> native Haiku with an optional dark mode in Preferences for the DAW's windows
> only; M1.5 offscreen drawing waits for a hardware draw-time measurement;
> M1.6 icons are HVIF in the BeOS/Haiku icon style.

# Haiku DAW → 1.0: commercial look, feel and behaviour

## Context
Packages 01–04, 07 and R0–R5 are merged on `master` (`51e52c5`). The CHANGELOG already says 1.0.0. A code review by three audits (UI, workflow, robustness) found that the engine and DSP are solid. What is missing is the layer a user touches: data safety, file handling, window layout, widget style, interaction feedback and responsiveness. These are what make the app feel like a prototype rather than a commercial DAW.

Decisions Marc made on 2026-10-09 (record them in `docs/agent-prompts/08-release-1.0-ENTRY.md` §2):
- **Hold the 1.0 tag** until this plan lands. Until then the CHANGELOG heading reads `1.0.0 — unreleased`. `Version.h` stays 1.0.0.
- **In 1.0 scope:** sidechain (package 05), multichannel input, FLAC/Ogg export, time-stretch (package 06).
- **Dynamic LV2 latency:** re-solve plugin delay compensation (PDC) when a plugin's reported latency changes.

Confirmed defects that block release (verified in code):
- `MainWindow::QuitRequested` (`src/ui/MainWindow.cpp:3208`) quits without a save prompt and deletes the recovery file. Nothing tracks whether the project has unsaved changes.
- Take files are named from `fTakeCounter`, which restarts at 0 each session (`MainWindow.cpp:1717`), and `WavWriter` opens with `ios::trunc` (`WavWriter.cpp:40`). Recording again after reopening a project **overwrites takes the project still uses**.
- Load, save, record, missing-media, audio-device and plugin-load failures only print to stderr.

## How the work runs (same discipline as packages 01–09)
- One branch per milestone item: `feature/<slug>` or `fix/<slug>` off `master`, `--no-ff` merges, and a `docs/agent-prompts/1N-<slug>-PR.md` record for each.
- Logic goes into kit-free code first (`src/model`, `src/dsp`, or headers) with host tests, plus a mutation check. Haiku-only code must pass `scripts/haiku_syntax_check.sh` with 0 FAIL, then VM `build` and `build-off` ctest.
- GUI flows get automated in `tests/ui_functional_tests.cpp`. Marc only checks things that need eyes or ears; those go on the PR's click list.
- Hard rules are unchanged:
  - no allocation, locks or I/O in `FillBuffer`;
  - no `BMediaFile` or Media Kit decoders;
  - every model edit goes through `CommandStack`;
  - no AI trailers in commits.

---

## M0 — Data safety and trust (P0; do first, small)
1. **Unsaved-changes tracking.** Give each undo entry in `CommandStack` (`src/model/Command.h`, kit-free) a serial number that is never reused, and renew the entry's serial whenever a drag is merged into it. On save, record the serial of the top entry; the project is unsaved whenever the top entry's serial differs from the recorded one. For an empty stack keep a base serial: 0 at first, then the serial of the last entry dropped when history is trimmed (the stack holds 256 entries). No clean-index — comparing the current position with the saved one is wrong twice over: a drag merged into the saved entry moves no position, and undoing everything left after a trim does not reach the original project while position zero claims it does. Show `*name — Haiku DAW` in the window title.
   - Prompt Save / Discard / Cancel on Quit, Open, New and window close.
   - Only delete the recovery file after a clean save or an explicit Discard.
   - Host-test the serial across undo, redo, coalescing (a merged drag must read dirty) and a trimmed stack undone to empty.
2. **File menu:** New (Cmd-N), Open, Open Recent (last 10, kept in `AppSettings`), Save (silent once the project has a path), Save As (Shift-Cmd-S), Close.
3. **Unique take names.** Pick the next free `take-N.wav` by scanning the take directory, or use a timestamp name. `WavWriter` refuses to overwrite an existing file (open with an exclusive-create flag). Host test: two sessions never produce the same name.
4. **Errors the user can see.** Add one `ReportError(title, detail)` helper (a dark-themed alert; see M1) and route these through it:
   - `LoadFrom` and `SaveTo` failures;
   - missing media on load, as one dialog listing the files, with Locate… and Skip;
   - audio device init failure;
   - Recorder open or write failure, where disk full stops the take and keeps what was written;
   - plugin load failure, shown as a "missing" insert badge.
   This also makes RELEASE_CHECKLIST item 27 pass.
5. **Edits that bypass undo become commands:** tempo field (`MainWindow.cpp:1312`), master gain (`:1272`), mixer strip apply (`:919`), solo-safe (`TimelineView.cpp:812`).
6. **Tracker integration:**
   - add `ArgvReceived` and `RefsReceived` on the app;
   - delete the stale WAV-argv path in `main.cpp`;
   - register a `.dawproj` MIME type with a sniffer rule;
   - add `file_types` and app flags in `haiku-daw.rdef.in`.
7. **Redraw performance and keyboard focus:**
   - clip the waveform loop to the visible area (`TimelineView.cpp:2190`, `:2396`), and draw it with `BeginLineArray`;
   - cull off-screen lanes, and use the update rect in `DrawLanes` (`:1871`);
   - `SetTrackPeaks` should invalidate only the meter column (`TimelineView.h:103`);
   - cache `ComputeCrossfades` per edit instead of per draw;
   - add a `MainWindow::DispatchMessage` that routes Space and edit keys to the timeline whenever a text field isn't actively editing;
   - call `MakeFocus` when the timeline gets a `MouseDown`.

## M1 — UI foundation (enables everything visual)
1. **Split up `MainWindow`.** It is 3.2k lines with an 84-case `MessageReceived` handler. Extract:
   - `ProjectDocument`: path, dirty flag, load/save/recovery;
   - `TransportController`: engine lifetime, play/stop/seek/loop;
   - `RecordController`: arm, takes, latency compensation;
   - `RenderJobs`: export, freeze, region ops on worker threads.
   This is pure moving of code, verified by `ui_functional_tests`. Do it first, so later branches don't collide in one file.
2. **Theme system.** Extend `UiMetrics.h` into `Theme.h`:
   - colour tokens, plus font tokens taken from `be_plain_font` size;
   - a scale factor so every metric in `UiMetrics`, `Widgets.h` and the `TimelineView` rects is relative to the font size (HiDPI);
   - one set of meter thresholds, fixing the 0.6/0.9 vs 0.7/1.0 mismatch.
3. **Dark widget kit** (`src/ui/widgets/`): `DawButton`, `DawToggle`, `DawSlider`/`DawFader`, `DawKnob` (lifted from the `EffectsWindow` knob), `DawTextField`, `DawMenuField`, `DawCheckBox`, `DawListView`, `DawProgress`, `DawAlert`.
   - Every value control gets hover, pressed and focus states, double-click to reset to default, Shift for fine adjustment, the wheel, and a tooltip.
   - Replace the stock light controls in the TransportBar (BPM field, volume slider), SampleBrowser, PluginBrowser, InstrumentWindow, SendsWindow, QuantizeWindow, ExportWindow, RenameWindow and ExportProgressWindow.
   - Also install a dark `BControlLook` subclass, so any stock control that remains (menus, scrollbars) matches.
4. **Layout Kit and docking:**
   - rebuild every dialog with `BLayoutBuilder`;
   - give the main window `BSplitView` panes: an inspector that can be resized and toggled (I), a docked bottom editor pane that hosts the piano roll (with a "pop out" option), and a docked browser pane (samples and plugins in tabs);
   - mixer, effects and plugin browser become single-instance (show and activate the existing window);
   - the mixer scrolls horizontally;
   - dialogs use the modal or floating window feel;
   - pane sizes and visibility persist in `AppSettings`.
5. **Offscreen drawing** for `TimelineView`, `PianoRoll` and `MixerWindow`: a `BBitmap` back-buffer with static layers cached and only the playhead and meters redrawn.
6. **Icons:** an HVIF app icon in the rdef; vector toolbar and tool-palette icons (HVIF as resources, or drawn with `BShape`); a project-file icon.

## M2 — Arrange window feel
1. **Pointer feedback.** Cursors for trim, fade, gain, move, split and slip. A hover highlight on clip edges and header buttons. Tooltips throughout.
2. **Navigation:**
   - the zoom anchors on the mouse position (Ctrl+wheel; keys −/+ anchor on the playhead);
   - Shift+wheel or a horizontal wheel scrolls sideways;
   - real scrollbars;
   - you can scroll past the end of the last clip;
   - the piano roll follows the playhead;
   - optional overview minimap strip under the ruler.
3. **Arrange tool palette** (pointer, pencil, scissors, glue, mute, fade), keys 1–6, matching the piano roll. Split at playhead with S. A grid menu (bar, 1/2 … 1/32, triplets, off) with a snap indicator.
4. **Time-range selection** that is independent of clips (drag on empty lane area, or in the ruler with a modifier). Range Cut, Copy, Paste, Delete and Insert silence across the selected tracks.
   - Full Edit menu: Cut, Copy, Paste, Delete, Select All, Duplicate.
   - The clipboard holds many clips and pastes at the playhead onto the selected track.
   - Undo and Redo labels come from `UndoName()`, and child windows forward Cmd-Z.
5. **Clip depth:**
   - audio clips gain `name`, `colorIndex` and `muted`;
   - names are truncated to fit;
   - the waveform is drawn after clip gain and fades;
   - fade shapes (linear, equal-power, S-curve), using equal-power by default for crossfades;
   - slip edit (Alt-drag moves the content inside the clip);
   - nudge with `,` `.` by grid step.
   - Model, IO and command changes are host-tested; new lines in `ProjectIO` are append-only, with compat tests.
6. **Track depth:**
   - rename in place;
   - drag the lane edge to change height;
   - a colour swatch and colour picker;
   - Duplicate Track;
   - Alt-click for exclusive solo;
   - an input selector in the header;
   - collapse or hide.
7. **Markers:** a markers/locations list window, and a marker lane that can be toggled.

## M3 — Transport, meters, mixer, preferences
1. **Transport bar:**
   - buttons for return-to-zero, rewind and fast-forward, Play, Stop, Record, Loop, Metronome and Punch, each showing its state;
   - a large clickable time display that cycles between BBT, min:sec, samples and timecode, and accepts a typed position;
   - tempo and meter display;
   - CPU/DSP load (the engine measures how long each callback takes against the buffer duration, using atomics) and an xrun counter (engine underruns plus `Recorder::fXrun`);
   - a status bar.
2. **Keyboard map:** Home goes to start (it currently only scrolls), R records, L toggles loop, C toggles click, Enter returns to zero, and so on. The shortcuts window is generated from one kit-free key table, so the help and the behaviour can't drift.
3. **Meters:** a dB scale with peak hold and a clip indicator that resets on click, plus RMS. A correlation meter on the master. The kit-free meter math is host-tested.
4. **Mixer strips:**
   - send-level knobs on the strip, input trim, phase invert, and solo groups (all in the model and the engine and exporter, host-tested in the exporter);
   - fader dB entry by double-click;
   - inserts already exist, so restyle them with the widget kit.
5. **Preferences window:** audio output device and buffer size, the default sample rate, count-in, the metronome sound and level, UI scale, default project folder and autosave interval. These move out of the menu radio items, and the Audio menu keeps quick toggles.
6. **Project Settings:** sample rate (offering a resample of the media), tempo, meter and notes.
7. **New Project dialog** with templates: Empty, Band (8 audio + bus), Producer (MIDI and audio). Templates are `.dawproj` files shipped in `data/templates`.

## M4 — Engine responsiveness (feel under load)
1. **Engine load off the window thread.** Build the graph on a worker thread and swap it in with an atomic pointer exchange. Keep `BSoundPlayer` alive across structural edits, so editing during playback causes no gap. The RT side only ever exchanges pointers; the old graph is freed on a reclaim thread.
2. **Seamless loop.** Loop wrap and seek-while-playing currently restart the engine (`MainWindow.cpp:1465`, `:488`). Replace that with an in-engine seek: the disk streams pre-roll the loop start.
3. **Disk-stream pool.** A fixed set of worker threads, and only clips near the playhead hold an open stream. This replaces one thread per clip with a wait of up to 100 ms each.
4. **Background peaks** with an on-disk peak cache keyed by path, modification time and size. Freeze, Reverse, Normalize and Strip Silence run as `RenderJobs` with progress.
5. **Windowed-sinc resampler** (kit-free `src/dsp/`) for export at another rate and for file-rate conversion. The current linear resampler stays only for the live monitor path. Host tests check aliasing rejection and passband flatness.
6. **Dynamic LV2 latency.** `Lv2Host` polls the latency port off the RT thread (the meter tick reads a latched atomic the RT thread writes). On a change, the engine re-solves PDC on the worker thread and swaps the delay lines in at a block boundary through item 1's graph swap.
   - The exporter re-latches after its pre-roll.
   - Document the brief re-alignment glitch.
   - Host-test with a synthetic effect whose latency changes.
   - Update `02-lv2-host-PR.md` with the decision.

## M5 — Audio I/O (multichannel input)
1. **Model:** `Track.channels` (mono or stereo) and `InputSource {device, channelL, channelR}`. The `ProjectIO` line is append-only; mono tracks pan as mono sources.
2. **Recorder:**
   - enumerate inputs (`BMediaRoster::GetLiveNodes` or `GetDormantNodes`) and connect to the chosen node, not `GetAudioInput`;
   - capture all channels into a ring and de-interleave each armed track's channels into its own take;
   - record takes as 24-bit or float (a Preferences setting);
   - stop cleanly on disk full.
3. **Output device selection** in Preferences.
4. Kit-free channel-split and deinterleave logic, host-tested. Device enumeration goes on the click list for the Focusrite and HDA on the 192.168.1.186 box.

## M6 — File formats (bundled libraries, no Media Kit)

Never the Media Kit for these — `BMediaFile` and its decoders are forbidden project-wide because the target has no decoder plugins. A review suggested the Media Kit for AIFF/FLAC import; that suggestion stays rejected.

1. **Import:**
   - AIFF/AIFC with our own parser alongside `WavSource`;
   - FLAC with libFLAC;
   - Ogg Vorbis with libvorbisfile;
   - behind an `IAudioSource` factory chosen by sniffing the file, with the drop handler and Import accepting all of them.
   - Optional at build time (`DAW_FLAC`, `DAW_VORBIS`), like `DAW_LV2`.
2. **Export:** FLAC (16 and 24-bit) and Ogg Vorbis (quality setting) in `ExportWindow`. The Exporter gets an `IAudioSink` interface. Host round-trip tests.
3. Recipe: add `lib:libFLAC` and `lib:libvorbis` (plus their devel packages) to REQUIRES and BUILD_REQUIRES. MP3 stays out.

## M7 — Features in scope for 1.0
1. **Sidechain** (package 05): build exactly as specified in `docs/agent-prompts/05-sidechain.md`. Then extend it to LV2 sidechain ports, which are currently left unconnected (`Lv2PortMap.h:105`), and to a sidechain source picker on the insert slot.
2. **Time-stretch** (package 06): build exactly as specified in `docs/agent-prompts/06-timestretch.md` (WSOLA on the disk thread, the same `Stretcher` in the exporter, deterministic seeking). Pitch-shift stays modeled-only.
3. **MIDI expression:**
   - sustain (CC64), pitch-bend and mod-wheel become audible, by giving the synth and sampler per-voice phase integration and a sustain latch;
   - live controller messages reach the engine;
   - offer any CC number in the CC lane.
   A sustain pedal is the baseline for anyone playing piano.
4. **LV2 state and presets:** save plugin state with `lilv_state` into the project (plugins with internal state currently lose it on save, which is a data-loss bug), and load and save presets from the insert slot.

## M8 — Release
1. **Packaging:**
   - fill the recipe checksum from the tag tarball;
   - add `rc` and `xres` to `BUILD_PREREQUIRES`, and make the CMake rule fail instead of silently skipping;
   - add the Deskbar symlink, installed docs and templates, and `lib:` REQUIRES.
   Build the `.hpkg` on the VM and install it on the hardware box.
2. **Documentation:** bring README, USER_GUIDE (screenshots for the new layout), CHANGELOG and ROADMAP up to date, and the package status table in `docs/agent-prompts/README.md`.
3. **RELEASE_CHECKLIST additions:**
   - install from `.hpkg`, Deskbar launch, opening a project by double-click in Tracker;
   - the unsaved-changes prompt;
   - disk full during recording;
   - multichannel record on the Focusrite;
   - FLAC/Ogg export played back in MediaPlayer;
   - sidechain ducking;
   - a stretched loop staying in time;
   - a sustain pedal;
   - a dynamic-latency plugin (4K EQ 2 toggling oversampling while playing);
   - a visual pass of every window at 100% and 150% font size.
4. **Final gates:** hardware pass by Marc on 192.168.1.186, then all suites green (host, ASan, VM `build`/`build-off`, `ui_functional_tests`), then tag `v1.0.0` and set the CHANGELOG date.

## Post-1.0 (explicitly deferred)
LV2 instrument plugins (MIDI atom input, 0-in/2-out), multicore DSP, folder tracks and VCAs, ripple edit, swipe comping, automation write/touch/latch, project archive and cleanup of unused media, an undo history panel, MIDI clock/MTC/Link, control surfaces and MIDI learn, MP3, pitch-shift, a plugin scan cache and blocklist, out-of-process plugin sandboxing.

## Ordering and parallelism
- M0 → M1.1 → M1.2 and M1.3. After those, M1.4–6, M2, M3 and M4 can run in parallel on separate branches; they touch disjoint files once `MainWindow` has been split.
- M5, M6 and M7.1–7.4 are mostly engine, model and DSP work, so they can start right after M0. M7.1 and M7.2 both touch the engine FX/disk loops, so merge them in sequence. M4.6 depends on M4.1.
- M8 comes last.

Each item needs a spec in `docs/agent-prompts/1N-*.md` before work starts, in the format of 05 and 06.

## Verification
- **Per branch:**
  - host `cmake --build build-host` exits 0 and `ctest --test-dir build-host` is all green;
  - the ASan build (`b-asan`) is green;
  - `sh scripts/haiku_syntax_check.sh` reports 0 FAIL;
  - `sh scripts/vm.sh test` passes on both VM build dirs, including `ui_functional_tests`;
  - every new behaviour has a test that was mutation-checked;
  - exact pass counts go in the PR record.
- **New `ui_functional_tests` flows:**
  - dirty, then Quit, gives a prompt;
  - Save As then Save;
  - a missing-media dialog;
  - Edit Cut/Copy/Paste on a range;
  - preferences persisting;
  - single-instance windows;
  - docked editor pane toggling;
  - an engine swap during playback with no `BSoundPlayer` re-create (an engine counter is exposed for tests).
- **Performance check on the VM:** a 32-track, 300-clip project. Play starts in under 300 ms. The timeline draw takes under 4 ms per frame while idle-playing (timed with `system_time()` and logged under a debug flag).
- **Marc's part, on 192.168.1.186:** the extended RELEASE_CHECKLIST, covering sound, looks, packaging and multichannel hardware.
