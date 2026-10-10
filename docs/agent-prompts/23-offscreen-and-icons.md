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

## Verifying either half

Both change what the screen shows. Before calling either done, run the
screenshot pass in `docs/UI_GUIDELINES.md` (section 1) and look at every shot
the change touches; for the back-buffer, compare the shots against a run
without it — they must be identical.
