# Instructions for coding agents

This file is for any model implementing changes here (Claude, DeepSeek or
another). Read these before writing code:

1. `docs/HANDOFF.md` — the project, the build, the host ⇄ VM dev loop, the
   hard-won lessons.
2. `docs/agent-prompts/README.md` — the package plan, the status table and the
   rules every package follows. Your task brief is one of the files beside it.
3. **`docs/UI_GUIDELINES.md` — before touching anything under `src/ui/`.**
4. Starting a session with no other instructions? Your entry prompt is
   `docs/agent-prompts/24-continue-1.0-ENTRY.md`; the plan it works through is
   `docs/PLAN_1.0.md`.

Non-negotiable:

- **UI work is done only when you have looked at it.** Run the `DAW_UI_SHOTS`
  screenshot pass on the VM (unlocked screen), open every shot of every window
  you changed, and fix what is wrong. Passing tests do not prove the screen is
  right — a milestone once merged at 172 green checks with the main window
  visibly broken.
- **Never change the operating system's settings** — no `set_ui_color`, no
  system preferences. The DAW themes its own windows only; the default look
  is native Haiku, with an optional dark mode (`docs/UI_GUIDELINES.md` §5).
- Kit-free code is host-tested (`ctest --test-dir build-host`); Haiku-only code
  is verified on the VM (`sh scripts/vm.sh build`, then the suites there).
- Every P0/P1 fix ships a regression test.
- Commits: author `Marc Korte <marc@duskaudio.com>`, conventional-commit
  subjects, and **no** `Co-Authored-By:` or other AI-attribution trailer, in
  commits or PR text.
