# Task: Layout Kit and docking (plan M1.4)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/layout-kit` off master (M0, M1.1, M1.2, M1.3 complete). Every window is
still positioned by hand: fixed rectangles that do not grow with the controls
inside them (so a 150% font clips them), no panes, and each auxiliary window
makes a new instance of itself every time it is opened.

## Work items

1. **Every dialog builds with `BLayoutBuilder`** and sizes itself to its
   contents (`ResizeToPreferred`), so a larger font grows the dialog instead of
   clipping it. Rename, Quantize, Export, Export-progress, Sends and Instrument
   were the six that had hand-placed rects; Sends and Instrument rebuild their
   contents at runtime, so their roots are `BGroupView`s (a group layout takes
   children at any time).

2. **The main window gets panes**:
   - a `BSplitView` row: the inspector beside the timeline. The inspector is
     resizable by its splitter, collapsible, and toggled with
     View > Inspector (key **I**);
   - a vertical split under it: the row above, over a docked bottom pane
     (a `BTabView`) that hosts the MIDI editor and starts collapsed. The dock
     toggles with View > Editor & Browsers (key **J**);
   - the docked editor opens when a MIDI region is double-clicked, with a
     **Pop out** button that hands the same region to its own window.
     `TimelineView` no longer creates that window itself: it posts
     `kMsgOpenEditor` and the main window decides;
   - pane sizes and visibility persist in `AppSettings` (`uiinsp`, `uibottom`),
     covered by the host-side round-trip test.

3. **Single-instance windows:** the mixer, the effects editor and the plugin
   browser show and activate the window they already have instead of opening a
   second one.

4. **The mixer scrolls horizontally** when the strips do not fit.

5. **Dialogs feel like dialogs:** modal or floating window feels where the
   window is a prompt.

## Rules

- Haiku-only: `scripts/haiku_syntax_check.sh` (0 FAIL) and the VM suites are
  the checks; the host suite must stay green and unchanged except where a
  kit-free piece (AppSettings) grows.
- No behaviour changes beyond the layout itself: the messages a window posts,
  and what it does with them, stay exactly as they were. `ui_functional_tests`
  drives these windows by posted message; those checks are the contract.
- New layout code uses `Themed()` metrics, like everything else (M1.2).
- `ui_functional_tests` grows deliberate checks: the pane toggles, the docked
  editor opening and popping out.

## Definition of done

- VM `build` and `build-off` ctest green with `ui_functional_tests` at the new
  check count; host suite green; syntax check 0 FAIL.
- PR record `docs/agent-prompts/22-layout-docking-PR.md`; the status table in
  `docs/agent-prompts/README.md` gets the item.
- A click list for the parts that need eyes: the panes at 100% and 150% font,
  the splitter drag, the dock's editor, the single-instance windows.

> **Superseded (2026-10-10).** This package merged at 172 green checks with the
> main window visibly broken — the transport bar stretched to a third of the
> window, the dock was an unlabelled tab over a stretched button, the dB
> readout sat inside the Mute row. A click list deferred the looking to Marc,
> and nobody looked. Any UI package now follows `docs/UI_GUIDELINES.md`: the
> implementer reviews screenshots of every window it touches before calling it
> done.
