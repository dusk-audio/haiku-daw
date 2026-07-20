# Session entry — package 04 (MIDI tools: quantize, swing, humanize, legato)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. This file is the entry prompt; it carries the verified repo state and the deltas since the task doc was written. Read in this order:

1. This file.
2. `04-midi-tools.md` — the task itself. Still accurate in substance; corrections below where the codebase has moved under it.
3. `03-inserts-ui-RESUME.md` — sections "Environment, exactly as it works today" and "Rules this branch has already paid for", if and when you touch `src/ui/`. The build/VM recipes there are current.

## State of the repo (verified 2026-07-20)

- Master HEAD `5a09823`. Packages 01 (fx-inserts-core), 02 (lv2-host) and 03 (inserts-ui) are all merged. Host suite green on master: **46/46** (`cmake -S . -B build-host && cmake --build build-host -j && ctest --test-dir build-host`).
- Package 07 (LV2 live editor) may be running **in parallel** in another session on branch `feature/lv2-live-editor`. Coordination rules below.
- Branch for this package: `git checkout -b feature/midi-tools` off master. Conventional commits (`feat(midi): ...`).

## Corrections to the task doc (codebase moved since it was written)

- Test count is 46, not 40; the `add_executable`/`add_test` block in `CMakeLists.txt` has grown past the quoted line numbers. Follow the pattern (one standalone `int main()` + `CHECK` macro per file, linked against `daw_model`), not the line references.
- Package 03 added region-editing machinery you must respect, not duplicate: MIDI regions now grow to cover notes drawn past their end, left-edge trims exist, and `TrimClipFrontCommand` (`src/model/Commands.h`) rebases clip-relative note/controller content on a front trim and re-sorts the clip list. Your transforms operate **within** a clip on clip-relative frames; if a quantize pushes a note earlier than frame 0 or past the clip end, clamp to the clip — do not resize the region as a side effect.
- `tests/model_tests.cpp` grew ~137 lines in package 03; skim its newest tests before adding command tests so yours match the current idiom.
- Before binding the `q` shortcut in the piano roll, enumerate the existing `KeyDown` handler (`src/ui/PianoRoll.cpp`) — tools already claim `1`-`7` and several letters; pick a free key if `q` is taken and record the choice in your PR doc.

## Parallel-work coordination (07 may be live)

- Fully safe: everything kit-free — `src/model/MidiOps.*` (new), `src/model/Commands.*`, tests, CMake. This is ~80% of the package; do it first and land it green.
- `src/ui/PianoRoll.cpp` / `src/ui/TimelineView.cpp`: package 07 does not touch them. Safe.
- `src/ui/MainWindow.cpp/.h`: package 07 **does** touch it (editor entry points). Keep your footprint there minimal — new message constants and a thin handler block — and rebase on master before merging if 07 lands first.
- The Haiku VM belongs to package 07 while it runs. Your Haiku-only code is small (menu + shortcut wiring); write it pattern-faithfully, mark it compile-unverified in your PR doc, and queue a VM compile check for when the VM frees up. Do not sync files onto the VM while 07's session is mid-verification — you would overwrite its loose-file state (the VM is a working copy, not a git remote).

## Definition of done

Everything under "Definition of done" in `04-midi-tools.md` (the `midiops_tests` coverage list is the core of it), plus:

- All 46 existing host tests still green; `midiops_tests` added and green, including the mid-clip tempo-change case built on a real `TempoMap`.
- Mutation-test the key assertions (break the code, watch the test fail, revert) — the discipline the 03 branch established; it caught two hollow assertions there.
- A `04-midi-tools-PR.md` in this directory: semantics chosen (grid/strength/swing), shortcut chosen, what is compile-unverified on Haiku, and what a future groove-template feature would need.
