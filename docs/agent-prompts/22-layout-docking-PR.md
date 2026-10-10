# M1.4 layout and docking — PR record

Branch `feature/layout-kit` off `master` (`3ee6eae`). Spec: `22-layout-docking.md`.

## Slice 1 — the dialogs build with the Layout Kit

Rename, Quantize, Export, Export-progress, Sends and Instrument. Each sizes
itself to its contents (`ResizeToPreferred`), which is what makes them correct
at a larger font — the old fixed rectangles grew neither with the controls nor
with the text inside them. Sends and Instrument rebuild their contents at
runtime (a send added or removed, the voice kind changed), so their roots are
`BGroupView`s: a group layout takes children at any time, and a row is its own
small group.

## Slice 2 — the main window gets real panes

The window is a layout: menu bar, transport strip, then a vertical split whose
top row is the inspector/timeline split and whose bottom item is the dock.

- The inspector is a split item: resizable, and toggled with
  View > Inspector (key **I**).
- Pane state persists — `uiinsp` (visible, width) and `uibottom` (visible,
  height) join `AppSettings`, with the round-trip covered by the host test
  (`appsettings_tests`).
- `TimelineView`, `InspectorView` and `TransportBar` carry `B_SUPPORTS_LAYOUT`;
  the transport strip reports its own preferred height (36 design px) instead of
  MainWindow pinning a rectangle for it.
- `FrameResized` keeps only what is still true: the transport bar's CONTENTS are
  pinned offsets, not a flow, so `LayoutTransportBar` stays.

## Slice 3 — the docked MIDI editor

The timeline no longer creates the piano roll's window itself: double-clicking a
MIDI region posts `kMsgOpenEditor`, and the main window — the only place that
knows where the dock is — opens the region in the bottom pane, uncollapsed and
focused. **Pop out** hands the same region to the `PianoRoll` window it used to
be. The playhead reaches whichever editor is showing (the docked view directly,
the window through the messenger it registers).

## Slice 4 — window behaviour

- **The toggles remove the view from the split rather than collapsing it.** A
  collapsed `BSplitView` item keeps its preferred size in this Haiku — the dock
  stayed 261 px tall after `SetItemCollapsed(1, true)`, which would have left an
  empty strip where the editor had been. Hiding and re-adding (at its old index)
  hands the space back deterministically.
- **The dialogs were passing FLAGS in the FEEL argument.** `BWindow(frame, title,
  B_TITLED_WINDOW, B_NOT_ZOOMABLE | ...)` puts the flags where the window feel
  belongs, so `B_NOT_ZOOMABLE`, `B_NOT_RESIZABLE` and `B_ASYNC_CONTROLS` were
  never applied and the feel was an invalid value (Haiku falls back to normal).
  The six dialogs now read `B_TITLED_WINDOW, B_FLOATING_APP_WINDOW_FEEL, <flags>`
  — dialogs stay above the app's windows and can no longer be zoomed.
- **The mixer is single-instance**: asking again sends the current strips to the
  open rack (`kMsgMixStrips`) and brings it forward (`kMsgMixerActivate`)
  instead of stacking a second one.
- **The mixer pans horizontally** with the wheel when the strips do not fit
  (clamped to the rack's width), so a rack wider than the screen is reachable
  rather than silently cut off.

## What this slice did NOT do

- **The browsers are not docked.** `SampleBrowser` and `PluginBrowser` build
  their contents directly into their windows; docking them needs each split
  into a view + window pair (as the piano roll already was). They stay as
  windows, opened from the View menu — the dock's Browser tab is the remaining
  piece of M1.4.
- **The effects editor is not single-instance.** It is per-track and takes its
  chain at construction; making one instance retarget means teaching the window
  to replace its chain, which is a change to the editor's own contract rather
  than a layout change. Left for its own slice, recorded here so it is not
  mistaken for done.

## The one that cost three VM cycles

A run killed while the project is dirty leaves `recovery.dawproj` in
`~/config/settings/HaikuDAW/`, and the next run opens the **Recover?** alert at
startup — which BLOCKS the window thread until someone answers it. Every check
that needs the window lock then times out, the first test fails, and the suite
sits there looking exactly like a hang. The tell: the log stops after the first
test's line while `ps` shows a live process and no `Crashed program` window.
Same shape as M1.1's crash-dialog lesson, different cause.

The suite now deletes that file before it creates the window, so a test run
starts from a clean slate. Nothing else changed: the recovery prompt itself was
never driven by the functional suite (the release checklist covers it).

## M1.5 and M1.6, measured in passing

- **Play start on 32 tracks / 320 clips: 3.1 s** on the VM. The plan's budget is
  300 ms. This is M4.1's job (the graph and 320 clip streams are built on the
  window thread); the number is now in `23-offscreen-and-icons.md` and in a
  smoke check in `ui_functional_tests`.
- **The timeline draw time could not be measured here**: the VM's screen is
  locked, so the app_server never asks the window to draw and M0.7's instrument
  printed nothing. Needs the hardware box.
- **The tool palette's icons are done** (`widgets/DawIcons.h`); the HVIF app
  icon is not, because nothing on this machine can author one — see the same
  file.

## Verification

- Host suite: **52/52**, including the new `AppSettings` pane round-trip.
- `haiku_syntax_check.sh`: **10 OK, 0 FAIL**.
- VM `build`: ctest **54/54**, `ui_functional_tests` **172 checks, 0 failures**
  (was 165; +7: the inspector toggle twice, the docked editor opening, the dock
  emptying, the popped-out window appearing and closing, and the pane bounds).
- VM `build-off`: ctest **50/50**.
- Click list for Marc: drag the inspector splitter; press **I** and **J**; open
  a MIDI region (double-click) and see it docked, then **Pop out**; the dialogs
  should feel like dialogs now (no zoom button, they stay above the main
  window); a rack with more strips than fit should pan with the wheel.
