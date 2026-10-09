# feat: draw only what is visible, and let the keys reach the timeline (M0.7)

Branch `feature/redraw-focus` off `master` (`7a4be4b`). Commits: the spec, the
change, two test fixes the VM found.

## What changed

**Drawing.** `DrawLanes` now skips lanes that do not intersect the `update`
rect it is handed (the index still advances — it names the lane, not the row on
screen) and does not stroke grid lines outside it. `DrawClipWave` loops only the
columns inside the intersection of the clip, the update rect and the content
area — a clip can span the whole timeline, and the loop used to walk all of it
with one `StrokeLine` per column — and emits its columns through
`BeginLineArray` in 256-line chunks (Haiku's line array carries the colour per
line; `SetHighColor` does not apply inside it). `SetTrackPeaks`/
`ClearTrackPeaks`, called at 60 Hz while playing, invalidate the header column
only — the one place the peaks are drawn — instead of the whole timeline.
`ComputeCrossfades` is cached per track against the exact clip tuple it was
built from (`startFrame`, `lengthFrames`, `fadeInFrames`, `fadeOutFrames`,
`takeGroup`): an O(n) comparison with no allocation instead of one allocation
per lane per draw; `SetProject` clears the cache.

**Keys.** `MainWindow::DispatchMessage` hands a `B_KEY_DOWN` with no command
modifier straight to the timeline whenever the focus is not a text field
(`BTextView`/`BTextControl` — a space is a space while the tempo field is being
edited); Cmd shortcuts keep going to the menus. Haiku's `BMessage` has no
retarget verb, so the message is delivered to the timeline directly.
`TimelineView::MouseDown` calls `MakeFocus(true)` (the view is already
`B_NAVIGABLE`), so one click makes the transport keys work. New public
`MainWindow::IsPlaying()` for the tests. `DAW_TIMELINE_TIMING=1` makes
`TimelineView::Draw` log its own average draw time (every 60 draws) — the
plan's "under 4 ms per frame" claim is to be measured with it, not asserted.

## What is verified, and how

- Host 52/52; `haiku_syntax_check.sh` 0 FAIL on `MainWindow.cpp`,
  `TimelineView.cpp`, `ui_functional_tests.cpp`.
- **VM `build` 54/54 and `build-off` 50/50**; `ui_functional_tests` at 141
  checks, 0 failures, including the new `test_keyboard_focus`: a click on the
  timeline takes the focus, Space then toggles the transport (twice), the tempo
  field keeps its space (the transport does not move), and the click takes the
  focus back.
- The test found two things worth the record: the *first* version posted the
  click to the window, and a posted window-level mouse message is NOT
  hit-tested onto the view under its `where` — the focus stayed on the tempo
  field's `_input_` text view (the diagnostic printed it) — and the harness's
  way is to deliver the message to the view itself. Second, a Space that looks
  dead may be a correct refusal: the project the test inherited had its only
  clip deleted by the previous test, so `StartPlayback` reported an audio
  failure through the new `ReportError` and the transport (rightly) stayed
  stopped.

## What is NOT verified

| Claim | State |
| --- | --- |
| The draw timings (the plan's 32-track/300-clip, under 4 ms/frame) | **Not measured.** The engine builds one disk thread per clip, so a 96-clip fixture takes longer to PLAY than a test should wait — that is M4.3's subject, and "idle-playing" is what the target means. An invalidate-driven substitute destabilised the suite and was removed rather than kept as noise. The instrumentation is in place: run the app on real hardware with `DAW_TIMELINE_TIMING=1` (playing a normal project) and read the `timeline draw:` lines from stderr. |
| The waveform still looks right | Clipping and the line-array rewrite change how the same columns reach app_server; no automated check can see a waveform. Click line for the hardware pass: open a project, look at the clips' waveforms (zoom in and out, scroll a long project), compare against a screenshot from before this branch. |
| The perf edits on a large project | Same click line, plus the timing flag above. |
