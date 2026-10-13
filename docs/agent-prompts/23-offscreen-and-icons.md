# Task: Offscreen drawing (plan M1.5) and icons (plan M1.6)

You are working in `/home/marc/haiku-daw`. Branch `feature/offscreen-icons` off
master (M0–M1.4 complete).

## M1.5 — offscreen drawing

The plan asks for "a `BBitmap` back-buffer with static layers cached and only
the playhead and meters redrawn" in `TimelineView`, `PianoRoll` and
`MixerWindow`, against a budget of 4 ms per frame on a 32-track, 300-clip
project.

**What was measured first (2026-10-09, VM):**

- Play start on 32 tracks / 320 clips: **3.1 s**. The plan's budget is 300 ms;
  the fix is M4.1 (the engine graph and every clip stream is built on the window
  thread today). Recorded, not fixed here.
- Timeline draw time: **not measurable on the VM** — its screen is locked, so
  the app_server never asks the window to draw, and M0.7's
  `DAW_TIMELINE_TIMING` instrument (which reports every 60 draws on stderr)
  produced nothing. The measurement needs the hardware box (192.168.1.186) or
  an unlocked VM screen.

**Why the back-buffer is not simply written:** the drawing code in all three
views draws into `this` (unqualified `SetHighColor`/`FillRect`/…): there is no
place to redirect it. Caching into a bitmap therefore needs the drawing helpers
to take a target `BView*` — a mechanical but wide change (~500 call sites in
`TimelineView.cpp` alone). Before paying that, measure: M0.7 already culls
lanes and grid lines against the update rect and invalidates only the playhead
columns and the meter strip at 60 Hz, so what a back-buffer would save is the
*static* part of a *narrow* repaint. The instrument exists to say whether that
is 0.2 ms or 3 ms.

**Definition of done for this item:** the number, from the hardware box, in the
record — and then either the back-buffer (with the `BView*` target refactor) or
a recorded decision that the measurement does not justify it. Not "the code was
restructured and we hope it is faster".

### The protocol (the number still does not exist)

Everything below is for Marc to run: the VM and the hardware box are his, and a
locked screen draws nothing (HANDOFF, *a locked VM screen draws nothing*), so
neither can be driven from the host.

**Where.** 192.168.1.186, screen unlocked, is the primary machine — it is what
the plan names. The VM works too as long as its screen is unlocked; a locked
VM produces no `timeline draw:` lines at all and the run looks like a failure
of the instrument.

**What.**
1. *The suite route (no session of that size needed).* `ui_functional_tests`
   ends with `TestBigProjectPlayback`, which builds 32 tracks x 10 clips (320
   clips of a 1 s tone in `/tmp/haiku_daw_ui_big.wav`), plays it, and lets the
   timeline draw for ~2 s before stopping — about 120 `TimelineView::Draw`
   calls, so two report lines:
   ```sh
   DAW_TIMELINE_TIMING=1 ./ui_functional_tests 2>&1 | grep "timeline draw"
   ```
   Before the run, delete `~/config/settings/HaikuDAW/settings` and any leftover
   `recovery.dawproj`: a stale settings file fixes the UI scale for the whole
   run, and a leftover recovery file is a modal alert in front of it
   (`docs/HANDOFF.md`, both lessons).
2. *The long route (what the number really wants).* A real 32-track /
   300-clip session on the box, the app started from a Terminal with
   `DAW_TIMELINE_TIMING=1` in the environment, playing for a minute or more
   with the playhead sweeping. This gives dozens of report lines instead of
   two, and it is the only run that shows what a real session costs.

**What to record**, in this file:
- machine, date and Haiku revision; the project's shape (tracks, clips, sample
  rate) and how long the transport rolled;
- every `timeline draw: X.XX ms average over 60 draws` line, or — when there
  are many — the count and the min/max/average of those averages. Each line is
  the mean of 60 draws, so the tail is not visible in it;
- whether follow-playhead was on (a sweeping playhead invalidates a column each
  60 Hz tick, which is the case the back-buffer would target), which other
  windows were open, and whether anything else ran on the box at the time;
- the transport's own `DSP` and `XR` readouts over the same window, as the
  second witness (a draw that misses its frame shows up as DSP load or xruns);
- the verdict against **the budget: 4 ms per frame** while idle-playing, the
  number `docs/PLAN_1.0.md`'s verification section sets.

**What the number decides.** At or under budget, the back-buffer stays
unbuilt and this file carries the measurement and Marc's decision (M0.7's
culling already clips lanes, grid lines and the waveform to the update rect, so
what a back-buffer would save is the static part of a narrow repaint). Over
budget, the work is the `BView*` target refactor across the three views
(~500 call sites in `TimelineView.cpp` alone) plus the buffer — so the numbers
have to be worth that, which is the whole point of measuring first.

No back-buffer is written in this package, and no measurement is run in it:
both are Marc's call after he sees the figures.

## M1.6 — icons

**Done:** `src/ui/widgets/DawIcons.h` draws the tool palette's glyphs (pointer,
pencil, brush, eraser, scissors, glue, velocity) and the transport glyphs with
the drawing API inside a unit square — vector, colour-agnostic, sharp at any
size. The MIDI editor's toolbar is icon-only with hover tooltips.

**Not done, and why:** the HVIF app icon and the project-file icon. `rc` only
compiles icons from raw HVIF bytes (confirmed by decompiling MediaPlayer's
resource: `resource vector_icon array { $"6E6369660D…" }`), and this machine has
no way to author one — no Icon-O-Matic, no `libicon` (only `libiconv`), no
`hvif2png`, and the HVIF byte layout is not something to guess at. Options for
whoever picks this up:

- author the icon on the hardware box with Icon-O-Matic and commit the
  resulting `.hvif` byte array into `haiku-daw.rdef.in`;
- or write the icon as a legacy `#'ICON'` (bitmap) resource, which Haiku still
  displays and which can be generated from a PNG by a small script.

Either way the icon belongs in the rdef, and the recipe's `PROVIDES`/install
step should not need to change.

**Where the artwork stands now (2026-10-12).** The drawing and the plumbing are
in, and only the conversion step is left: `data/icons/icon-app.svg` and
`data/icons/icon-project.svg` are the two icons drawn to Haiku's guidelines
(64 x 64, 2:1 isometric, key light top left, 1 px silhouette outline, floor
shadow to the right, mixed colours, nothing invisible at 32 x 32), and
`data/icons/README.md` is the conversion recipe — Icon-O-Matic: import the SVG,
File > Export > "HVIF RDef source", paste the byte string where the marker says.
`haiku-daw.rdef.in` carries the two commented `resource vector_icon array` /
`resource(1, "dawproj_icon") #'VICN' array` blocks so that step is mechanical,
and `src/main.cpp`'s `RegisterProjectMimeType()` already loads the file-icon
resource and hands it to `BMimeType::SetIcon` (a no-op until the bytes exist,
so the two icons can land separately). What is still owed: the bytes, and the
Tracker/Deskbar check at 16, 32 and 64 px that T12's definition of done asks
for — both need a Haiku machine with Icon-O-Matic.

## Verifying either half

Both change what the screen shows. Before calling either done, run the
screenshot pass in `docs/UI_GUIDELINES.md` (section 1) and look at every shot
the change touches; for the back-buffer, compare the shots against a run
without it — they must be identical.
