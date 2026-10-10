# UI guidelines — read before touching anything under `src/ui/`

These rules exist because a whole milestone of UI work (M1.3/M1.4) was merged
with every test green and the main window visibly broken: a dead band under the
transport bar, a dB readout printed over the Mute button, a bottom pane that was
an unlabelled tab and a stretched button, plugin and parameter windows larger
than the screen, clip names running into the next clip, and alert buttons with
their labels against the left edge. `ui_functional_tests` passed 172, then 210
checks through all of it. **Tests prove behaviour; they do not prove the screen
looks right.** The fix (2026-10-10) is in `git log`; this file is how it does
not happen again.

The bar is a commercial DAW (Logic, Bitwig, Reaper): nothing overlaps, nothing
is clipped, nothing hangs off the screen, no dead space, every pane says what it
is, and an empty pane says how to fill it.

## 1. Looking is part of done

Any change that alters what a window draws or how it is laid out is not done
until you have **looked at screenshots of it** and fixed what you saw.

1. Build on the VM (`sh scripts/vm.sh build`; for an uncommitted tree see the
   bundle recipe in `docs/HANDOFF.md`).
2. Run the functional suite with screenshots on. The VM's screen must be
   unlocked — a locked screen draws nothing (`docs/HANDOFF.md`, measurement
   limits):
   ```
   sh scripts/vm.sh ssh 'rm -f ~/config/settings/HaikuDAW/recovery.dawproj;
     rm -rf /tmp/shots; mkdir -p /tmp/shots; cd ~/haiku-daw/build &&
     DAW_UI_SHOTS=/tmp/shots ./ui_functional_tests'
   scp -i ~/.ssh/haiku_vm 'user@192.168.122.48:/tmp/shots/*' <local dir>/
   ```
   Each test calls `Shot("name")` where it has a window up; the files are
   numbered in run order. (`screenshot` exits non-zero even when it worked —
   the "shot failed" lines are noise; the files are what count.)
3. **A window you add or change gets a `Shot()`** at the point a test has it
   on screen. No test opens it yet? Add one — `tests/ui_functional_tests.cpp`
   drives every window by posted message.
4. Open **every** screenshot that shows your change, at full size, and zoom on
   edges, dividers and text. Check it against the list in section 3.
5. Changed geometry? Look at 150% too (`SetThemeScaleOverride(1.5f)`, see
   `TestThemeScale`).
6. Say in the PR record which shots you reviewed and what you fixed. A
   "click list for Marc" is for what needs ears or real hardware — never a
   substitute for looking at the screen yourself.

Interactive check outside the suite: launch `./build/daw` on the VM, send keys
with `virsh -c qemu:///system qemu-monitor-command haiku-beta6 --hmp 'sendkey
alt-j'`, and capture with `virsh -c qemu:///system screenshot haiku-beta6 x.ppm`.

## 2. Make the test windows honest

- The suite installs `DawControlLook` exactly as `main.cpp` does. Never review
  a run without it: the stock look draws different splitters, buttons and
  backgrounds, and you will chase bugs that are not there (and miss real ones).
- A test that changes the model directly (`LockedAddTrack`, writes under the
  window lock) must post `kMsgUiRefresh` afterwards, as the app's own edit
  paths do. Otherwise only the strips the playhead sweeps repaint and the
  screenshot shows half-drawn lanes.

## 3. What to check on every screenshot

- **Fixed strips stay fixed.** Toolbars, header strips and bars do not grow
  when the window does. No unexplained empty bands.
- **Nothing overlaps.** Text over a control, a readout inside a button row.
- **Text fits.** Try the longest value (a long file name, `-23.4` rather than
  `--`, a 150% font). Truncate to the rect (`TruncateString`), do not overflow.
- **Labels are centred where they should be** (buttons, alerts, panel buttons).
- **Windows fit their content and the screen.** No window taller or wider than
  what it shows, none off the bottom of the screen.
- **Every pane names what it shows**, and an empty one says how to fill it.
- **No control sits on a box of a different shade** than its panel.
- **Disabled controls look disabled.**
- **Dividers are uniform** — no dots, ticks or stray lines.
- **Stale content is a bug:** after Open/New, after a pane resize, behind a
  modal alert, the view shows the current state.
- **Titles say what the window is for** ("Export Mix", "Locate take-2.wav"),
  never the stock "<app>: Save".

## 4. Layout Kit and drawing traps (each one bit this codebase)

- **A plain `BView` in a layout must report min and max sizes, not just
  `GetPreferredSize`.** Its max defaults to unlimited and every group item
  weighs 1.0, so a fixed-height strip soaks up spare height. Override
  `MinSize()`/`MaxSize()` (see `TransportBar`) or set explicit sizes.
- **Buttons in a group layout stretch** unless given an explicit max size (see
  `HeaderButton` in `MainWindow.cpp`).
- **No tab bar for a single page.** A `BTab`'s label comes from its view's
  name; an unnamed view gives an empty tab. Use a header strip until there is
  a second page.
- **`BStringView` does not centre vertically** in a stretched frame — put glue
  above and below it and keep the glue in a container you can swap as one.
- **Views that draw relative to their bottom or right edge need
  `B_FULL_UPDATE_ON_RESIZE`.** Without it a split-pane resize repaints only the
  exposed strip and the old bottom rows stay where they were (the inspector's
  Record/Mute rows vanished this way).
- **Anchor rows from the bottom up, then put readouts in the gap you left.**
  Drawing at `Bounds().bottom - 8` when the bottom row is 4..24 px tall puts
  the text inside that row.
- **`BSplitView`:** `DrawSplitter`'s `orientation` is the split's, not the
  bar's (a horizontal split's bar is vertical). A collapsed item keeps its
  preferred size in this Haiku — remove the view from the split to hide it
  (`SetInspectorShown` / `SetDockShown`).
- **`DawControlLook` overrides must honour every argument they are given** —
  alignment, flags, rects. `DrawLabel` once ignored its `BAlignment` and every
  stock button in the app drew its label flush left.
- **Kit controls take their background from their parent**
  (`AdoptPanelColors`), never a hard-coded colour.
- **Content-sized windows:** size from the content, clamp to the screen
  (`BScreen`). A plugin GUI window fits the plugin's view — and its child views
  follow their parent's edges, so pin them (`B_FOLLOW_LEFT_TOP`) for the fit and
  restore their modes afterwards, or the fit shrinks the plugin by the margin.
- **When the project is replaced (Open, New)** every view holding a track or
  clip id is reset: inspector, docked editor, record arms
  (`MainWindow::ShowReplacedProject`). A modal prompt about the new project
  comes after the views show it.
- **File panels** go through `ShowPanel()`: a title naming the action and, for
  save panels, a prefilled name.
- New geometry uses `Themed()` metrics (M1.2) and kit widgets (M1.3).
