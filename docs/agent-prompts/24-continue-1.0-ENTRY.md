# Session entry — continue Haiku DAW toward 1.0 (M1.4 onward)

You are a coding agent working in `/home/marc/haiku-daw` on a Linux host (zsh).
The project is a native digital audio workstation for Haiku OS (C++17, CMake).
The owner is Marc. Your job is to carry the 1.0 plan forward, one task at a
time, in the order of section 5.

Read this whole file before running anything. It supersedes
`08-release-1.0-ENTRY.md` (whose task list is finished). When this file and an
older doc disagree, this file wins.

---

## 1. Read these, in this order

1. This file.
2. `AGENTS.md` (repo root) — the non-negotiables, one page.
3. **`docs/UI_GUIDELINES.md`** — before any change under `src/ui/`. Its first
   rule is the reason this file exists in its current form: a whole UI
   milestone merged with every test green and the main window visibly broken.
   You review screenshots of every window you change before you call it done.
4. `docs/PLAN_1.0.md` — the 1.0 plan (milestones M0–M8) and its verification
   bar. Your tasks are its remaining items.
5. `docs/agent-prompts/README.md` — the status table (what is merged, with a
   record per item) and the rules every package follows.
6. `docs/HANDOFF.md` — background, architecture in one breath, the dev loop,
   hard-won lessons, the `Frame` vs `BView::Frame()` gotcha. Its "M4 state"
   section describes the UI before the Layout Kit (one window, no menu bar):
   historical — the code is the truth.
7. `docs/ARCHITECTURE.md` — the design of record.
8. When a task needs it: the item's existing spec/record in
   `docs/agent-prompts/`, `docs/PRODUCTION_HANDOFF.md` (hard constraints).

## 2. State of the project (verified 2026-10-10, `master` at `14c3fea`)

- `master` is the integration branch (not `main`). Working tree clean. GitHub
  remote `origin` = `dusk-audio/haiku-daw` (public); master is pushed.
- Suites, all green on that commit: host `build-host` 52/52, VM `build`
  54/54 (includes `ui_functional_tests`, 210 checks), VM `build-off`
  (`-DDAW_LV2=OFF`) 50/50.
- **Done** (do not rebuild; records in `docs/agent-prompts/`): packages 01–04
  and 07, R1–R5, and from the plan M0.1–M0.7 (unsaved changes, file menu,
  unique takes, visible errors, undo gaps, Tracker integration, redraw +
  focus), M1.1 (MainWindow split), M1.2 (theme + `Themed()` metrics), M1.3
  (widget kit + `DawControlLook`), M1.4 (Layout Kit dialogs, inspector/timeline
  split, docked MIDI editor with Pop out, single-instance mixer and plugin
  browser) and the M1.4 visual pass of 2026-10-10.
- **Partly done:**
  - M1.4 — the sample and plugin browsers are not docked; the effects window
    is per-track, not single-instance. → task T2.
  - M1.5 offscreen drawing — measured, not built: play start on 32 tracks /
    320 clips is 3.1 s (budget 300 ms — that is M4.1's to fix); the timeline
    draw time could not be measured because the VM's screen was locked. See
    `23-offscreen-and-icons.md`. Marc decided: measure first (§3, T11).
  - M1.6 icons — vector toolbar/transport glyphs done (`widgets/DawIcons.h`);
    no HVIF app icon or project-file icon. Marc decided: Haiku-style HVIF
    (§3, T12).
- **Not started:** M2, M3, M4, M5, M6, M7, M8.
- Known small UI leftovers from the visual pass (fold them into T2): the
  generic parameter panel's sliders draw no handle at value 0, and its window
  title is the plugin name alone (an LV2 editor's names the track too).

## 3. Decisions that belong to Marc — never choose them yourself

**Decided 2026-10-10, by Marc:**
1. **The app never changes the OS.** No `set_ui_color` or any other
   system-wide setting. **Look:** the native Haiku look by default (system
   colours, stock control look), with an **optional dark mode** in Preferences
   that styles the DAW's own windows only. The old always-dark theme is not
   the default. → T1.
2. **M1.5 offscreen drawing: measure first.** Time the timeline's drawing on
   the hardware box with the 32-track/320-clip project
   (`DAW_TIMELINE_TIMING=1`); build the back-buffer only if a frame misses the
   4 ms budget. → T11.
3. **M1.6 icons follow the BeOS/Haiku icon set:** HVIF vector icons drawn to
   Haiku's icon guidelines (the system set's perspective, top-left lighting,
   outline and palette) — app icon and project-file icon. → T12.

**Still Marc's, if they come up:** anything that would change 1.0 scope, the
version string, the license (MIT) or distribution (HaikuPorts recipe) — all
already decided; do not reopen them. New questions of that weight: ask, then
record the answer here.

Already decided (2026-10-09, recorded in `08-release-1.0-ENTRY.md` §2): the
1.0 tag is held until M0–M8 land; sidechain, multichannel input, FLAC/Ogg
export and time-stretch are in 1.0; MP3 is out; dynamic LV2 latency is handled
by re-solving PDC when a plugin's latency changes.

## 4. Environment

**Host (Linux, where you work)**
- Kit-free build + tests:
  `cmake -S . -B build-host && cmake --build build-host -j8 && ctest --test-dir build-host`.
- Sanitizers: `cmake -B b-asan -DDAW_SANITIZE=ON && cmake --build b-asan -j8 && ctest --test-dir b-asan`.
- Haiku-only code (`src/ui/`, `src/engine/Engine.cpp`, `Recorder.cpp`,
  `src/plugin/PluginHost.cpp`, anything using the Media/Interface/Midi Kit)
  does not compile on the host. Cross-check it with
  `sh scripts/haiku_syntax_check.sh` (0 FAIL) before every commit that touches
  it, then build it on the VM.
- **Check the build's exit code, not only ctest** — a failed build leaves old
  binaries and ctest still passes.
- zsh does not word-split `$VAR`; wrap ssh/scp lines built from variables in
  `sh -c '...'`.

**Haiku VM (builds, functional tests, screenshots)**
- libvirt domain `haiku-beta6`, R1/beta6, 2 vCPU / 2 GB, `192.168.122.48`
  (if it moves: `virsh -c qemu:///system net-dhcp-leases default`).
- `sh scripts/vm.sh ssh '<cmd>'`; `sh scripts/vm.sh build` / `test` sync the
  host's current branch to the VM (fetched from GitHub when pushed, a git
  bundle otherwise) and build/test the `build` dir. Use `VM_REF=<ref>` for
  another ref; the recipe for an **uncommitted** tree is in `docs/HANDOFF.md`
  (dev loop).
- Two build dirs must stay green: `build` (LV2 on) and `build-off`:
  `sh scripts/vm.sh ssh 'cd ~/haiku-daw && cmake -B build-off -DDAW_LV2=OFF && cmake --build build-off -j2 && ctest --test-dir build-off'`.
- **Screenshots:** `DAW_UI_SHOTS=/tmp/shots ./ui_functional_tests` in
  `~/haiku-daw/build`, then `scp -i ~/.ssh/haiku_vm 'user@192.168.122.48:/tmp/shots/*' <dir>/`.
  The VM screen must be unlocked (a locked screen draws nothing). Full
  procedure: `docs/UI_GUIDELINES.md` §1.
- Before any run: `rm -f ~/config/settings/HaikuDAW/recovery.dawproj` on the
  VM — a killed dirty run leaves it and the next run hangs on a modal alert.
- Nothing is audible on the VM. LV2 fixtures installed: 4K EQ 2
  (direct-access UI) and DAF `examples/Parameters` (control-port UI).

**Real Haiku hardware (the only machine with audio and the Focusrite)**
- `192.168.1.186`, `ssh -i ~/.ssh/haiku_vm user@192.168.1.186`, repo at
  `~/haiku-daw`. **Ask Marc before using it** — it is his physical machine.

## 5. Ordered task list

Each task: its own branch off the latest `master` (`feature/<slug>` or
`fix/<slug>`). Before coding, write the item's spec as
`docs/agent-prompts/NN-<slug>.md` (next free number, from 25), in the format
of `05-sidechain.md`: codebase orientation, goal, work items, definition of
done. Keep the record `NN-<slug>-PR.md` as you go. Push the branch and report;
**do not merge to master unless Marc says so in the session.**

**T1 — Theme: leave the OS alone; native look by default, optional dark
mode.** (`feature/theme-modes`) Decided by Marc 2026-10-10 (§3). Today
`ApplyThemeColors()` in `src/main.cpp` calls `set_ui_color`, which rewrites the
**system-wide** colours — Haiku saves them, so every app and Tracker stay
recoloured after the DAW quits. That is a bug, and the dark look it applied is
not the default Marc wants.
- Remove every `set_ui_color` call. The app never changes system settings.
- A theme mode in `AppSettings` (kit-free, round-trip host test): **System**
  (default) and **Dark**. A View menu item toggles it until M3.5's Preferences
  window exists, then it moves there.
- **System mode looks like a Haiku app:** colour tokens in `Theme.h` derive
  from `ui_color()` (panel, document, control, text, navigation/accent colours)
  with `tint_color` for the variants (alternate lanes, grid, headers); the
  stock `be_control_look` draws stock controls; kit widgets (`DawButton`,
  `DawSlider`, `DawKnob`, …) draw through `be_control_look` so they match
  Haiku's own. The custom-drawn surfaces (timeline, piano roll, meters,
  inspector, transport bar) use the tokens and must read well on a light
  panel colour — clips, waveforms, meters and the playhead keep their contrast.
- **Dark mode** is today's palette and `DawControlLook`, applied to the DAW's
  own windows only (`be_control_look` and view colours are per-process, so
  this never leaks into other apps). File panels and alerts are the DAW's
  windows too — they follow the mode.
- Follow the user's Appearance changes live in System mode
  (`B_COLORS_UPDATED` → re-derive tokens, reset view colours, invalidate).
  Switching mode live is preferred; if it needs a restart, say so in the menu
  item and the record.
- Kit-free palette derivation (base colours in, tokens out) with a host test
  that text/background contrast is at least 4.5:1 in both modes and for a
  dark and a light system panel colour.
- The screenshot pass runs **in both modes** (`ui_functional_tests` takes the
  mode from an env var or a test hook) and every window is reviewed in both.
- Tell Marc in the record how to restore system colours already overwritten
  (Preferences > Appearance > Colors > Defaults); do not do it for him.

**T2 — Finish M1.4: docked browsers, single-instance effects window.**
(`feature/dock-browsers`) The dock (`MainWindow::fDock`) is a header strip
(`PaneHeader`) over a body that holds the MIDI editor or an empty-state hint.
Give it pages — Editor, Samples, Plugins — switched from the header strip
(a segmented control in the strip, not a `BTabView`: see UI_GUIDELINES §4 on
tabs). `SampleBrowser` and `PluginBrowser` build into their own windows today;
make their content a view that can live in the dock or in a window, keeping
every message they post unchanged. The effects window becomes single-instance
(show and activate the existing one). Fold in the two leftovers from §2.
`ui_functional_tests` grows checks for the page switch and the
single-instance window, and `Shot()`s of each page.

**T3 — M7.4 LV2 state and presets** — moved ahead of the polish milestones
because it is **data loss**: a plugin with internal state loses it on save.
Save state with `lilv_state` into the project (append-only `ProjectIO` line,
compat test), restore on load; load/save presets from the insert slot.

**T4 — M2 Arrange window feel**, one branch per item, in the plan's order
(2.1 pointer feedback → 2.7 markers). 2.5's model/IO/command changes are
host-tested with append-only `ProjectIO` lines and compat tests.

**T5 — M4 Engine responsiveness.** 4.1 first (graph built off the window
thread, atomic swap, `BSoundPlayer` kept alive — this is what fixes the 3.1 s
play start; the 32-track/320-clip bound in `TestBigProjectPlayback` tightens
toward 300 ms as it lands), then 4.2–4.5; 4.6 (dynamic LV2 latency) only after
4.1. RT rules in §7 are absolute here.

**T6 — M3 Transport, meters, mixer, preferences**, items 3.1–3.7.

**T7 — M7.1 sidechain (package 05), then M7.2 time-stretch (package 06)** —
specs already exist (`05-sidechain.md`, `06-timestretch.md`); both touch the
engine's FX/disk loops, so the second branches from master after the first
merges.

**T8 — M5 multichannel input**, **T9 — M6 FLAC/Ogg import + export**,
**T10 — M7.3 MIDI expression.** Device enumeration (M5) goes on the hardware
click list for the Focusrite and the HDA.

**T11 — M1.5 measurement (Marc-gated, hardware box).** Ask Marc first. Run
the big-project playback with `DAW_TIMELINE_TIMING=1` on 192.168.1.186, record
the per-frame draw times in `23-offscreen-and-icons.md`, and report. Build the
back-buffer only if Marc agrees after seeing the numbers.

**T12 — M1.6 icons in the Haiku style.** Draw the app icon and the
`.dawproj` file icon as SVG following Haiku's icon guidelines (study the
system set: `/boot/system/data/artwork/` and the HVIF icons of the stock apps;
keep to the shapes and gradients HVIF can express). The SVG → HVIF step is
Icon-O-Matic (import SVG, export "HVIF RDef source"): Marc does it on a Haiku
machine unless you find a way to drive it; then the bytes go into
`haiku-daw.rdef.in`. Check both icons at 16, 32 and 64 px in Tracker and
Deskbar screenshots.

**T13 — M8 release.** Marc-gated: packaging, docs, the extended
RELEASE_CHECKLIST, his hardware pass, then the tag.

Marc may reorder this list; his order wins.

## 6. How to work

1. **One task at a time.** Spec → implement → verify → record → push → report.
2. **Read before you write.** Match the surrounding code's style, naming,
   comment density and idioms; look for an existing helper first.
3. **Kit-free first.** Logic goes in `src/model/`, `src/dsp/` or header-only
   helpers with host tests; Haiku-only code stays thin wiring.
4. **Every fix and feature ships a test**; **mutation-check it** (break the
   code it guards, watch it fail, restore) and name the mutation in the commit
   body.
5. **GUI flows are automated** in `tests/ui_functional_tests.cpp` by posting
   what the widgets post (`09-ui-functional-tests.md`). A model change made
   under the window lock is followed by `kMsgUiRefresh`.
6. **Look at the screen** (`docs/UI_GUIDELINES.md`): any change to what a
   window draws or how it is laid out gets a `Shot()`, a screenshot run on the
   VM, and every shot of it opened and checked against the guidelines'
   checklist — at 150% too when geometry changes. Fix what you see before
   calling it done, and list the shots you reviewed in the record. A click
   list for Marc is only for what needs ears or the hardware box.
7. **Verification before every commit**, all green, counts in the record: host
   build exit 0 → host ctest → ASan → `haiku_syntax_check.sh` 0 FAIL → VM
   `build` and `build-off` ctest → the screenshot pass for UI changes.
8. **Never claim more than you verified.** "Compiles; needs the hardware pass"
   is a fine status; "works" without evidence is not.
9. **Stop and ask Marc** when: a §3 decision comes up; a test you did not write
   fails and two attempts do not explain it; a change would touch the RT audio
   path in a way §7 does not clearly allow; or the task grows past its spec.

## 7. Hard rules — never break these

- **Audio callback** (`Engine::FillBuffer` and everything it calls): no
  allocation, locks, file I/O, syscalls or logging — arithmetic on
  preallocated memory, lock-free rings and atomics only.
- **Never `BMediaFile`** or Media Kit decoders; formats are our own code or a
  bundled library.
- **Every model mutation goes through `CommandStack`.** Drags preview, then
  push one command on mouse-up.
- `using Frame = daw::Frame;` in any `BView`/`BWindow` subclass that uses
  model frames.
- **Commits:** author `Marc Korte <marc@duskaudio.com>`, conventional-commit
  subjects, a body that says why. **No `Co-Authored-By:` or any other
  AI-attribution trailer** in commits or PR text.
- Never commit to or merge into `master`, rewrite history, force-push or
  delete branches unless Marc asks in the session.
- Never modify checkouts outside this repo (`/home/marc/projects/DAF`,
  `/home/marc/projects/plugins`); work on copies.
- Do not touch the VM's libvirt network config.

## 8. Your first actions

1. Read the files in §1.
2. Run the host suite and confirm the count in §2; check
   `sh scripts/vm.sh ssh 'uname -a'` answers and the VM screen is unlocked
   (a `virsh -c qemu:///system screenshot haiku-beta6 x.ppm` shows a desktop,
   not a lock screen).
3. Write the T1 spec and start T1 on `feature/theme-modes` — every later UI
   task is reviewed in both modes, so the modes come first.
