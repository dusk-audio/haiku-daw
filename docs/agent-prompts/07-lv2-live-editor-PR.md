# feat(lv2): link the native plugin editor to the playing instance

Package 07. Branch `feature/lv2-live-editor` off `master` `73e463b`.

Everything here is Haiku-only UI plus one engine-adjacent addition; the flags it
adds to the model are host-testable. Section "What is verified" is the honest
list, split into what was clicked and what was only compiled.

**This document is written as the work lands, not afterwards** — the sections
below say which parts are done and which are still design.

## State

| Step | State |
| --- | --- |
| VM restored (`build` LV2-on, `build-off` `-DDAW_LV2=OFF`) | done — 46/46 and 43/43 |
| Host suite `build-host` | 46/46 (46 before this package's tests, 46+ added ones below) |
| Plugin fixture on the VM | done — **4K EQ 2** (direct-access) and **Parameters** (control-port) |
| Phase 0 — click the editor-close crash fix | **done, by the user** (see below) |
| Phase 1 — link control-port UIs | code landed; click test pending |
| Phase 2 — the DIRECT_ACCESS decision | **decided: option C, by the user** |
| Phase 3 — engine → UI direction | design below, not implemented |
| Phase 4 — identity and teardown | partially in from Phase 1 (address-by-(track,fx,slot), commit-on-close) |

## The VM, and how the fixture was rebuilt

Marc replaced the dev VM on 2026-10-08: libvirt domain `haiku-beta6`, Haiku
R1/beta6, **2 vCPU / 2 GB** (build with `-j2`), 192.168.122.48, SSH key
`~/.ssh/haiku_vm`. `build-off` had to be created (`cmake -B build-off
-DDAW_LV2=OFF`); `build` was already configured LV2-on.

**DPF no longer exists — it is DAF.** There is no `/home/marc/projects/DPF` or
`DPF-Widgets`; the framework is `/home/marc/projects/DAF` (which now carries the
widgets in-tree as `DAF/widgets/`), and the plugins are `plugins/<name>/daf-plugin/`
with `DafPluginInfo.h` and `shared-daf/`. So the fixture was rebuilt from DAF
rather than from a DPF checkout, and `dpf-haiku-gl-ui.patch` — which is still the
patch that makes the Haiku GL backend render and take input — needed two ports to
DAF's newer pugl before it linked:

| Old (DPF-era) | New (DAF's pugl) | Why |
| --- | --- | --- |
| `puglGetNativeView(PuglView* const)` | `puglGetNativeView(const PuglView* const)` | pugl made the view const (acafe87); in C++ a different overload, so it goes silently undefined at link time |
| `puglViewStringChanged(...)` | `puglApplyViewString(...)` | renamed upstream (3ecf2df) |

Both were applied to the VM's scratch copy of DAF only. Marc's checkouts were not
touched. Recipe, for the next person:

1. `git -C /home/marc/projects/DAF archive HEAD` and
   `tar cf` of `plugins/plugins/{<plugin>,shared-daf}` → scp to the VM → extract
   under `~/fixtures/` (`DAF/` and `plugins/`).
2. `patch -p1 < dpf-haiku-gl-ui.patch` in `~/fixtures/DAF`, then the two ports
   above.
3. Configure an LV2-only overlay `CMakeLists.txt` that calls `daf_add_plugin(...)`
   with `TARGETS lv2` and the plugin's `FILES_DSP` / `FILES_UI` +
   `${DUSK_DAF_UI_SOURCES}` (the plugin repo's own CMakeLists drags X11 test
   harnesses that cannot configure on Haiku). Build, then copy
   `build*/bin/<plugin>.lv2` to `~/config/non-packaged/lib/lv2/`.
4. `4K EQ 2` is `MONOLITHIC` (UI inside the DSP binary); DAF's `Parameters`
   example is not (separate `_ui.so`).

**A control-port UI fixture is needed and now exists**: every Dusk plugin is
`DAF_PLUGIN_WANT_DIRECT_ACCESS`, so none of them can exercise Phase 1. DAF's own
`examples/Parameters` (`http://distrho.sf.net/examples/Parameters`, 9 control
inputs) is the Phase 1 target.

## Phase 0 — the crash fix, verified by clicking

The package 03 fix (`4099b14`, detach the plugin's views before `cleanup()`)
had never been clicked. The user opened the 4K EQ 2 editor, closed it while
playing and while stopped, repeatedly, on the new VM: **clean close, every
time**. That is the first click confirmation this fix has had.

## Phase 1 — control-port UIs drive the playing insert

`Lv2UiWindow::Open` now takes the insert's address and a messenger
(`track`, `fxIndex`, `BMessenger apply`), and all three call sites pass what they
already held (inspector slot list, effects editor, mixer strip → MainWindow).

A UI that does **not** declare `instance-access` among its required features is
linked: its `LV2UI_Write_Function` writes are posted as `kMsgFxLive` — the one
live channel, unchanged — so a knob move is audible during the drag, and are
committed to the model on a 400 ms debounce (and once more on window close) via a
new `SetFxParamCommand`. That commit is what keeps a rebuild-on-play from
silently reverting the gesture the user just heard; it deliberately does NOT call
`SyncFxToEngine`, because the audio already has the value and rebuilding the
chain would cut reverb tails to re-apply it.

The editor's local instance is still its own — the live link never hands out a
pointer to anything the audio thread owns.

## Phase 2 — the DIRECT_ACCESS decision: **C, mediated instance**

The user chose **C**. A direct-access UI keeps its own instance and never touches
the engine's; the host mirrors values across through the existing single-writer
path:

- **UI → engine**: the UI writes into ITS instance's port buffers, which are ours
  (`Impl::ctl`). A poll thread diffs those buffers at ~60 Hz and posts changes
  through the same `kMsgFxLive` + debounce machinery Phase 1 uses. No lock the
  plugin must know about, no pointer into the audio thread, and the read is a
  word-sized load the x86 memory model already makes indivisible.
- **engine → UI**: Phase 3, below.

The title keeps telling the truth per editor: a direct-access editor says
"(live - drives the playing insert)" only once this path exists, never before.

## Phase 3 — engine → UI (design)

Without this, everything is one-way: automation moving a parameter, or the
generic parameter panel being dragged while the native editor is open, changes
the sound and leaves the editor showing the old value. The design below is the
one being implemented.

**1. The engine publishes one insert's control values.** `IEffect` grows

```cpp
    // Current values of this insert's control parameters, in slot order.
    // Optional: an effect with no such notion returns 0. Read by the engine's
    // own audio thread while publishing, never by a UI.
    virtual int ControlValues(float* out, int maxSlots) const { return 0; }
```

and only `Lv2Effect` implements it (a copy of the control buffer it already
keeps). `Engine::SetFxWatch(track, master, fxIndex)` names the insert to publish
— addressed exactly like `SetFxParamLive`, and resolved against the RUNNING
chain on every block, so a rebuild cannot leave it pointing at a freed instance
(`fxIndex < 0` stops). In `FillBuffer`, next to `CaptureFxMeters`, the watched
insert's values are copied into flat storage behind a seqlock, the same
mechanism `MeterSpectrum` already uses, so a reader cannot see a torn frame.

**2. MainWindow carries them to the editor** on the 60 Hz pulse it already runs
for meters. The editor registers with `kMsgFxWatch` (track, fx, messenger) when
it opens and again with no messenger when it closes; MainWindow pushes
`kMsgFxParams` (~40 floats) while a registration is live. The pulse now also
runs while an editor is open and the transport is stopped, so a generic-panel
drag with everything idle still reaches the editor.

**3. The editor applies them as `port_event`**, and does so on its own thread —
never the window looper, where `LockGL` deadlocks. So there is ONE thread per
editor doing three jobs in a fixed order:

```
  idle()            renders the plugin's frame
  apply inbound     port_event() for values the engine published
  poll own buffers  (direct-access only) notice the user's edits
```

Applying an inbound value also writes it into the port buffer AND into the
direct poll's "last seen", so the two directions cannot echo each other into a
loop. `port_event` on the direct mode's instance keeps the plugin's own state
consistent with what its UI is now showing.

What this deliberately does not do: the editor still never touches the engine's
instance, and the engine never calls into the editor's.

## What is verified, and how

| Claim | Evidence |
| --- | --- |
| Fixture is installable and the DAW's own host scans, verifies, instantiates and processes it | `lv2_host_tests` on the VM: `4K EQ 2 stereo params=34`, 105 checks, 0 failures |
| The mode split is real, not vacuous | `uimode` probe on the VM: Parameters → 0 required features; 4K EQ 2 → 5, including `instance-access` |
| Phase 0 close-crash fix | clicked by the user (see above) |
| Everything builds where it must | VM `build` 46/46, VM `build-off` 43/43, 0 warnings; host `build-host` 46/46 |
| `SetFxParamCommand` touches only its slots, undoes exactly, honours the master flag, and refuses an index that addresses nothing | host tests, mutation-tested three ways (drop the old-value capture, restore the new value on Undo, ignore the master flag — each made the new assertions fail) |
| A knob move in a live editor is audible during the drag | **NOT YET — waiting on a click test** |
| Engine → UI reflection | **not implemented** |
