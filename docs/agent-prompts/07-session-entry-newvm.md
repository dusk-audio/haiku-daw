# Session entry — package 07 (LV2 live editor) on the NEW Haiku VM

You are a coding agent working in `/home/marc/haiku-daw` on a Linux host. The
project is a native DAW for Haiku OS (C++17, CMake). The user is Marc. This file is
your entry point: it tells you what state everything is in, what to do, in what
order, and what you must not do. Read all of it before running anything.

## 1. Read these files, in this order

1. This file.
2. `docs/agent-prompts/07-lv2-live-editor.md` — the task. Complete and current.
   Nothing in it has been started.
3. `docs/agent-prompts/07-lv2-live-editor-HANDOFF.md` — the previous entry prompt
   for this package. Its **phase plan (Phases 0–4), constraints and definition of
   done still apply.** Its "First task: reconcile the VM" section and every VM
   address/recipe in it are **obsolete** — section 3 below replaces them.
4. `docs/agent-prompts/03-inserts-ui-RESUME.md` — read the sections "How the cores
   were read" and "Rules this branch has already paid for" closely; every rule
   there applies to you. Its "Environment, exactly as it works today" section is
   **obsolete** (old VM) — section 3 below replaces it.
5. `docs/agent-prompts/02-lv2-host-PR.md` — only when you need LV2 host internals
   (port mapping, latency latch, param seeding, the Haiku LV2 search-path fix).

## 2. Repo state (verified 2026-10-08)

- Branch `master`, HEAD `73e463b`. Packages 01, 02, 03 are merged. Package 07 is
  unstarted; there is no `feature/lv2-live-editor` branch yet.
- One uncommitted change: `scripts/vm.sh` default IP changed to the new VM
  (`192.168.122.48`). Commit it as the first commit on your branch:
  `chore(scripts): point vm.sh at the new Haiku VM`.
- Host test suite: `cmake -S . -B build-host && cmake --build build-host -j && ctest --test-dir build-host` → 46/46.
- Known defect, not yours: 4K EQ 2's latency moves at runtime (0→27→0); PDC can
  be ~27 samples off. Recorded in `02-lv2-host-PR.md`. Do not try to fix it here.

## 3. The new Haiku VM — this replaces every older VM note

Marc replaced the dev VM with a fresh Haiku install on 2026-10-08. The old VM
(`192.168.122.232`) is gone, along with everything that lived only on it.

**Access**

- libvirt domain `haiku-beta6` (`virsh -c qemu:///system ...`), Haiku R1/beta6
  `hrev59866+90`, x86_64, **2 vCPU / 2 GB RAM** — build with `-j2`, not `-j4`.
- IP `192.168.122.48`, from DHCP. If SSH stops answering, look it up again:
  `virsh -c qemu:///system net-dhcp-leases default` (host name `shredder`), then
  set `HAIKU_VM_IP=<ip>` for `scripts/vm.sh`.
- SSH: `ssh -i ~/.ssh/haiku_vm -o IdentitiesOnly=yes user@192.168.122.48`. Key
  auth works; no password. `sh scripts/vm.sh ssh '<cmd>'` wraps this.
- The host shell is **zsh**, which does not word-split `$OPTS`. When you build
  ssh/scp command lines from variables, run them inside `sh -c '...'`.

**What is installed on it**

- gcc, git, python3 (shipped); `cmake`, `pkgconfig`, `lilv`, `lilv_devel`, `lv2`
  (installed 2026-10-08 with `pkgman install -y`).
- `~/haiku-daw` is a **real git clone** (from a host git bundle), at master
  `73e463b`. `build/` is configured LV2-on, built clean, ctest 46/46.

**What is NOT on it yet — you must restore this before Phase 0**

- **No LV2 plugins with a native UI.** Haiku's system bundles in
  `/boot/system/lib/lv2` are DSP examples with no editor. Package 07 cannot be
  tested without a plugin UI. The old VM had Marc's DAF plugins (4K EQ 2 and
  others) built for Haiku with their GL UIs; those builds are gone.
- **No crash-core tooling.** `~/crashreports/drive.py` and `coremem.py`
  (described in the RESUME doc) are gone.
- **No `build-lv2` or `build-off` dirs.** Only `build`.

**Make it yours (do this first, and record what you did in your PR doc)**

1. Create the extra build dirs on the VM:
   `cmake -B build-off -DDAW_LV2=OFF` and build it; both `build` and `build-off`
   must stay green for the whole package.
2. Rebuild the plugin fixture. Inputs on the Linux host:
   - Marc's plugins: `/home/marc/projects/plugins` (per-plugin
     `plugins/<name>/daf-plugin/`, shared DSP `plugins/shared-daf/`).
   - **DAF** (the framework formerly called DPF; its widgets live in-tree as
     `DAF/widgets/`): `/home/marc/projects/DAF`. There is no `/home/marc/projects/DPF`.
   - The Haiku GL UI port is a DAF patch: `/home/marc/projects/dpf-haiku-gl-ui.patch`
     (apply with `patch -p1` from a **copy** of DAF on the VM — never modify
     Marc's host checkout of DPF or the plugins repo).
   - Build recipe that worked before: copy the plugin's `DafPluginInfo.h` into
     an overlay dir, write a standalone CMakeLists that calls
     `daf_add_plugin(<name> TARGETS lv2 FILES_DSP ... FILES_UI ...)`, and put the
     overlay dir first on the include path. Plugin sources stay untouched.
   - Copy the sources to the VM with `tar cf - <dirs> | ssh ... 'tar xf -'` into
     a scratch dir such as `~/fixtures/`, never into `~/haiku-daw`.
   - Install the resulting `.lv2` bundle into `~/config/non-packaged/lib/lv2/`
     (or set `LV2_PATH`), then confirm the DAW lists it. Start with **4K EQ 2**;
     it is the plugin the user cares about and it has 26 control ports plus
     latency. If you also need a plain control-port UI (no
     `WANT_DIRECT_ACCESS`) for Phase 1, say so and pick one, or build a DPF
     example plugin.
   - **Before spending more than an hour on this, ask Marc** whether the old VM's
     disk image still exists so the fixtures can be copied out instead.
3. Re-create the core-reading helper only when you actually get a crash. The
   method is in the RESUME doc ("How the cores were read"): `Debugger -c --core`
   needs a pty; drive it with a small Python `pty` script.

**Syncing your work to the VM**

The host is authoritative; the VM never commits. For a committed branch,
`sh scripts/vm.sh sync` puts the **current branch** on the VM (`VM_REF=<ref>`
picks another): a commit already on GitHub is fetched there, one that is not is
sent over in a `git bundle`. For uncommitted work, **including new untracked
files** (`git stash create` misses untracked files, which silently breaks the
VM build):
  ```sh
  git add -A && TREE=$(git write-tree) && CT=$(git commit-tree $TREE -p HEAD -m wip) && git reset -q --mixed HEAD
  git update-ref refs/heads/_vmwt $CT && git bundle create /tmp/wt.bundle _vmwt && git update-ref -d refs/heads/_vmwt
  # scp /tmp/wt.bundle, then on the VM: git fetch /tmp/wt.bundle _vmwt && git reset --hard FETCH_HEAD
  ```
  Keep scratch build dirs out of the repo root or `git add -A` sweeps them in.

Then build on the VM and check that the files you changed show up as
`Building CXX object` lines:
`sh scripts/vm.sh ssh 'cd ~/haiku-daw && cmake --build build -j2 2>&1 | tail -20'`.
The VM clock runs about 4 h **ahead** of the host. Because git sets fresh mtimes on
checkout this should not cause make to skip files, but if it does, `touch` them.

**Seeing and driving the VM screen** (GUI testing)

- Screenshot: `virsh -c qemu:///system screenshot haiku-beta6 /path/s.ppm` then
  convert with `magick s.ppm s.png` and read the PNG. Running the `screenshot`
  command over SSH fails (exit 69) — that is the command, not app_server, which
  a process started over SSH DOES reach: `tests/ui_functional_tests.cpp` drives
  the real windows that way (see `09-ui-functional-tests.md`).
- Keyboard works: `virsh -c qemu:///system qemu-monitor-command haiku-beta6 --hmp 'sendkey <key>'`
  (e.g. `ctrl-alt-delete`, `ret`, `shift-a`). A typing helper that maps text to
  sendkey names is easy to write; one `sendkey` per character.
- **QEMU's monitor mouse does NOT reach Haiku.** You cannot click. Anything that
  needs a real click or drag inside a plugin editor (which is most of Phase 0 and
  the "audible" checks) needs Marc at the machine. Prepare the exact steps, ask
  him to do them, and report those paths as **unclicked** until he confirms.
- Launch the app from SSH so it lands on the desktop:
  `DEBUG_SERVER_DISABLE_GUI=1` keeps a crash from parking behind a dialog. Haiku's
  `ps` lists threads, not processes.

## 4. What to do

Branch first: `git checkout -b feature/lv2-live-editor` off master, commit the
`vm.sh` change, then:

1. **VM setup** — section 3 "Make it yours". Done when both VM configs build,
   ctest is green in `build`, and a plugin with a native UI shows up in the DAW.
2. **Phases 0–4** exactly as written in `07-lv2-live-editor-HANDOFF.md`.
   - Phase 0 needs Marc to click (open an LV2 editor, close it while playing and
     while stopped, several times). Ask him; do not mark it verified yourself.
   - **Phase 2 is a decision gate.** Stop and present options A/B/C from the
     spec to Marc before you write any Phase 2 code. Carry the recommendation
     (C) and its reasoning from the HANDOFF; let him choose.
   - Phase 3 changes the engine: write the design into the PR doc first.

## 5. Rules — do not break these

- **Ask before**: pushing anything, merging into master, deleting branches,
  force-resetting anything on the host, installing packages on the host, or
  editing anything outside `/home/marc/haiku-daw` other than scratch copies.
- Commits: on your feature branch only, conventional style
  (`feat(lv2): ...`, `fix(ui): ...`), small and coherent.
- Linux host can only compile the kit-free code (`daw_model`, tests). Anything
  under `src/ui/`, the engine targets, or `src/plugin/` that uses Haiku kits
  only compiles on the VM. **Never claim Haiku code works because the host
  build passed.** Build it on the VM.
- One live parameter channel: `kMsgFxLive` → `Engine::SetFxParamLive`. Do not add
  a second path.
- Every LV2 reference sits behind `#ifdef DAW_HAVE_LV2`; `-DDAW_LV2=OFF` must build.
- Teardown and locking rules from the RESUME doc are non-negotiable: detach views
  before `cleanup()`, never `dlclose` a UI library, lock order window → world,
  every lilv call under `gWorldMutex`, never render from `BView::Draw()`.
- Editors post messages to MainWindow; commands own the model. No direct model
  mutation from editor code.
- Report honestly. If a test fails, paste the output. If you could not click
  something, say "unclicked". If you guessed, say so. Mutation-test key new
  assertions (break the code, watch the test fail, revert).

## 6. Definition of done

- VM restored: `build` and `build-off` green on the VM, host `build-host` 46/46
  plus whatever tests you add, plugin fixture installed and documented.
- Everything under "Definition of done" in `07-lv2-live-editor.md` and
  `07-lv2-live-editor-HANDOFF.md`.
- `docs/agent-prompts/07-lv2-live-editor-PR.md` written: decisions (including
  Marc's Phase 2 choice), how the fixture was rebuilt, test coverage, and an
  explicit list of what was verified by clicking vs what remains unclicked.
- Update the status table in `docs/agent-prompts/README.md` and replace the
  obsolete "Environment" section of `03-inserts-ui-RESUME.md` with a pointer to
  section 3 of this file.

When you finish a phase or hit a decision gate, stop and give Marc a short status:
what changed, what is verified, what needs him.
