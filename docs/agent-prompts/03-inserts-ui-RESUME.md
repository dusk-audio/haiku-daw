# Resuming Package 03 (inserts UI) — read BEFORE picking up any remaining item

All four work items are **built and reviewed three times**, but almost none of it
has been seen running, and none of it is merged. This file is the state of the
branch as of `d2f8618`. It is a *resume* handoff, not the entry handoff —
`03-inserts-ui-HANDOFF.md` is the one that was written going *into* the package,
and its API notes and warnings all still hold.

Read in this order:
1. This file (current state, what is left, what is unverified).
2. `03-inserts-ui-PR.md` — the full record, including both addenda covering
   native plugin editors, region editing, the mixer strips, and the review passes.
3. `03-inserts-ui.md` — the original task doc. Stale in places; where it and the
   two files above disagree, they win.

## Branch state

- Branch `feature/inserts-ui`, **34 commits** ahead of `master`, HEAD `d2f8618`.
- Working tree clean. Nothing merged yet.
- Green everywhere it can be run: Linux host 46/46, `-DDAW_SANITIZE=ON` 46/46
  leak-clean, `-DDAW_LV2=OFF` 43/43, Haiku VM 46/46 and 43/43 in the two LV2
  configurations with **0 warnings**. Real Haiku hardware was green as of
  `d892bf1`, but is powered off and has not seen the last three commits.

## What to do next

The code is done; **what it needs now is a person clicking it.** The manual test
script in the task doc's "Definition of done" has never been run. The mixer slot
list, the bypass dot on both strips, the reorder drag, the plugin browser and
the new bypass/wet-dry header have all been compiled and reasoned about, never
observed. Bypass and wet/dry in particular are worth *listening* to: they are
the two controls with no live preview, so the audio is meant to step once on
mouse-up, and the only way to tell that from "it did nothing" is to hear it.

## What is DONE

**All four work items.** Items 1, 2 and 4 in full; item 3 including the
per-insert bypass and wet/dry header, which commit on release for the reason the
task doc gives. Beyond the task doc, the branch also grew:

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
| Click-a-row scrolls the editor to that slot | `EffectsWindow` has no API for it. In practice a row click opens the editor on that insert *alone* (focus mode), so there is nothing to scroll past. |
| A full strip leaves no empty row | Six-plus on the inspector, four-plus on a mixer strip. Adding then goes via the editor; the overflow row opens it. |
| The wheel does not adjust wet/dry | It scrolls the panel list past that slider, like any control the wheel handler does not claim. |

## The defect the mixer work uncovered

Effect edits made from the **channel strip** — add, reorder, bypass — reached
the model and the drawing but never the running engine. `kMsgUiRefresh` is a
repaint; the only `Engine::SyncFx` call sat in the `kMsgApplyFx` handler, which
only the effects *editor* posts. Invisible when stopped (Play rebuilds anyway),
audible-as-nothing while rolling. Fixed with `kMsgFxChanged` →
`MainWindow::SyncFxToEngine`, now shared by both paths. Do not fold it back into
`kMsgUiRefresh`: the timeline posts that for clip moves, and syncing there would
rebuild the engine at the playhead for edits unrelated to effects.

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

## The crash: found, diagnosed, fixed (unverified by clicking)

Two core files on the user's Desktop settled it. **It was not a lilv race.**
Closing an LV2 editor aborted the team:

```
BLooper::Quit -> daw::Lv2UiWindow::~Lv2UiWindow + 0xb0   <- the cleanup() call
  -> four_k_eq_2.so (six frames)
    -> BView::~BView + 0x2a  ->  _kern_debugger
```

`BView::~BView` opens with an unconditional `debugger("Trying to delete a view
that belongs to a window. Call RemoveSelf first.")`, and `+0x2a` is exactly that
call. We handed the plugin our container as `LV2_UI__parent`, left it in the
window, and then called `cleanup()` — inviting the plugin to delete attached
views. The crash was inside the plugin; the cause was ours. Fixed by detaching
the container *before* `cleanup()`, which clears `fOwner` across the whole
subtree.

**Both cores show the same abort**, so this is the long-standing click-time
crash, not a new one. It has NOT been confirmed fixed by closing an editor —
that needs someone at the machine.

### How the cores were read, since it took a while to work out

- `Debugger -s --core <file>` **cannot** save a report from a core ("Operation
  not supported"). `Debugger -c --core <file>` works, but **needs a tty**: from a
  plain pipe it blocks forever and prints nothing. Drive it through a pty —
  `~/crashreports/drive.py` on the VM does this. Allow a long settle before
  sending commands; a 34-thread core outruns even 150 s.
- `sc` traces the *current* thread, which starts as the debug task and is empty.
  Select the real one first: `thread <id>`, then `sc`.
- **Check the binary before believing a symbol.** `build/daw` is relinked
  constantly, and the debugger resolves against whatever is on disk *now*. The
  newer core matched byte for byte; the older one differed in 1586178 of 1666209
  `.text` bytes, so every `daw::` symbol it printed was fiction. Library frames
  stayed valid in both. `~/crashreports/coremem.py` does the comparison.
- Finding the abort string in a core proves nothing by itself — it is also a
  plain constant in libbe's `.rodata` at `+0x2a9de0`, mapped into every process.
  The *second* copy, on a thread stack, is the recorded abort.

## Older notes on the crash, kept because they still apply

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
- **A UI edit is not applied until something tells the engine.** Executing the
  command and repainting is only two thirds of it; see the defect above.
- **The mixer holds a snapshot and must not read the model.** Anything it needs
  drawn — including an insert's *name*, which for LV2 is a host lookup — is
  computed on the main thread and pushed to it. Its edits post an intent
  ("toggle index 2"), not a value, so a stale snapshot cannot write the wrong
  state.

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
