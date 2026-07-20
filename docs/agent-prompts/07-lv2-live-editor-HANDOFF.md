# Session entry — merge package 03, then package 07 (LV2 live editor)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. This file is the entry prompt for the session; it tells you where the repo actually stands and what to do first. Read in this order:

1. This file.
2. `07-lv2-live-editor.md` — the task itself. It is complete and current; nothing in it has been started.
3. `03-inserts-ui-RESUME.md` — **still the authoritative environment guide**: VM sync/build recipe, clock-skew workaround, crash-core tooling, and the "rules this branch has already paid for" list. Every one of those rules still applies; the teardown-order and lock-order rules are the ones package 07 is most likely to trip.
4. `02-lv2-host-PR.md` — only when you need LV2 host internals (port mapping, latency latch, param seeding).

## State of the repo (verified 2026-07-20)

- Packages 01 (fx-inserts-core) and 02 (lv2-host) are **merged into master**. Master HEAD `408607b`.
- Package 03 (inserts-ui) is **complete and signed off by the user**, sitting on branch `feature/inserts-ui`: 38 commits, HEAD `242a633`, working tree clean. Green everywhere it can run: Linux host 46/46 (plain + `-DDAW_SANITIZE=ON`), `-DDAW_LV2=OFF` 43/43, Haiku VM 46/46 and 43/43, zero warnings. **Not merged.**
- Packages 04 (midi-tools), 05 (sidechain), 06 (timestretch): unstarted, task docs in this directory, now unblocked (their dependency, 01, is in master).
- Package 07: spec'd in `07-lv2-live-editor.md` (committed as "close open decision #1"), unstarted. The user has already decided the headline question — the native LV2 editor SHOULD drive live playback; view-only was a stepping stone.
- Known open defect, not yours to fix here: 4K EQ 2 reports runtime-varying latency (0→27→0) that the host's Prepare-time latch cannot represent; PDC can be ~27 samples wrong when it oversamples. Recorded in `02-lv2-host-PR.md`.

## First task: reconcile the VM

**The merge is already done** (2026-07-20): `feature/inserts-ui` fast-forwarded into master at `5a09823`, host suite re-verified green on master (46/46). Start by branching: `git checkout -b feature/lv2-live-editor` off master.

VM note: the VM working copy (`ssh user@192.168.122.232`, `~/haiku-daw`) is **not a git remote**. It sits at `cf2e1df` with later changes applied as loose synced files, so its tree only approximates master. Reconcile it before phase 0: sync every file in `git diff --name-only cf2e1df..master` using the tar recipe in the RESUME doc, `touch` them on the VM (clock skew), rebuild all three VM configs, and confirm your files appear as `Building CXX object` lines.

## Then: package 07, in phases

Work `07-lv2-live-editor.md` in this order; each phase is a coherent commit series.

- **Phase 0 — click the unverified fix.** The editor-close crash fix (`4099b14`, detach views before `cleanup()`) was diagnosed from core files and reviewed, but nobody has clicked it. Open an LV2 editor on the VM, close it while playing and while stopped, repeat a few times. If it still aborts, that core file is your first job — tooling in `~/crashreports/` on the VM, reading instructions in the RESUME doc.
- **Phase 1 — link control-port UIs (Problem 1).** Widen `Lv2UiWindow::Open` with the insert's address (`track`, `fxIndex`, messenger to MainWindow — both call sites already hold them) and route the UI's `write_function` control-port floats to `kMsgFxLive` → `Engine::SetFxParamLive`. This is the tractable half; it lands on its own and is immediately audible.
- **Phase 2 — the DIRECT_ACCESS decision gate.** DPF `WANT_DIRECT_ACCESS` UIs (4K EQ 2 et al.) bypass `write_function` and poke the DSP instance through a raw handle. The spec's options A (leave them view-only), B (hand over the live handle, accept the race), C (mediated instance, mirror params both ways through the existing single-writer `SetParam` path). **This decision belongs to the user — present the options before implementing.** Recommendation to carry into that conversation: **C** — it is the only one that is both live and free of new RT hazards; B's failure mode is a core file in the audio thread.
- **Phase 3 — engine → UI direction (Problem 3).** Automation or a second editor must reach the native UI's `port_event`. This needs the engine to publish per-insert control-port values (the per-insert meter publishing is the shape to mirror) and the editor to pump them from a non-looper thread (`LockGL` deadlocks on the looper — RESUME rules). This is a deliberate engine change: write the design into the PR doc before coding it.
- **Phase 4 — identity and teardown.** Engine rebuilds (structural edits, rebuild-on-play) must not leave the editor linked to a freed instance — address by `(track, fx, slot)` resolved per call, never a cached pointer. Add the fifth teardown rule from the spec: on window close, unhook the engine from editor state BEFORE the editor's instance/handles die. Verify by closing during playback.

## Constraints (inherited, non-negotiable)

- One live channel: `kMsgFxLive` / `SetFxParamLive`. No second path.
- Every LV2 reference behind `#ifdef DAW_HAVE_LV2`; `-DDAW_LV2=OFF` must build.
- All existing teardown/lock rules from the RESUME doc: detach before `cleanup()`, never `dlclose`, window→world lock order, all lilv access under `gWorldMutex`, never render from `Draw()`.
- Mutation funnel unchanged: editors post to MainWindow; commands own the model.

## Definition of done

- 03 merged to master; VM reconciled and green in all three configs.
- Everything under "Definition of done" in `07-lv2-live-editor.md`, including: control-port editors audibly live with no mouse-up gate; DIRECT_ACCESS handled per the user's choice with truthful window titles; engine→UI reflection working; close-during-playback clean; `-DDAW_LV2=OFF` and host suites green.
- A `07-lv2-live-editor-PR.md` in this directory recording decisions, coverage, and what remains unverified — same discipline as the 02/03 PR docs. Report unclicked paths as unclicked.

## Queued after this package

04 (midi-tools), 05 (sidechain), 06 (timestretch) are all unblocked and mostly host-testable on Linux — they can run as parallel agents while this package occupies the VM. Small overlap warning: 05 edits the same engine FX loop regions 01 reshaped; merge 05 before or after this package, not during a shared engine edit.
