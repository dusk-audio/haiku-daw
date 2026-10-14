# UX-pass follow-ups — PR record

Branch `fix/ux-pass-followups`, cut from `master` after PR #22 (the UX pass,
packages 49–52) merged. Four commits: `bf97697`, `ed9e468`, `0774703`, `e28f22b`.
The base is the merge itself, so the PR diff is exactly these four.

Everything here came out of two bug reports and one review:

1. "choosing a different option in Preferences crashes the app" (before #22),
2. a CodeRabbit pass over the branch (`coderabbit review --agent`),
3. "input monitoring does not work on a record enabled track", then
4. "record and input is enabled, yet I do not see any levels on the meters".

## 1. The crash, and why the suite had not caught it (`bf97697`)

The crash is real and has one shape: `BMenu::_Show` calling
`fSuper->Bounds()` on a window the pop-up's tracking thread does not own —
"Looper must be locked", and Haiku stops the team. The VM's syslog carries the
stack from the report:

```
DEBUGGER: Looper must be locked.
  BMenu::_Show + 0x4b            <- fSuper->Bounds()
  BPopUpMenu::_StartTrack + 0x61
  BPopUpMenu::_thread_entry + 0x3a
```

It was reproduced on the VM against the pre-fix call (`Go()` on the field's own
menu) and shown absent from the shipped one (a fresh, unparented pop-up), both
with a throwaway probe linking the app's own `daw_ui`.

What PR #22 left undone was the coverage: `TestPreferences` marked menu items
directly, so no pop-up, no tracking thread and no pick were ever exercised —
exactly the gap the report came out of. **A pop-up CAN be driven by a test**,
and the recipe is now in `docs/HANDOFF.md` beside the other test-driving traps:

- a posted mouse-down on the FIELD opens it (`BPopUpMenu::Go` tracks on a
  thread of its own, so the window's thread sits in `Go()` until it closes);
- its window is a `BMenuWindow`, which `be_app->CountWindows()` filters OUT —
  find it through its looper (`get_next_thread_info` +
  `BLooper::LooperForThread`, matched on the `"w>" + title` thread name);
- take the row with the kit's own keys (Down moves, Return takes:
  `BMenu::KeyDown` → `fChosenItem` → the item `Go()` returns), posted with the
  FOCUS token;
- wait out `get_click_speed()` (a pick inside it is read as the opening click
  and thrown away), and count the Downs from the row under the REAL pointer
  (`BMenu::GetMouse` + `BMenuItem::Frame`), because the tracking loop reads the
  pointer and the buttons from the input server and never from messages.

`TestPreferences` now drives EVERY row of the window the way a user drives it:
each menu field's row through its own pop-up, the level through the slider's own
keys (a click takes its value from wherever the real pointer is), the three
ticks through real clicks (down AND up), the folder row through the message its
panel posts, the disabled output-device row through a click that must open
NOTHING. Each result is held to the app's state, to the running engine, to the
timeline's monitor lamp, to the autosave runner and to a reload of the settings
file; the restore puts every row back by value.

Two things were found on the way: a DISABLED `DawMenuField` opened its menu on a
click (the stock field's guard was its hidden bar's enabled state), and the
suite's clean-slate removals had been no-ops since the day they were written
(`BPath::Append` appends — appending a leaf to `ProjectDocument::SettingsPath`
gives `…/settings/recovery.dawproj`, which never exists), so a run inherited the
last one's ui scale, panes and window frame. Both fixed; the suite now really
starts from a clean slate, before the application object exists, and answers a
`Recover?` prompt that still appears with Discard.

## 2. The review's findings (`0774703`)

Six findings, each checked against the code first. All six real:

| # | Where | What |
|---|-------|------|
| 1 | `MainWindow::kMsgApplyMidiOp` | `base` declared and never read: a transform recorded base 0 in the roll gate, so a same-turn gesture could not match its batched transition and was refused. Its two sibling handlers read the field; the events one even carries the comment describing this bug. |
| 2 | `ui_functional_tests` × 5 | `LockWithTimeout` result ignored, then `Unlock()` — on a timeout that is `Unlock()` on a looper the test thread does not own (the debugger call this file warns about) and a command executed without the lock. Guarded, with the shutdown's `Quit()`. |
| 3 | `PlayMeterFixture` | Cleared and re-recorded its own snapshot on the SECOND call, so the restore put every track back MUTED — the leak the snapshot exists to prevent. |
| 4 | `TestRenderFeedback` | Left every earlier track muted and twelve 120-second synth tracks behind it. Now records what it changes, restores it and removes its own tracks through the command stack. |
| 5 | the recovery-file removal × 2 | The `SettingsPath`/`Append` no-op above. |
| 6 | `PluginBrowser` | `SendMessage(new BMessage(…))` — `BMessenger::SendMessage(BMessage*)` copies and does not take ownership: one leak per click on a row with no selected track. |

Also from the same runs: `TestThemeScale` now restores the scale override it
found instead of hard-coding "back to the real font" (the override is
process-wide, and leaving it at the font ratio while the app still holds 125%
made the next test that applies a scale check nothing), and the Preferences test
picks a scale row that DIFFERS from the one the run inherited, for the same
reason — the app skips a scale it already holds.

## 3. Input monitoring (`ed9e468`, `e28f22b`)

Two reports, two layers.

**"Does not work on a record enabled track."** An audio track's `I` button did
nothing at all outside a take: the only `IMonitorSource` was the take's
Recorder, and the idle monitor (every arm/I change) handled MIDI tracks only —
so the one control whose meaning is "hear the input live w/o record-enable" was
dead for audio until Record was pressed. `Recorder::StartMonitor` adds a
monitor-only capture (same device resolution, same negotiated format, same
monitor ring, no take files, no disk thread), `UpdateIdleMonitor` grows the
audio branch (the FIRST audio track whose `I` is on — one capture, one stereo
pair, the same rule as "the first armed track names the device"), and the
teardown follows a take's (detach the source, quiesce, free).

**"No levels on the meters while talking."** Three gaps:

- the mix reached the master and nowhere else — every per-track meter (lane
  header, inspector VU, mixer strip) reads that track's own bus peak, and the
  monitor mix goes straight to the master by design;
- an armed audio track with NO clips had no bus to meter on at all (audio buses
  are built from clip streams; the MIDI side has always kept one for an armed or
  monitoring track);
- nothing named the track the input belonged to.

`Engine::SetMonitorTrack` + `Graph::MixMonitorInput` publishing the input's own
peak on that bus (both RT paths, measured on the samples just mixed) + the bus
rule for armed/monitoring audio tracks fix all three.

One more, found by the regression test: a monitor-only graph renders through
`RenderMonitorOnly`, and that RT body never mixed the input — the mix lived in
the playback path alone, so the idle monitor raised a graph that rendered the
live MIDI voices and nothing of the input it was raised for. **A monitor-only
graph is the only thing rendering while it is up**; `MixMonitorInput` is now
shared by both RT bodies. And the piano roll's audition idle timer could stop a
monitor session it did not own (its guard asked the transport, not the engine) —
it now asks `InputMonitorAttached()`.

## Verification

- VM `ui_functional_tests`: **1276 checks, 0 failures**, plain and in both
  screenshot modes (107 shots each, opened during the run — the Preferences
  window, its pop-ups in System and dark, and the pop-up's marked row).
- The screenshot pass for THIS branch was the plain run's: nothing under
  `src/ui/` draws differently in commits 2–4 (the meters' numbers move, not
  their geometry), and commit 1's pop-up shot was reviewed in both modes.
- Host `ctest --test-dir build-host`: 78/78.
- The tests that carry the weight: `TestPreferences`'s `pickRow` (the crash
  path), `TestInputMonitor` (a fake `IMonitorSource` through the engine's own
  API — 0.4 arrives as 0.4 on both the master and the track's meter, half-rate
  as 0.5 — plus the idle session driven through the real button message).

## What no machine here can verify

The capture half. Neither the host nor the VM has a real input device, so
audible monitoring, the take's own input metering and the multichannel routing
stay on Marc's click list, on the machine with the interface:

1. arm an audio track, `In` = a live input, `I` on: hear it at once, no record;
   the lane header meter, the inspector VU, the mixer strip and the master all
   move; the transport says "Monitoring input".
2. the same track with the transport recording: heard and metered through the
   take.
3. `I` off (and the Preferences row off): silence, meters at rest.
4. a MIDI track's `I` with a keyboard: live notes, unchanged.
5. two audio tracks with `I` on: only the FIRST one's input is monitored and
   metered — one capture, like a take's one device.

## Housekeeping on the dev VM

Both `.dawproj` recovery files and the settings file are deleted by a suite run
now (before the application object exists) — that is the clean slate the suite
always claimed, and it means a run wipes `~/config/settings/HaikuDAW/settings`
(window frame, panes, preferences), which the app rewrites with defaults. The
`theme` key was left at `system` after the dark screenshot pass, which is what
the machine had before.
