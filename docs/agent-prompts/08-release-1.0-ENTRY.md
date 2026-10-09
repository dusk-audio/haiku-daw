# Session entry — push Haiku DAW toward a 1.0 release

You are a coding agent working in `/home/marc/haiku-daw` on a Linux host (zsh).
The project is a native digital audio workstation for Haiku OS (C++17, CMake).
The owner is Marc. Your job is to move the project toward a shippable 1.0 by
working through the ordered task list in section 5, one task at a time.

Read this entire file before running anything. When this file and an older doc
disagree, this file wins (section 3 lists the known stale docs).

---

## 1. Read these, in this order

1. This file.
2. `docs/HANDOFF.md` — project background, architecture in one breath, the
   threading rule, the `Frame` vs `BView::Frame()` gotcha. Its VM and
   "SSH abandoned" sections are obsolete (see section 3).
3. `docs/ARCHITECTURE.md` — design of record.
4. `docs/PRODUCTION_HANDOFF.md` — the audit, what is done, hard constraints.
5. `docs/agent-prompts/README.md` — the feature-package status table.
6. `docs/agent-prompts/03-inserts-ui-RESUME.md`, sections "How the cores were
   read" and "Rules this branch has already paid for". Every rule there applies.
7. Only when your task needs it: `docs/ROADMAP.md`, the package specs in
   `docs/agent-prompts/0N-*.md`, `docs/agent-prompts/07-lv2-live-editor-PR.md`.

## 2. State of the project (verified 2026-10-09)

**Branches**
- `master` — integration branch (note: `master`, not `main`). Packages 01
  (fx-inserts-core), 02 (lv2-host), 03 (inserts-ui) are merged.
- `feature/lv2-live-editor` — package 07, **32 commits ahead of master, not
  merged**. All four phases are written, survived three adversarial review
  rounds, and are green: host `build-host` 48/48, VM `build` 49/49, VM
  `build-off` (`-DDAW_LV2=OFF`) 45/45. **The one thing left is Marc
  click-testing the live paths** (list in `07-lv2-live-editor-PR.md`, table
  "What is verified", rows marked NOT YET). It merges to master only after that.
- Other `feature/*` branches are all already merged; ignore them.
- Working tree clean.

**What is done** (do not rebuild): engine with RT-safe callback; routing graph
(tracks → buses → master, sends, monitor dim/mono); BS.1770 metering; tempo/meter
map; gain/pan/fx-param automation; full editing (split, crossfade, multi-select,
region ops, freeze); recording (overdub, count-in, punch, loop-record + takes,
input monitoring); MIDI (synth + SFZ/SF2 sampler, piano roll, CC lanes, SMF
import/export, external Midi Kit 2 input with per-track assignment + demux,
hardware-verified with one keyboard); sample browser (BFS attrs + BQuery);
autosave + crash recovery; export (stems, 16/24/32-bit + TPDF dither, loudness
normalization, true-peak limiter — **API only, see below**); PDC (offline + RT);
LV2 hosting with insert slots, bypass/mix, plugin browser, native plugin editors.

**Known gaps between here and a releasable 1.0**
1. **Export UI**: `src/ui/MainWindow.cpp:527` calls
   `ExportWav(*fProject, path.Path(), fProject->sampleRate)` — hard-coded 16-bit,
   no normalization/limiter, no format choice. The `bitDepth` and
   `ExportNormalize` options exist and are host-tested but no user can reach
   them. Export also runs on the UI looper (window freezes, no cancel).
2. **Never listened to**: many engine changes are cross-compiled and VM-built but
   never verified audibly (P0 #9 fader/mute race fix, P0 #10 monitor/live-MIDI
   teardown, MIDI CC gain/pan/smoothing, RT PDC with the LookaheadLimiter,
   underrun resync, xrun pad). The VM has no usable audio; only the real Haiku
   box (section 4) can prove them.
3. **Autosave during recording** (`MainWindow.cpp`, P1 minor).
4. **Round-trip latency**: `MainWindow::fRoundTripFrames` is always 0 — the
   Media-Kit device-latency query was never written; punch/loop-record paths
   don't compensate.
5. **Multichannel audio input**: `Recorder.cpp` always opens the system default
   input (`BMediaRoster::GetAudioInput`); `InputSource.channel` is modeled and
   serialized but ignored by capture.
6. **Release engineering does not exist**: no top-level `README`, no `LICENSE`,
   no version number in `project()`, no About box, no HaikuPorts recipe / `.hpkg`,
   no `CHANGELOG`, no user documentation.
7. Unstarted feature packages: 04 midi-tools (spec + entry prompt ready,
   host-testable), 05 sidechain (blocked until 07 merges — shares the engine FX
   loop), 06 timestretch.

**Open decisions that belong to Marc — never choose them yourself** (ask, then
record the answer in the relevant doc):
- What is in 1.0 vs post-1.0 (section 5 is a *proposal*).
- FLAC / Ogg / MP3 export in 1.0? (needs a bundled library; MP3 has licensing.)
- Multichannel input (gap 5) in 1.0?
- Dynamic LV2 latency policy (4K EQ 2 reports 0→27→0; `IEffect::LatencySamples()`
  must be constant). Options are in `02-lv2-host-PR.md`. Do not pick one.
- License and distribution channel (HaikuPorts vs. a standalone `.hpkg`).
- The version string (1.0.0 vs a 0.9 beta first).

**Recorded 2026-10-09, by Marc (the answers above, verbatim in substance):**
- 1.0 scope: **confirmed as proposed** in section 5 — R1–R4 merged to master,
  R5 verified on the real hardware. The post-1.0 list stays post-1.0.
- Version string: **1.0.0**.
- License: **MIT**.
- Distribution: **HaikuPorts recipe** (`haiku-daw-<ver>.recipe`).
- Still open, no decision taken: FLAC/Ogg/MP3 export in 1.0 (default until
  answered: WAV only); multichannel input in 1.0 (default: post-1.0);
  dynamic-latency policy (do not pick; the option list now lives in
  `02-lv2-host-PR.md` §"What is still NOT verified").

## 3. Stale documentation — trust this section over these

- `docs/HANDOFF.md`: "SSH-into-the-VM was abandoned", the `192.168.1.230:8000`
  HTTP pull loop, the hrev57937 VM, and the test counts are all obsolete.
- `docs/ROADMAP.md`: Phase I ("Plugins (long-term)") and "Deliberately skipped:
  full plugin hosting" are obsolete — LV2 hosting shipped in packages 02/03/07.
  Phase X/Y/Z "Remaining" lists are still accurate.
- `docs/agent-prompts/07-session-entry-newvm.md` mentions `DPF`/`DPF-Widgets`;
  those are now `/home/marc/projects/DAF` (see `07-lv2-live-editor-PR.md`).
- `scripts/vm.sh` builds with `-j4`; the VM has 2 vCPU / 2 GB. Use `-j2`.

Fixing these docs is a legitimate small task (5.R4) — do it in a `docs:` commit.

## 4. Environment

**Host (Linux, where you work)**
- Kit-free build + tests:
  `cmake -S . -B build-host && cmake --build build-host -j8 && ctest --test-dir build-host`
  → 48/48 on the live-editor branch today.
- Sanitizers: `cmake -B b-asan -DDAW_SANITIZE=ON && cmake --build b-asan -j8 && ctest --test-dir b-asan`.
- Haiku-only code (`src/ui/`, `src/engine/Engine.cpp`, `Recorder.cpp`,
  `src/plugin/PluginHost.cpp`, anything using the Media/Interface/Midi Kit)
  **cannot compile on the host**. Cross-compile check it with
  `sh scripts/haiku_syntax_check.sh` (must report 0 FAIL) before every commit
  that touches it.
- **Check the build's exit code, not only ctest.** A failed build leaves old
  binaries in place and ctest still reports pass.
- zsh does not word-split `$VAR`. When building ssh/scp command lines from
  variables, wrap them in `sh -c '...'`.

**Haiku VM (builds + headless tests)**
- libvirt domain `haiku-beta6`, Haiku R1/beta6, 2 vCPU / 2 GB, IP
  `192.168.122.48` (if it moves: `virsh -c qemu:///system net-dhcp-leases default`).
- `ssh -i ~/.ssh/haiku_vm -o IdentitiesOnly=yes user@192.168.122.48`, or
  `sh scripts/vm.sh ssh '<cmd>'` / `sh scripts/vm.sh test`.
- No git remote exists. Code reaches the VM via `git bundle` + `scp` (see
  `scripts/vm.sh sync`).
- Build dirs on the VM: `build` (LV2 on) and `build-off` (`-DDAW_LV2=OFF`). Both
  must stay green.
- **`app_server` is not reachable from SSH.** No test can create a `BWindow`,
  and nothing is audible. You cannot click-test. GUI and audio behaviour is
  verified by Marc; your job is to write him a precise click list (section 6).
- Plugin fixtures installed: 4K EQ 2 (direct-access UI) and DAF
  `examples/Parameters` (control-port UI) in `~/config/non-packaged/lib/lv2/`.

**Real Haiku hardware (the only machine with audio)**
- `192.168.1.186`, `ssh -i ~/.ssh/haiku_vm user@192.168.1.186`, repo at
  `~/haiku-daw`, HDA audio, 12 cores. Older Haiku revision than the VM.
- Code transfer: `git bundle` + `scp`. `scripts/serve.sh` serves on port 9090
  (port 8000 is firewalled and *hangs*, it does not refuse).
- Ask Marc before using it — it is his physical machine.

## 5. Ordered task list (proposal — confirm scope with Marc first)

Before starting task R1, post the "Open decisions" list from section 2 to Marc
and ask him to confirm or edit the 1.0 scope below. Work on whatever he does not
change while waiting.

Each task gets its own branch off `master` named `feature/<slug>` (or
`fix/<slug>`), except R0.

**R0 — Land package 07 (Marc-gated).** Do not merge it yourself. Prepare a
click-test checklist from the NOT YET rows of `07-lv2-live-editor-PR.md` and give
it to Marc. When he reports results: fix any failure on
`feature/lv2-live-editor` with a regression test; when he says merge, merge to
master with `--no-ff` and update the status table in
`docs/agent-prompts/README.md`. R1 must branch from master *after* this merge if
it touches `MainWindow.cpp` (it does) — if R0 is still waiting, do R2 first.

**R1 — Export dialog + worker-thread export.** (`feature/export-dialog`)
- A native export window: format (WAV only unless Marc approves FLAC), sample
  rate, bit depth 16/24/32-float, dither on/off for 16-bit, loudness
  normalization {on, target LUFS, true-peak ceiling dBTP, limiter on/off},
  range (whole project / loop range), and stems vs. mixdown. Remember the last
  choice in `AppSettings` (kit-free, host-test the new fields round-trip).
- Run `ExportWav` / `ExportStems` on a worker thread with a progress bar and a
  Cancel button. The exporter is kit-free: add a progress callback + a
  cancellation flag (`std::atomic<bool>`) to it, host-test that cancel stops
  early and leaves no partial file (write to temp, rename on success), and that
  progress is monotonic and reaches 1.0.
- Existing paths that flush pending LV2 editor gestures before export (package
  07) must still run before the worker starts.
- Same for freeze (`MainWindow.cpp` around line 2597) if it is cheap; otherwise
  leave it and say so.

**R2 — Package 04, MIDI tools.** Spec: `docs/agent-prompts/04-midi-tools.md`,
entry: `04-midi-tools-HANDOFF.md`. Almost entirely host-testable. Branch
`feature/midi-tools`. Can run before R0 lands.

**R3 — Stability leftovers.**
- Autosave during recording: either make it safe (the take's WAV is still being
  written — the recovery file must not reference a half-written take, or must
  reference it in a way load can survive) or explicitly suspend autosave while
  recording and resume after. Pick the simplest correct option, document it.
- Media-Kit device-latency query feeding `fRoundTripFrames`
  (`BMediaRoster::GetLatencyFor` on the input/output nodes), plus round-trip
  compensation in the punch and loop-record take paths. The math is already in
  `RecordPlan::CompensateRoundTrip` (host-tested); this is wiring.
- `scripts/vm.sh`: `-j4` → `-j2`.

**R4 — Release engineering.** (`chore/release-1.0`)
- `project(haiku_daw VERSION x.y.z CXX)` with the version Marc picks; generate a
  `Version.h` from CMake; show it in a Help ▸ About window and in
  `--version` / stderr at startup.
- Top-level `README.md` (what it is, requirements: Haiku R1/beta5+, optional
  lilv/lv2 for plugins, build instructions, known limitations) and
  `CHANGELOG.md` for 1.0 built from `git log master` and `docs/ROADMAP.md`.
- `LICENSE`: **ask Marc which license**; do not pick one.
- A HaikuPorts-style recipe (`haiku-daw-<ver>.recipe`) or an `.hpkg` build
  script, per Marc's distribution decision. Include a `.rdef` with the app
  signature, version, and icon placeholder if one is not already present.
- A short user guide (`docs/USER_GUIDE.md`): the transport, keyboard shortcuts
  (the in-app shortcuts help is the source), recording, MIDI input assignment,
  inserts/LV2, export. Describe only behaviour you can find in the code.
- Fix the stale docs in section 3.

**R5 — Release verification pass (with Marc, on the real hardware).** Write
`docs/RELEASE_CHECKLIST.md`: every never-listened item from gap 2, PRODUCTION
item 1, and the package 07 click list, each as one line with the exact steps and
the expected result. Marc runs it on 192.168.1.186; you fix what fails, each
with a regression test where one is possible.

**Post-1.0 unless Marc says otherwise:** 05 sidechain, 06 timestretch,
FLAC/Ogg/MP3, multichannel input, dynamic-latency policy, pitch-bend/mod-wheel
synthesis, live CC recording, MIDI clock.

## 6. How to work

1. **One task at a time.** Finish, verify, commit, report — then the next.
2. **Read before you write.** Before editing a file, read the surrounding code
   and match its style, naming, comment density and idioms. Search for an
   existing helper before writing a new one.
3. **Kit-free first.** Put logic in kit-free code (`src/model/`, `src/dsp/`,
   header-only helpers) where a host test can reach it. Keep Haiku-only code to
   thin wiring. This is how the project stays testable without a GUI.
4. **Every fix and every feature ships a test.** Tests are standalone
   `int main()` + `CHECK` files, one `add_executable` + `add_test` each in
   `CMakeLists.txt`, linked against `daw_model`. Deterministic only (fixed RNG
   seeds, no wall clock).
5. **Mutation-check your tests.** After a test passes, break the code it guards
   (delete the fix, flip a condition) and confirm the test fails, then restore.
   Say in the commit body which mutation you tried. A test that passes against
   broken code is worse than none.
6. **Verification before every commit**, in this order, all green:
   host build exit code 0 → host ctest → `haiku_syntax_check.sh` 0 FAIL (if any
   Haiku-only file changed) → VM `build` and `build-off` + ctest (if engine/UI
   changed). Report the exact counts.
7. **GUI and audio cannot be verified by you.** For every behaviour that needs
   a window or ears, add a line to the task's click list: numbered steps, the
   expected result, and what a failure looks like. Never write that something
   "works" when only the compile proved it — say "compiles; click-test pending".
8. **Keep a record.** For each task, keep a `docs/agent-prompts/08-<slug>-PR.md`
   written as you go: what changed, why, what is verified and how, what is not,
   what you judged not worth fixing and why.
9. **Stop and ask Marc** when: a decision in section 2's list comes up; a test
   you did not write fails and you cannot see why within two attempts; a fix
   would need to touch the RT audio path in a way not covered below; or the
   task's scope grows past its description.

## 7. Hard rules — never break these

- **Audio callback (`Engine::FillBuffer` and everything it calls):** no
  allocation, no locks, no file I/O, no syscalls, no logging. Only arithmetic on
  preallocated memory, lock-free rings and atomics. Disk threads do I/O; the UI
  thread hands intents to the engine and never writes engine memory.
- **Never use `BMediaFile`** or Media Kit decoders — the target has no decoder
  plugins. File formats are parsed/written by our own code or a bundled library.
- **All model mutations go through `CommandStack`** (undoable). Drags preview,
  then push one command on mouse-up.
- **Inside any `BView`/`BWindow` subclass** that uses model frames, declare
  `using Frame = daw::Frame;`.
- **Commits:** author `Marc Korte <marc@duskaudio.com>` (already the configured
  identity). Conventional-commit subjects (`feat(export): ...`,
  `fix(engine): ...`, `docs: ...`, `test(...)`, `chore(...)`). **No
  `Co-Authored-By` or any AI-attribution trailer** — `docs/HANDOFF.md` forbids it.
  Body explains *why*, not just what.
- **Never** commit directly to `master`, merge to `master`, rewrite history,
  force anything, or delete branches unless Marc asks in this session. There is
  no remote; do not add one.
- **Never modify** Marc's checkouts outside this repo (`/home/marc/projects/DAF`,
  `/home/marc/projects/plugins`). Work on copies.
- Do not touch the VM's libvirt network config.
- Do not "fix" the dynamic-latency behaviour or pick any open decision.

## 8. Definition of done for 1.0 (proposal — Marc confirms)

- Package 07 merged; R1–R4 merged to master; every host, ASan, VM `build` and
  VM `build-off` suite green with counts recorded.
- Every item in `docs/RELEASE_CHECKLIST.md` passed by Marc on real hardware.
- A user can install the package, open the app from Deskbar, record, edit,
  mix with built-in and LV2 effects, and export a mastered WAV of their chosen
  bit depth without reading the source.
- README, LICENSE, CHANGELOG, USER_GUIDE present and accurate; About shows the
  version.

## 9. Your first actions

1. Read the files in section 1.
2. Run the host suite and confirm 48/48 on `feature/lv2-live-editor`; check
   `sh scripts/vm.sh ssh 'uname -a'` answers.
3. Send Marc, in one message: the open-decision list (section 2), the proposed
   scope (section 5), and the package 07 click-test checklist (R0).
4. While he answers, start R2 (package 04) on `feature/midi-tools` off `master`.
