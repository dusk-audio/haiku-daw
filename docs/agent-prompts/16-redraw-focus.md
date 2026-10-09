# Task: Redraw performance and keyboard focus (plan M0.7)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/redraw-focus` off master. Last M0 item: a busy project redraws far more
than it shows, and the transport keys only work if the timeline happens to have
focus.

## The state (verified)

- `TimelineView::DrawClipWave` loops a column per pixel of the WHOLE clip (up
  to the whole timeline) with one `StrokeLine` per column, visible or not.
- `DrawLanes` walks every track and draws every lane and every grid line
  through it, ignoring the `update` rect it is handed; grid lines are stroked
  even when off the view.
- `SetTrackPeaks` (called at 60 Hz while playing) does a full `Invalidate()`,
  although the peaks are only drawn in the header meter column.
- `ComputeCrossfades` is called per lane per DRAW (it allocates a vector each
  time), though it only changes when the clips change.
- Keys: `TimelineView::KeyDown` already handles Space (transport), arrows,
  zoom, Home, `l`; but the timeline only has focus after a click, so once a
  text field (the tempo field) has been touched, Space goes into the text
  view and the transport keys are dead.

## Work items

1. **Clip the waveform to what is visible**: pass the update rect down
   (`DrawLanes` → `DrawClip` → `DrawClipWave`) and loop only the columns inside
   it (and inside the content area); draw them with `BeginLineArray` in chunks
   (one app_server call per chunk, not per column).
2. **Cull lanes** whose rect does not intersect the update rect, and skip grid
   lines that fall outside it (still iterating, but no drawing).
3. **`SetTrackPeaks`/`ClearTrackPeaks` invalidate only the header column**
   (x < `kHeaderWidth`) — the only place the peaks are drawn.
4. **Cache crossfades per track**: recompute only when the clip tuples it
   depends on (`startFrame`, `lengthFrames`, `fadeInFrames`, `fadeOutFrames`,
   `takeGroup`) differ from what the cache was built from — an exact O(n)
   comparison with no allocation, against an O(n) allocation per draw.
5. **Keyboard focus**:
   - `MainWindow::DispatchMessage` reroutes a `B_KEY_DOWN` with NO command
     modifier to the timeline whenever the current focus is not a `BTextView`
     (a focused text field must keep its keys — a space is a space there);
     Cmd-shortcuts keep flowing to the menus.
   - `TimelineView::MouseDown` takes focus (`MakeFocus`), so a click on the
     timeline makes the transport keys work again.
6. **`ui_functional_tests`**: with no text focus, a posted Space toggles the
   transport (a new public `MainWindow::IsPlaying()` for the test); with the
   tempo field focused, the same Space does NOT; a posted `B_MOUSE_DOWN` on the
   timeline leaves it the window's current focus.

## Definition of done

- Host suite green (nothing kit-free changes; counts recorded);
  `sh scripts/haiku_syntax_check.sh` 0 FAIL; VM `build` and `build-off` ctest
  green with the new ui checks.
- The plan's draw-time check: `DAW_TIMELINE_TIMING=1` makes `TimelineView::Draw`
  log its own draw time (system_time around the body, a few lines a second), so
  the "under 4 ms per frame while idle-playing" claim can be measured on real
  hardware instead of asserted. Measure what the VM allows and say so in the
  record.
- PR record `docs/agent-prompts/16-redraw-focus-PR.md`, including what the
  timing flag measured and what still needs Marc's eyes/hardware.
