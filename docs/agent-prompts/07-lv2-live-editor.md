# Task: link the native LV2 editor to the playing instance

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. This package
turns the LV2 plugin editor from **view-only** into a control surface for the
insert that is actually making sound. It is the follow-up the package 03 handoff
named as open decision #1 ("may the plugin GUI touch the live instance?"). The
user has since decided: **yes, eventually** — view-only was a deliberate
stepping stone, and this is where it stops being one.

Everything here is Haiku-only (`src/ui/Lv2UiWindow`, plus an engine touch-point).
It cannot be compiled on the Linux host; build and run on the VM
(`ssh user@192.168.122.232`). See `03-inserts-ui-RESUME.md` for the sync/build
recipe, the clock-skew workaround, and the crash-reading tooling in
`~/crashreports/` — you will want the latter, because a live editor is a new way
to touch an object the audio thread owns, and the failure mode is a core file.

## The one decision that is NOT yours to make alone

**Whether a DPF DIRECT_ACCESS UI may hold the engine's live instance pointer.**
See "Problem 2" below. It is the whole RT-safety crux of this package. Bring the
options to the user; do not pick one silently. Everything else here you can
carry on your own judgment.

## What "view-only" is today, exactly

`Lv2UiWindow::Open` (`src/ui/Lv2UiWindow.cpp`) stands up the plugin's own GUI
against a **throwaway DSP instance** it instantiates itself (`d->dsp`, seeded
from the insert's stored `params`). Two things make it inert:

1. The `write_function` handed to `instantiate()` is a no-op — control-port
   writes from the UI are accepted and dropped (see the lambda around line 466).
2. `d->dsp` is not the instance in the audio graph. Nothing the UI does reaches
   the sound, and nothing the sound does (automation, another editor) reaches
   the UI. The window title says so.

The RT-safe channel for control-port values **already exists** and is used by
the generic parameter panel:

```
editor -> kMsgFxLive(track, fx, slot, val) -> Engine::SetFxParamLive
       -> Lv2Effect::SetParam            (a plain float store into the
                                           control-port buffer, re-read each run)
```

`Lv2Host.cpp`'s `SetParam` is a single-writer store into the buffer the port is
already connected to (`src/plugin/Lv2Host.cpp:230`). That is the model to reuse.

## Problem 1 — the editor does not know which insert it is

`Open(frame, uri, name, params)` carries no `track`, no `fxIndex`, and no
messenger to MainWindow. It cannot post `kMsgFxLive` because it cannot name the
insert. First piece of work: widen the entry point so the editor knows its
address in the graph and can talk to the main window, the same way
`EffectsView` does. Both call sites (`InspectorView::MouseUp`,
`MainWindow::kMsgMixFx`) already have the track and slot in hand.

With that, a **control-port** UI links cleanly: route the `write_function` to a
`kMsgFxLive` post (mind the URID — control-port writes come through with the
plain-float protocol; anything else is Problem 2). This is the tractable half
and worth landing on its own.

## Problem 2 — DIRECT_ACCESS UIs bypass write_function entirely  ← the RT decision

4K EQ 2 and the other DPF plugins here are built `WANT_DIRECT_ACCESS`, so their
UI is handed the **instance handle** through `instance-access` /`data-access`
(`src/ui/Lv2UiWindow.cpp:453-460`) and reads and writes DSP state through that
raw pointer — it never calls `write_function` for those. Today that pointer is
the harmless throwaway `d->dsp`. Linking such a UI live means handing it the
**engine's** instance handle instead, at which point the GUI thread mutates an
object the audio callback runs every block, with no lock the plugin knows to
take. That is the boundary package 03 refused to cross silently.

Options to put to the user, not to choose for them:

- **A. Leave DIRECT_ACCESS UIs view-only; link only control-port UIs.** Safe,
  smaller, and honest — the title keeps saying "view only" for the plugins it
  is still true of. But it means the plugin the user most wants (4K EQ 2) stays
  view-only.
- **B. Hand over the live handle and accept the race.** DPF's own reads/writes
  are mostly word-sized stores the x86 memory model makes atomic-ish; it may
  "work" and occasionally glitch. Fragile, and the failure is a core file.
- **C. A mediated instance.** The editor keeps its own instance but the host
  mirrors parameter changes both ways (engine <-> editor) through the existing
  single-writer `SetParam` path, so neither side ever touches the other's
  instance. More work; no new RT hazard. Likely the right answer, but it is the
  user's to bless.

## Problem 3 — the reverse direction (engine -> UI)

Automation moving a parameter, or a second editor, must be reflected in the
native UI through its `port_event`. Nothing feeds that now. This needs the
engine to publish per-insert control values (it already meters per-insert; a
parallel push of control-port state is the shape) and the editor to pump them
into `port_event` from a thread that is NOT the window looper — the same
constraint the idle thread already lives under (`LockGL` deadlocks on the looper
thread; see the rules in the RESUME doc).

## Problem 4 — identity across engine rebuilds

The engine rebuilds its chain on any structural edit and again on play
(`ReloadActiveEngine`, rebuild-on-play). A live-linked editor holds a link to an
instance that is then freed and replaced. `kMsgFxLive` sidesteps this by
addressing the insert by `(track, fx, slot)` and letting the engine resolve it
each time — reuse that indirection rather than caching an instance pointer.
Anything that does cache a pointer (Problem 2 option B) has to be re-bound on
every rebuild, or it is a use-after-free waiting for a structural edit.

## Constraints

- Reuse the `kMsgFxLive` / `SetFxParamLive` path; do not invent a second live
  channel. If the engine needs a *new* capability (e.g. publishing control-port
  state for Problem 3), that is a deliberate, reviewed engine change — write it
  up, do not hack it into the UI.
- Keep `-DDAW_LV2=OFF` building; every LV2 reference stays behind
  `DAW_HAVE_LV2`.
- The teardown-order rules the crash fix established still hold: detach the
  plugin's views before `cleanup()`, never `dlclose`, lock window-then-world.
  Live-linking adds a fifth: on window close, tell the engine to stop expecting
  editor writes BEFORE the editor's own instance/handles go away.

## Definition of done

- Control-port LV2 UIs drive the playing insert; moving a knob in the editor is
  audible immediately (no mouse-up gate — this is the live path, unlike the
  strip's bypass/mix).
- The DIRECT_ACCESS decision is made by the user and implemented as chosen; the
  window title tells the truth about which mode each editor is in.
- Automation or a second editor moving a parameter updates the native UI.
- Closing an editor mid-playback does not crash and does not leave the engine
  writing into freed editor state. Verify by closing during playback, not just
  when stopped — the package 03 crash only showed with an editor actually open.
- `-DDAW_LV2=OFF` and both VM configurations build clean; host suites pass.
