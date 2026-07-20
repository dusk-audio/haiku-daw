# Resuming Package 03 (inserts UI) — read BEFORE picking up any remaining item

Package 03 is **largely built and reviewed twice**, but not finished and not
merged. This file is the state of the branch as of `d892bf1`. It is a *resume*
handoff, not the entry handoff — `03-inserts-ui-HANDOFF.md` is the one that was
written going *into* the package, and its API notes and warnings all still hold.

Read in this order:
1. This file (current state, what is left, what is unverified).
2. `03-inserts-ui-PR.md` — the full record, including the addendum covering
   native plugin editors, region editing, and both review passes.
3. `03-inserts-ui.md` — the original task doc. Stale in places; where it and the
   two files above disagree, they win.

## Branch state

- Branch `feature/inserts-ui`, **30 commits** ahead of `master`, HEAD `d892bf1`.
- Working tree clean. Nothing merged yet.
- Green everywhere it can be run: Linux host 46/46, `-DDAW_SANITIZE=ON` 46/46
  leak-clean, `-DDAW_LV2=OFF` 43/43, Haiku VM 46/46 in both LV2 configurations
  with **0 warnings**. Real Haiku hardware was also green, but is powered off.

## What is DONE

Work items 1 (inspector only), 2, 4 and the generic parameter panel from item 3.
Beyond the task doc, the branch also grew:

- **Native LV2 plugin editors** (`src/ui/Lv2UiWindow`). An LV2 insert opens the
  plugin's own GUI; the generic list is the fallback. This required porting DPF's
  Haiku pugl backend, which had never rendered a frame on the platform — that
  patch is OUTSIDE this repo at `~/projects/dpf-haiku-gl-ui.patch` (~1000 lines)
  because it belongs to DPF. `prototypes/lv2_ui_host` is the standalone harness
  that proved it.
- **Inspector layout** reordered to the signal flow the user specified: input,
  inserts, sends, output, group, automation, pan, fader, record/monitor,
  mute/solo.
- **Region editing**: MIDI regions grow to cover notes drawn past the end;
  left-edge trimming exists; `TrimClipFrontCommand` handles start, length,
  source offset (audio) and clip-relative note/controller rebasing (MIDI).

## What is NOT done

| Item | Note |
| --- | --- |
| Mixer-strip slot list (item 1) | Inspector only. The mixer is a separate looper editing a snapshot, so bypass must **post** rather than execute — not the inspector's pattern. This is the biggest remaining piece. |
| Per-effect bypass + wet/dry header in the editor (item 3) | The strip can bypass; the editor cannot, and wet/dry has no UI anywhere. The task doc's analysis of why both must be commit-only still stands and is still correct. |
| Click-a-row scrolls the editor to that slot | `EffectsWindow` has no API for it. |
| Six-plus inserts leave no empty row on the strip | Adding must go via the editor; the overflow row opens it. |

## Open decisions that are the USER'S to make — do not decide these alone

1. **May the plugin GUI touch the live instance?** The native editor is
   deliberately view-only: it holds its own instance seeded with the insert's
   stored values, and says so in its window title. Wiring it to the playing
   instance means letting a GUI touch an object the audio thread uses every
   block. That is an RT-boundary decision, left open rather than made silently.
2. **Dynamic plugin latency.** 4K EQ 2 reports latency that moves at runtime
   (0 → 27 → 0). The host latches once at `Prepare`, as `IEffect` requires, so
   PDC can be ~27 samples wrong when that plugin oversamples. The likeliest fix
   is in the plugin, not the host. See `02-lv2-host-PR.md`.

## Open problem

**A click-time crash in the running DAW**, reported by the user and never
reproduced against the fixed build. Several lilv races were fixed after it was
seen and may have removed it — **that is unconfirmed**. Do not record it as
fixed without a backtrace. Notes that cost time to learn:

- Haiku's `debug_server` holds a crashed team and its dialog until dismissed, so
  a stale dialog reads exactly like a fresh crash. `kill -9` does not clear it.
- `ps` on Haiku lists **threads**, not processes — do not count lines and
  conclude the app is running twice.
- Run with `DEBUG_SERVER_DISABLE_GUI=1` and capture to a log.

## Environment, exactly as it works today

- **VM**: `ssh user@192.168.122.232`, repo at `~/haiku-daw`. This is the only
  machine that can build `src/ui/`. Real hardware (192.168.1.186) is powered off.
- **The VM is a working copy, not a git remote.** It sits at `cf2e1df` with
  later changes applied as loose files. Sync edited files with:
  ```sh
  tar cf - <paths> | ssh user@192.168.122.232 'cd ~/haiku-daw && tar xf -'
  ```
- **The VM clock is ~4 h behind the host**, so make floods the log with "modification
  time in the future" and "Clock skew detected", and may skip rebuilding. `touch`
  the synced files on the VM, then filter the noise:
  ```sh
  ssh user@192.168.122.232 'cd ~/haiku-daw && touch <paths> && cd build &&
      cmake --build . -j4 2>&1 | grep -Ev "^make\[|Clock skew|modification time"'
  ```
  Always confirm the files you changed appear as `Building CXX object` lines. A
  silent "Built target" after a sync means make skipped them.
- **VM build dirs**: `build` (LV2 on), `build-lv2`, `build-off` (`-DDAW_LV2=OFF`).
- **Host build dirs**: `build-host` (plain), `build` (`-DDAW_SANITIZE=ON`).
- `scripts/serve.sh` defaults to **port 9090**: the host's firewalld drops 8000,
  so a clone from another machine *hangs* rather than being refused, which reads
  as a fault on the Haiku end. `scripts/hw_setup.sh` bootstraps a fresh Haiku
  install on real hardware.
- Screenshots of the running VM come from the user's GNOME session (the VM window
  is on the left of the screen). `screenshot` over SSH fails with exit 69 —
  app_server is unreachable from a non-login session.

## Rules this branch has already paid for

- **Never render from `BView::Draw()`.** It runs on the window's looper thread,
  which already holds the lock `BGLView::LockGL()` needs. First expose deadlocks:
  window appears, stays black, process alive but wedged.
- **Do not `dlclose()` a plugin UI library.** `~BWindow` deletes surviving
  children *after* the destructor body returns, so a view the plugin forgot to
  remove is deleted through a vtable in an unloaded library. Detach children in
  the destructor and keep the handle.
- **Lock order is window → world, never world → window.** `~Lv2UiWindow` runs on
  the looper holding the window lock and then takes the lilv world lock, so any
  path that takes the world lock first and the window lock second deadlocks.
  `Lv2UiWindow::Open` does all lilv work before the window exists for this reason.
- **lilv is not thread-safe** and is reached from several loopers. Every world
  access is under `gWorldMutex`.
- **Do not zero-fill `EffectDesc.params`.** Slot 1 on the JUCE-built LV2 plugins
  installed here is `Enabled`, default 1; a zero-filled descriptor renders
  silence, indistinguishable from a broken host. Use `EnsureParamSlot` /
  `MakeInsertDesc`, which seed from the plugin's own port defaults.
- **Hit "kinds" in the editor are dispatched by bare integer.** Enumerate the
  existing ones before assigning a new one — a collision compiles fine and
  silently routes events into the wrong handler.
- **Guard every LV2 reference** with `#ifdef DAW_HAVE_LV2`; `-DDAW_LV2=OFF` is a
  required configuration.

## Verification discipline expected here

- **Build on the target.** Package 02's worst bug (LV2 finding zero plugins on
  Haiku) passed every Linux *and* Haiku test, because "no plugins installed" is a
  legitimate pass.
- **Mutation-test key assertions**: break the code deliberately, confirm the test
  fails, revert. Every review fix on this branch that had testable behaviour was
  checked this way, and it has twice shown that an assertion was covering nothing.
- **Report what is unverified.** Most of this package was compiled but never
  clicked. Say so; do not let "compiles clean on the target" read as "works".

## Not ours, still worth reporting

`imgui_impl_opengl2.cpp` in DPF-Widgets guards its `glPushMatrix()` calls behind
`#ifndef IMGUI_DPF_BACKEND` but leaves the matching pops unguarded, so every
frame pops twice against zero pushes. Platform-independent; visible on Haiku only
because Mesa's software rasteriser reports what desktop drivers ignore. Belongs
upstream — the user's plugins depend on DPF-Widgets, so patching locally is the
wrong fix.
