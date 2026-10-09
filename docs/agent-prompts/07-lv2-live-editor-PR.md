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
| Phase 2 — the DIRECT_ACCESS decision | **decided: option C, by the user**; implemented |
| Phase 3 — engine → UI direction | implemented (design below) |
| Phase 4 — identity and teardown | implemented |

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

One case has no audio block to publish from: the transport is stopped and the
generic parameter panel moves a value. `Engine::PublishFxWatchNow()` covers it —
called from MainWindow right after `SetFxParamLive`, and it returns immediately
while the audio callback is running (then the block publishes on its own).

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

## Phase 4 — identity across rebuilds, and teardown

The editor addresses its insert by `(track, fxIndex, slot)` and holds no pointer
to anything the engine owns: `kMsgFxLive` and the new watch both resolve against
the running chain per call (the engine) or per message (MainWindow), so an engine
rebuild — a structural edit, and every play — cannot leave a dangling link.

The one hole that left was *index* identity: reorder or remove an insert and
`fxIndex` silently means a different effect, so a live editor would drive a
plugin it is not showing. MainWindow therefore remembers which insert an editor
registered for (by URI) and re-checks it wherever the chain changes: the editor
follows its insert if it moved — and is told its new index, or its next write
would use the old one — and closes if the insert is gone, exactly as the generic
panel does when its focused insert disappears.

Teardown order, in the destructor, on the way out:

1. stop the editor's thread (last thing that can read port buffers or post);
2. tell the engine to stop watching (registration cleared; the watch also dies
   with the editor's messenger, so a crash cannot leave the engine publishing);
3. commit anything the editor wrote but had not yet committed;
4. detach the container, then `cleanup()`, then delete the container;
5. never `dlclose`.

## The adversarial review, and what it changed

A second reviewer read the whole branch diff (finding 9 bugs, 7 risks, 2
questions). The seven that were fixed, all committed:

1. **Every engine rebuild dropped the watch.** `StartPlayback`,
   `StartRecordEngine`, the loop-record seam and the idle monitor each build a
   NEW `Engine`; the watches lived in the old one, so an open editor stopped
   following automation the first time the user pressed play. Re-applied now
   wherever `SetMeterFocus` already was, and the monitor-only block — the one
   branch that returned without capturing — publishes too.
2. **One watch slot, and a stop message with no identity.** Opening a second
   editor stole the first one's watch, and ANY editor closing cleared it. Now a
   table of watches keyed by `(uri, track, fx)`, pruned when a window dies, and
   closed wholesale on `File > Open` — a loaded project reuses the same track
   ids and indices for different effects.
3. **Undo/redo did not re-check the watched insert.** With the transport
   stopped nothing else would have; an undone reorder left the editor on its old
   index, driving whatever moved there.
4. **The inspector and the effects panel opened the editor from their own copy
   of the chain.** The panel's copy can be stale, so the window could be seeded
   from one insert and aim its writes at another. Both now ask MainWindow, which
   resolves against the model and decides whether there is a native editor at
   all. The title also carries the track name, so two editors for the same
   plugin are tellable apart.
5. **Two copies of one plugin in a chain.** URI alone cannot say which insert an
   editor was showing after a reorder, so it closes rather than guess.
6. **Direct-access editors lost their own writes.** Applying the engine's frame
   after `idle()` overwrote the value the plugin had just written into its port
   buffer, and the poll then saw no change and never sent it. Inbound now
   applies BEFORE idle; a slot with a gesture in flight is left alone; and
   writes are ignored until the editor has drawn its first frame, so a UI
   pushing its own defaults is not recorded as an edit.
7. **A save inside the 400 ms debounce wrote the pre-gesture value.** MainWindow
   now asks each open editor for its pending values on the save path and applies
   them before `ProjectIO::Save`.

Also from the review: the seqlock read takes the acquire fence it needs before
trusting the generation check (the same missing fence is in the pre-existing
`MeterSpectrum`, left alone here), the editor's insert address became atomic
(the window's looper can re-point it while the editor's thread reads it), and
the claim that keeps one editor per insert now keys on the insert, not the
plugin.

**Known, not fixed** (each judged smaller than its fix, and stated rather than
buried):

- The list is capped at `Engine::kWatchSlots` (4) editors following automation;
  past that an editor still works and still writes, it just stops following, and
  says so once on stderr. `kWatchMax` (64 parameters) truncates the same way for
  a plugin with more control inputs than that.
- `~Lv2UiWindow` joins the editor thread while the looper holds the window lock.
  A plugin whose `idle()` or `port_event()` blocks on that lock would hang the
  close. Inherited from the idle thread package 03 shipped (which Phase 0
  click-tested clean); port_event now shares the thread, so the exposure is
  slightly wider than before.
- A UI that declares `instance-access` as an *optional* feature but writes its
  ports directly anyway is treated as a control-port UI and gets no poll. The
  spec says a UI that writes DSP state directly must require it; no plugin here
  does otherwise.
- A control-port UI that pushes widget defaults through the write function after
  its first frame is indistinguishable from the user moving a control.

## Automated tests, and what they can and cannot reach

The live-editor code splits into three layers, and each is tested where it can
actually run:

| Suite | Runs on | Covers |
| --- | --- | --- |
| `lv2_ui_map_tests` (46 checks) | host | Port↔slot numbering (control OUTPUTS must not consume a slot), the inverse map, and the inbound apply policy: a value the GUI already shows is not re-sent, a parameter with a gesture in flight is left alone, out-of-range slots and unknown ports are dropped, and the port buffer plus the poll's last-seen are kept in step so the two directions cannot echo |
| `fx_watch_tests` (33 checks) | host | Registration identity by `(uri, track, fx)`, engine-slot allocation and exhaustion, and every chain-edit rule: keep, follow-a-move (with the new index), close on removal/dead window/dead track, close on an ambiguous URI, and the forced frame after a re-registration |
| `lv2_fixture_tests` (+9 checks) | host | `Lv2Effect::ControlValues` reports what actually took effect (clamped, per slot) and nothing for a built-in; `UiRequiresInstanceAccess` answers both ways against a fixture bundle that declares one UI of each kind |
| `fx_insert_tests` (+83 checks) | host | `SetFxParamCommand`: touches only its slots, undoes exactly, honours the master flag, refuses an index that addresses nothing |
| `lv2_live_editor_tests` (24 checks) | **Haiku VM** | The engine half, which no host test can reach because `Engine.cpp` links the Media Kit: a watched insert publishes its real, clamped parameter values; an untouched insert publishes NOTHING (the generation is the UI's cheap change test); watches are independent, survive being re-pointed, and report 0 — never a negative count — when there is nothing to publish |

Every one of these was mutation-tested: numbering every port, dropping the
gesture guard, matching registrations on URI alone, allowing an ambiguous URI,
looking for the wrong required feature, storing an unclamped parameter, and
publishing on every block each made the new assertions fail, and each mutation
was reverted.

**What no automated test here can reach, and why:** `app_server` is not
reachable from an SSH session on this VM (that is also why `screenshot` over SSH
exits 69), so no test can create a `BWindow`. That rules out the editor window
itself, the MainWindow message paths, and anything audible. Those stay on the
click list below, and the design tries to keep the *decision-making* out of them
precisely so that what remains manual is wiring rather than logic.

## What is verified, and how

| Claim | Evidence |
| --- | --- |
| Fixture is installable and the DAW's own host scans, verifies, instantiates and processes it | `lv2_host_tests` on the VM: `4K EQ 2 stereo params=34`, 105 checks, 0 failures |
| The mode split is real, not vacuous | `uimode` probe on the VM: Parameters → 0 required features; 4K EQ 2 → 5, including `instance-access` |
| Phase 0 close-crash fix | clicked by the user (see above) |
| Everything builds where it must | VM `build` 49/49, VM `build-off` 45/45, 0 warnings; host `build-host` 48/48 (the counts include the suites added here) |
| `SetFxParamCommand` touches only its slots, undoes exactly, honours the master flag, and refuses an index that addresses nothing | host tests, mutation-tested three ways (drop the old-value capture, restore the new value on Undo, ignore the master flag — each made the new assertions fail) |
| `Lv2Effect::ControlValues` reports what actually took effect (clamped, per slot) and a built-in reports nothing | `lv2_fixture_tests`, mutation-tested twice (ignore `maxSlots`; publish nothing — each made the new assertions fail) |
| A knob move in a live editor is audible during the drag | **NOT by test** — needs a real window and a real drag; waiting on the click test |
| Automation moving a parameter shows up in the open editor | **NOT YET — waiting on a click test** |
| Reordering/removing an insert redirects or closes its editor | **NOT YET — waiting on a click test** |
| Close during playback is clean with a live editor | **NOT YET — waiting on a click test** (the Phase 0 test preceded any of this code) |
