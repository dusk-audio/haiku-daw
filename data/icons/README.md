# Icon artwork

Source artwork for the app icon (`icon-app.svg`) and the `.dawproj` file icon
(`icon-project.svg`). These are drawing sources, not what ships: nothing here is
installed by CMake or the recipe. What ships is the HVIF byte string inside
`haiku-daw.rdef.in`, compiled into the `daw` binary by `rc`, plus — for the file
type — the `META:ICON` the app writes into the MIME database at startup.

Both are drawn to Haiku's icon guidelines
(<https://www.haiku-os.org/development/icon-guidelines>): a 64 × 64 canvas,
2:1 isometric geometry with straight verticals, the key light from the top left
with a cooler ambient from the right, a black 1 px outline on the silhouette
(2 px at 64), a floor shadow cast straight to the right and clear of the canvas
edge, colours mixed rather than pure, and no detail that is invisible at
32 × 32. The fader caps are the one bright accent so the app icon still reads as
a mixing desk at 16 × 16.

## SVG → HVIF (the step that needs a Haiku machine)

Only Icon-O-Matic can author HVIF, and it runs on Haiku: neither the Linux
development host nor the CI has anything that can (no `libicon`, no
`hvif2png`; `rc` compiles icons from raw HVIF bytes only). This is Marc's step.

For each file:

1. Copy the SVG to a Haiku machine and open **Icon-O-Matic**.
2. **File ▸ Open…** and pick the `.svg` (Icon-O-Matic imports SVG paths,
   linear gradients and shapes; see the notes below for the few things it
   cannot carry over).
3. Check the result against the guidelines' checklist at 32 × 32 and 16 × 16
   (Icon-O-Matic's own view window has the size menu), and touch up the
   perspective last — the SVGs are a first pass, not a finished artwork.
4. **File ▸ Export ▸ "HVIF RDef source"** and save.

Then, for the **application icon**:

* Open `haiku-daw.rdef.in` at the commented `resource vector_icon array` block
  (the header comment says where), paste the exported byte string where the
  marker says, and uncomment the block. Nothing else changes: Deskbar, Tracker
  and HaikuDepot read that resource off the binary, and the CMake post-build
  step already runs `rc`/`xres`/`mimeset` (and now fails the configure instead
  of silently skipping, M8.1).

And for the **`.dawproj` file icon**:

* Paste that icon's byte string into the second commented block in the same
  file, the one named `dawproj_icon` (`#'VICN'`). That one is not read off the
  binary by anything: a file type's icon lives in the MIME database, so
  `main.cpp`'s `RegisterProjectMimeType()` loads the resource and hands it to
  `BMimeType::SetIcon`. It is a no-op while the bytes are absent, so the two
  halves can land separately.

Verify with Tracker (both icons at 16, 32 and 64 px), the Deskbar's
Applications menu, and a saved `.dawproj` — that is T12's definition of done in
`docs/agent-prompts/24-continue-1.0-ENTRY.md`.

## What Icon-O-Matic may not carry over

Icon-O-Matic imports paths, rectangles, linear/radial gradients and transform
styles, but not everything an SVG can express. Expect to flatten or redo:

* the `opacity` on the shadow group — HVIF styles carry their own alpha, so
  either set the shadow's alpha in the style or ungroup and re-alpha;
* the stroke-only polylines (the silhouette and the creases) — HVIF wants a
  filled or stroked *path* with a style; stroking works, but a stroke that is
  half a pixel wide (the caps, the fold) may need rounding to 1 px;
* the two-stop gradients are fine; a gradient *angle* per shape has to be set
  by hand after import (the SVG uses axis-aligned ones on purpose);
* the `<title>` elements are metadata, not geometry.

Keep the shape *ids* (`console`, `faders`, `fader-1`…, `overlay`) if the
import keeps them: they are what makes it obvious which paths are LOD
candidates in Icon-O-Matic (the guidelines suggest hiding the fader details at
small sizes with Min/Max LOD — only if the 16 × 16 view needs it).

## If a legacy bitmap icon is wanted instead

Haiku still displays the old bitmap icons, and a raster is the cheaper route if
Icon-O-Matic is not an option on the day (this is the option recorded in
`docs/agent-prompts/23-offscreen-and-icons.md`). Then:

* Render each SVG to PNGs at 32 × 32 and 16 × 16 from any vector tool;
* convert those to the legacy resource forms, `#'ICON'` (large, 32 × 32) and
  `#'MICN'` (mini, 16 × 16) — `rc` compiles raw bytes and does not convert
  images, so the conversion is a small script that writes the icon's header
  and pixels; the byte strings then go into the rdef where the `vector_icon`
  block would go;
* the app icon works by resource alone, exactly as the vector one does; for the
  `.dawproj` type, hand the same bytes to the MIME database — either through
  the `SetIcon` call above (the bitmap overload takes a `BBitmap*` and an
  `icon_size`, not raw bytes) or once by hand in the FileTypes application.

A bitmap icon cannot scale, which is why the guidelines and the recorded
decision ask for HVIF; use this only as a fallback.

## Notes for whoever draws the final version

* Both SVGs are geometry and colour only — no font, no filter, no clip path —
  so that what Icon-O-Matic imports is what the SVG shows.
* The shadow is drawn as a shape rather than an alpha gradient on purpose: it
  survives a converter that drops group opacity.
* If the artwork changes, the two SVGs are the source of truth for the shapes;
  re-export to HVIF after editing, and do not hand-edit the byte string.
