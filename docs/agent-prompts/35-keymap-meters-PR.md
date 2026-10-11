# M3.2 keyboard map + M3.3 metering — PR record

Branch `feature/keymap-meters` off `master` (`6d88063`). Spec:
`docs/agent-prompts/35-keymap-meters.md`. Plan: `docs/PLAN_1.0.md` §M3.2–3.3.

## What landed

| Item | Where |
|---|---|
| the one key table (kit-free, as data) | `src/model/KeyMap.h` |
| the behaviour resolves keys through it | `src/ui/TimelineView.cpp` (`KeyDown` switches on `KeyAction`) |
| the help window, generated from it | `src/ui/ShortcutsWindow.{h,cpp}` (new), `MainWindow::MSG_SHORTCUTS` |
| the messages the map's window-level actions post | `src/ui/UiMetrics.h` (`kMsgTransportRecord`, `kMsgClickToggle`, `kMsgMeterClipReset`, `kMsgTransportLayout`) |
| the meter maths (kit-free) | `src/dsp/MeterMath.h` (dB scale, `PeakHold`, `StereoMeter`, `ClipLatch`, the clip bits) |
| the engine and recorder publish it | `src/engine/Engine.{h,cpp}`, `src/engine/Recorder.{h,cpp}` |
| one meter bar for every meter | `src/ui/Widgets.h` (`DrawMeterBar`, `DrawClipLamp`) |
| the master meter: hold, RMS, clip, correlation | `src/ui/MeterView.{h,cpp}` |
| track / mixer / inspector meters on the same scale | `TimelineView.cpp`, `MixerWindow.cpp`, `InspectorView.cpp` |
| the one threshold set stays the theme's | `src/ui/Theme.h` (`kMeterClip` is `kMeterWarn`'s sibling, now `= kMeterFullScale`) |
| host suites | `tests/keymap_tests.cpp` (608 checks), `tests/meter_tests.cpp` (59) |
| the functional flows | `tests/ui_functional_tests.cpp` (`TestShortcutsWindow`, `TestKeyMap`, `TestMetering`) |

### The keys

`Home` → start, `R` → record, `L` → loop on/off, `C` → click, `Enter` → return
to zero — plus everything the timeline handled before (`Space`, arrows, `+`/`-`,
`F`, `M`, PgUp/PgDn, Del/Backspace, Esc, Ctrl-D) and the menu accelerators as
documented rows. Shift-L keeps the marker-span loop (the key `L` used to have).

Two things the table surfaced:

- **Ctrl-D never worked.** The BeOS keymap delivers the *control character*
  0x04, not `d` with the Control flag, so the old `case 'd'` was dead code and
  `Duplicate` had no working key. The table binds the byte the keyboard sends.
- **Home only scrolled the view.** It now moves the playhead (and the view) to
  the start, which is what the plan asked for.

The piano roll's editor-local keys (1–7 tools, `q`) stay in `PianoRoll::KeyDown`
— they belong to that window's own toolbar; the header comment in `KeyMap.h`
says so.

### The meters

Master meter (transport bar): dB-scaled L/R bars (-60…+6 dB), an RMS line, a
peak-hold tick that falls 20 dB/s after 1.5 s, a per-channel clip lamp a click
resets, dB scale hairlines, and a correlation strip — all inside the meter's
existing 124 px, so the transport bar's pinned layout did not move. Track
headers, the mixer strips and the inspector use the same `DrawMeterBar` and the
same `MeterColor` bands, so a level means the same thing everywhere; the mixer's
fader-side scale is now a real dB scale instead of the linear gain ticks.

The engine publishes three more floats (RMS L/R, correlation) and one clip flag
word per block, from the same post-master-gain loop the peaks already walk, and
the recorder latches input clips the same way. No allocation, locks or I/O were
added; the RMS/correlation window is a preallocated `StereoMeter` prepared in
`Load`. The clip flags latch on the audio thread because the 60 Hz poll would
otherwise miss a clip that a later block overwrote.

## What the screenshot pass found (and fixed)

The first cut's shots showed the master meter painting at startup and then,
from the dock's **Pop out** onward, never again — a flat bar-coloured
rectangle where the meter should be. Two client-side `Invalidate`s and a
Hide/Show cycle changed nothing, and the geometry read back as correct.

The diagnostic prints (temporarily in the tree, removed before the commit)
showed `bar frame (0,22,1036,58)` in an **840 px window**, and
`MeterView::Draw` called exactly three times in the whole run. The chain:

1. A plain `BView`'s minimum size is its *current* size (`BView::MinSize` falls
   back to `GetPreferredSize`). `TimelineView` is an item in the pane split, so
   the split's — and the window layout's — minimum grew with it. After the dock
   was toggled, the window's layout minimum reached ~1036 px, and the layout
   hands that to every child: the transport bar was laid out **wider than the
   window**.
2. The app_server applies a child's resize mask when its parent grows, so the
   meter (`B_FOLLOW_RIGHT`) was moved to the bar's right edge — **outside the
   window** — on the server, while the client still had it at the old spot.
   Nothing painted it, and `MoveTo` early-returns while the client's cached
   position is unchanged, so the two never met again.

Fixes (all in this branch): `TimelineView::MinSize()/MaxSize()` say what the
view needs; `TransportBar::FrameResized` posts `kMsgTransportLayout` so the
window re-places the bar's pinned children when the *bar* (not the window)
changes width; `LayoutTransportBar` clamps to the window and also sizes the
meter, so it follows the theme scale; `TestMetering` asserts the meter is
inside the bar *and* the bar inside the window.

## Verification

- Host `build-host`: build exit 0, ctest **54/54** (was 52 — the two new
  suites). `b-asan` (`-DDAW_SANITIZE=ON`): build exit 0, ctest **54/54**.
- `scripts/haiku_syntax_check.sh`: **66 OK, 0 FAIL**. Master's copy of the
  script does not define `DAW_HAVE_LV2` (and lacks the add-on include dirs and
  lilv's), so it cannot compile the LV2 branches at all — this run used the
  script as it exists on `origin/feature/lv2-state` (which is where commit
  `33d6f05` added the define), copied to a scratch directory so this branch
  carries no script change.
- VM `build`: ctest **56/56**; `build-off` (`-DDAW_LV2=OFF`): ctest **52/52**.
- `ui_functional_tests` on the VM: **330 checks, 0 failures** (master: 210).
  The three new flows drive the real widgets: the shortcuts window opened by
  its menu message with its text read back out of the real `BTextView`, keys
  posted as real `B_KEY_DOWN` messages, and a `B_MOUSE_DOWN` on the real
  `MeterView` clearing a latched clip.

### Mutation checks

Host (`tests/keymap_tests.cpp`, `tests/meter_tests.cpp`), each applied, watched
failing, restored — 11 of 11 detected: a duplicate binding; a row with no help
text; the help dropping a row's line; `LinToDb` not 20·log10; 0 dB no longer at
full scale; the peak hold never decaying; the RMS window never evicting; the
correlation always +1; silence reading as correlated; the clip level ignoring
the int16 ceiling; `Reset` leaving the readings.

VM (`ui_functional_tests`), one run with eight independent mutations — 78
failures, each attributable: return-to-zero not zeroing; `L` never toggling;
the click not resetting the latch; the engine never latching; a hand-written
help text; a zeroed correlation; the shortcuts window stacking; the timeline
reporting no size. Two of the eight needed a second, focused run: the engine
mutation masked the click-reset check (nothing ever latched, so the reset
assertion passed vacuously) and the size mutation's symptom is the bar
overflowing the window, which the assertions only see through the new
bar-fits-the-window check.

### Screenshots reviewed (final run, `m3final`)

`00-startup`, `13-theme-150`, `16-docked-editor`, `18-popped-out-editor`,
`19-shortcuts`, `20-meter-playing`, `21-meter-clipped`, `22-big-project-playing`,
`26-opened-project` — every shot of every window this branch changes, plus a
pixel scan of all 27 shots confirming the meter's area is painted in each (it
was blank from the pop-out onward before the fix).

What I fixed after looking: the meter's disappear-the-first-time-you-toggle-a-
pane defect above; the shortcuts window sizing (the first cut counted only
non-empty lines, so the last three groups needed scrolling — the window now
asks the `BTextView` how tall its text is); the meter not following the theme
scale at 150% (`LayoutTransportBar` sizes it, not just places it).

One theme mode only: the base is pre-T1 (`master` has no mode switch and no
`DAW_UI_THEME`), so the both-modes pass belongs to whichever branch merges
after `feature/theme-modes`; every colour this branch draws comes from a
`Theme.h` token, so the mode switch reaches it unchanged.

## Left for Marc / later packages

- **Click list, ears:** the meters are audible-neutral (they only read), so
  there is nothing to listen for. Worth his eye on the real hardware box:
  the correlation strip's size in the 124 px meter, and whether he wants
  per-track clip lamps (track meters show level only today).
- The layout ratchet fixed here is the *general* form of the trap
  `docs/UI_GUIDELINES.md` §4 names: any plain `BView` in a layout reports its
  current size as its minimum. `InspectorView`, `PianoRollView` and the dock's
  panes still do; M1.4-era panes are the ones to check next time they change.
- `Editor & browsers` (the dock) still cannot show the shortcuts window's
  sibling help for the piano roll's own keys.
