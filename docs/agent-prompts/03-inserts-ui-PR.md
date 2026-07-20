# feat(ui): insert slots, plugin browser, generic parameter editor

Package 03. Branch `feature/inserts-ui` off `408607b` (package 02), 13 commits.

Everything here is Haiku-only UI. It **cannot be compiled on the Linux host**, so
every commit was compiled on the Haiku VM in both configurations before being
claimed. Nothing in this package was verified visually except where the user
sent a screenshot — see "What is NOT verified".

## Work items

| # | Item | State |
| --- | --- | --- |
| 1 | Insert slot list on the channel strip | Done (inspector); mixer strips NOT done — see gaps |
| 2 | Plugin browser | Done |
| 3 | EffectsWindow upgrades | Generic parameter panel done; per-effect bypass/mix header NOT done — see gaps |
| 4 | Instrument slot distinguished from inserts | Done |

## What the user sees now

**Channel strip (`InspectorView`).** The single "FX n" button is a slot list: one
row per insert with the effect's name and a bypass dot, plus a trailing empty row.
Bypassed rows draw dimmed. Dragging a row reorders the chain; a click that does not
move opens the editor; the empty row opens the browser. Capped at six rows, the
last reading "+N more..." and opening the editor, because an uncapped list would
push the fader and meter off the bottom of the strip.

The bypass dot posts `SetFxBypassCommand` — **built by package 01 and never used
by any UI until now**. It is a discrete toggle rather than a chain edit, so the
Edit menu reads "Bypass Effect" and each toggle undoes on its own.

The instrument slot moved to its own full-width row *above* the inserts, so the
one-instrument-plus-N-inserts model is visible rather than implied (item 4).

**Plugin browser (`PluginBrowser`).** One filtered list of everything insertable —
built-ins, add-ons, LV2 — narrowing as you type, Enter or double-click to insert.
Mirrors `SampleBrowser`, the existing `BWindow` + `BListView` + filter precedent.

**Effects editor.** Plugin-backed effects (`Plugin` and `Lv2`) draw a vertical
parameter list — label, slider, value, one row each — instead of the fixed knob
row. The knob row shows at most the five that fit across the panel, so a plugin
with more had the rest simply unreachable: 4K EQ 2 has 26 parameters and offered 5.
Double-clicking a row restores that port's default, since a generic list has no
per-parameter menu.

## Bugs found and fixed along the way

Four of these were only findable by looking at the thing running, which is worth
recording given how much of this package could only be compiled.

**LV2 inserts had no knobs at all.** `KnobsForDesc` returned nothing for
`EffectType::Lv2` because when it was written the build could not read LV2 port
metadata. Package 02 added that and deliberately gave `Lv2ParamInfo` the same
field names as `PluginParamInfo`; both types now derive knobs from one
`HostParamsFor()`.

**Zero-fill silenced plugins.** Both param-write paths grew `d.params` with
`resize(slot + 1, 0.0f)`. Zero is not a neutral filler: every LV2 plugin available
here exposes an `Enabled` control at slot 1 whose default is 1, so dragging a
higher knob zeroed it and the plugin rendered silence — indistinguishable from a
broken host. `EnsureParamSlot` fills invented slots from the plugin's own port
defaults; built-ins keep the zero fill, matching their documented layouts.

**Fractions written into toggles.** Visible on screen: 4K EQ 2's "M/S Mode", a
two-state control, sitting at 0.03. The plugin declares `lv2:toggled`,
`lv2:integer` and `lv2:enumeration`; the host read none of them. All three collapse
to one flag, now read in the scan, enforced in `Lv2Effect::SetParam` **and** snapped
by the editor. Enforcing it in the host too is deliberate — automation lanes and
project files carry values that never pass through a slider.

**A stale-fixture build bug that hid the fix.** The fixture's `.ttl` files were
copied by a `POST_BUILD` command hung off the `.so`, so they only refreshed when
the library relinked. Editing a `.ttl` left the bundle stale and the tests kept
asserting against old port metadata — the first attempt at the toggle fix looked
like it had failed. They are now proper build outputs with declared dependencies.

**Two self-inflicted ones**, caught before they shipped: a new hit kind assigned
`6`, which is already the FFT toggle (kinds are dispatched by bare integer, so it
compiled fine and would have silently routed every parameter slider into the FFT
handler); and the browser's spacebar passthrough aimed at the effects editor, which
does not handle `kMsgTransportToggle` — only MainWindow does.

## The reorder, and a review finding worth reading

Dragging an insert reorders the chain through the ordinary chain-replace command
(order is just the vector order), so it is one undo step.

Review flagged this as an off-by-one in the insert index. It is not: the arithmetic
was exhaustively simulated over every from/to pair, and the dragged item lands on
exactly the row it was dropped on in both directions. The real defect was that the
**drop indicator contradicted it** — a line drawn above the target row reads
"insert BEFORE this one", while the code makes the item *occupy* that row. The two
agree when dragging up and disagree when dragging down.

Fixed the drawing, not the arithmetic, because the arithmetic already matches how
the target is computed: `MouseMoved` takes the row under the pointer and clamps it
to the real inserts, so it names a slot, not a gap between slots. The target row is
now framed. Had the fix gone the other way, the code and the computation would have
disagreed instead.

A second hazard was found by self-review and fixed: the drag carried only row
indices, so a selection change between mouse-down and mouse-up would have applied
them to a *different* track's chain. It is now pinned to the originating track.

## Shared rather than duplicated

`EffectDisplayName` and `MakeInsertDesc` are declared in `EffectsWindow.h` and used
by both the editor and the strip. Both encode something that has exactly one
correct answer and would rot if copied: naming an LV2 insert needs a host lookup
(`pluginName` holds the URI, unreadable as a label), and seeding a new plugin
descriptor must use the host's port defaults rather than zeros, for the
`Enabled`-goes-silent reason above.

## Constraints honoured

- **No model or engine changes.** The only non-UI edits are in the LV2 host
  (reading three port properties it was ignoring) and its tests. `EffectDesc`,
  the commands, the engine and the exporter are untouched.
- All mutation goes through commands. The aux windows post; `InspectorView` runs
  on MainWindow's thread and owns the stack, so it executes directly — the pattern
  its existing input/output/sends handlers already use.
- No new compile-time dependency. `PluginBrowser` is added to the existing `daw`
  target; every LV2 reference is behind `DAW_HAVE_LV2`.

## Message flow

| Window → | constant | → handler | → command |
| --- | --- | --- | --- |
| PluginBrowser → EffectsView | `kMsgPluginChosen` | `EffectsView::MessageReceived` | (chain edit) → `kMsgApplyFx` → `SetFxCommand` |
| PluginBrowser → InspectorView | `kMsgPluginChosen` | `InspectorView::MessageReceived` | `SetFxCommand` directly |
| InspectorView bypass dot | — | `MouseDown` | `SetFxBypassCommand` directly |
| InspectorView row drag | — | `MouseUp` | `SetFxCommand` directly |
| EffectsView → MainWindow | `kMsgApplyFx` | existing handler | `SetFxCommand` |

## Verification

| Environment | Result |
| --- | --- |
| Haiku VM, LV2 enabled (lilv 0.24.20) | **46/46**, 0 errors, 0 warnings |
| Haiku VM, `-DDAW_LV2=OFF` | builds clean, 0 warnings |
| Linux host (model/DSP/tests only) | 46/46 |
| Linux `-DDAW_SANITIZE=ON` | 46/46, leak-clean |
| Linux `-DDAW_LV2=OFF` | 43/43 |
| Real Haiku hardware (i7-4960X) | **46/46**, 0 errors, 0 warnings |

Package 02's LV2 work also gained a render test this cycle (`lv2_render_tests`,
14 checks): LV2 inserts bounced through the real graph and compared sample-exactly,
including plugin-delay compensation across the LV2 boundary, which nothing had
exercised. Mutating the host to report zero latency fails it.

## What is NOT verified — read before trusting this

- **Almost none of this was seen running.** The author cannot reach `app_server`
  over SSH (`screenshot` fails with exit 69), so verification was: compiles clean
  on the target in both configurations, plus arithmetic checked directly where it
  could be (reorder simulated exhaustively; overflow counting tabulated for
  `nfx` 0..9). The user's screenshots confirmed the LV2 knob row, the 26-row
  parameter list and correct default seeding — everything after that is unseen.
- **Specifically unseen**: the plugin browser (filtering, Enter-to-insert, where
  the window opens), the slot list, the bypass dot, the reorder drag and its new
  drop indicator, and the instrument row.
- **Mixer strips do not have the slot list.** Item 1 asks for it in `MixerWindow`
  as well as the inspector. Only the inspector has it; the mixer keeps its
  existing fx button. Doing it there is a bigger change — the mixer is a separate
  looper with a snapshot, so bypass would have to post rather than execute.
- **No per-effect bypass/wet-dry header in the editor** (part of item 3). The strip
  can bypass; the editor cannot, and wet/dry has no UI at all. The task doc's
  analysis of why these must be commit-only still stands and is unaddressed.
- **Clicking a row opens the editor but does not scroll to that slot.**
  `EffectsWindow` has no API for it; adding one is more than this item warrants.
- **A chain of six or more inserts has no empty row on the strip**, so adding must
  go through the editor. The overflow row opens it.
- **The generic parameter panel will look useless for the JUCE-built LV2 plugins
  installed here**, which expose only `Free Wheeling` and `Enabled` as control
  ports; their real parameters travel as `patch:Set` atom messages, which v1 does
  not author. This is a property of those plugins, not of the mapping — 4K EQ 2
  built DSP-only exposes 26 real ports and displays correctly.

## Note for whoever picks this up

`scripts/hw_setup.sh` is new: one-shot bootstrap for a fresh Haiku install on real
hardware (`vm_setup.sh` assumes the repo is already present and installs no build
dependencies). `scripts/serve.sh` now defaults to port 9090 — the host's firewalld
drops 8000, so a clone from another machine *hangs* rather than being refused,
which reads as a fault on the Haiku end. Both are documented in place.

---

# Addendum: work after the original PR description

Everything above describes the package as first written. The branch has since
grown three areas, all driven by using the app rather than reading it.

## Native plugin editors

An LV2 insert opens the plugin's OWN GUI (`src/ui/Lv2UiWindow`), not the generic
parameter list. That list remains the fallback for built-ins, add-ons, and
plugins shipping no embeddable editor.

Getting there needed a port of DPF's Haiku pugl backend, which had never
rendered a frame on the platform: no `BGLView` was ever created, no events were
dispatched, and `puglUpdate` returned `PUGL_UNSUPPORTED`. The patch lives OUTSIDE
this repo at `~/projects/dpf-haiku-gl-ui.patch` (~1000 lines) because it belongs
to DPF, not the DAW. `prototypes/lv2_ui_host` is the standalone harness that
proved the mechanism before any of it touched the app.

Two rules that patch established, both of which cost real debugging time:
- **Never render from `BView::Draw()`.** It runs on the window's looper thread,
  which already holds the lock `BGLView::LockGL()` needs. The first expose
  deadlocks: window appears, stays black, process alive but wedged.
- **Input is queued on the looper and drained in `puglUpdate`.** Every other
  pugl platform delivers input from the drawing thread, and toolkits rely on it
  — Dear ImGui is explicitly not thread-safe.

**The editor is VIEW-ONLY.** It holds its own plugin instance seeded with the
insert's stored values, and says so in its title. Wiring it to the live instance
means letting a GUI touch an object the audio thread is using every block — an
RT-boundary decision left deliberately open rather than made silently.

## Region editing

- A MIDI region now GROWS to cover notes drawn past its end. It previously did
  not, and since the engine renders within region bounds those notes were kept
  in the model and never played — indistinguishable, from outside, from the note
  vanishing. Growth is one-way: a region deliberately left longer than its notes
  is not trimmed to fit.
- Left-edge trimming exists at all now, and `kEdgeGrab` went 5 px → 9 px. Five
  pixels was effectively unhittable, which made resizing look absent rather than
  merely fiddly.
- `TrimClipFrontCommand` carries start, length and source offset together. For
  audio the offset MUST advance with the start, or a front-trim slides the audio
  against the timeline instead of trimming it — which looks right and sounds
  wrong. The MIDI equivalent is `RebaseMidiContent`: notes and controllers are
  stored clip-relative, so the same trim has to shift them by the inverse delta
  or the music moves with the edge.

## Review pass

A full review of the branch found six defects, all fixed and mutation-tested
where behaviour allowed:

| Defect | Why it mattered |
| --- | --- |
| UI registry held raw `BWindow*` | Use-after-free: every `Open()` failure path quits the window and returns while it is still dying |
| `HasNativeUi()` called from `Draw()` | Unsynchronised lilv access from two loopers, every repaint |
| `Open()` held no world lock at all | Opening an editor raced a repaint across ~100 lines of lilv calls |
| Enum snapping ignored declared bounds | A scale point outside min/max was handed to the plugin |
| `fFocus` not adjusted on remove | Deleting the focused insert left a blank editor window |
| Browser read the current selection | Typing after a double-click inserted a different plugin |

The note-growth logic reviewed clean. `TrimClipFrontCommand` did not: a second
pass found three more defects in it, plus one in the setup script.

| Defect | Why it mattered |
| --- | --- |
| MIDI front trim did not rebase notes | Notes are clip-relative, so trimming the left edge moved every note later by the trim amount — a trim that transposed the music in time |
| Front trim wrote `startFrame` in place | It is the sort key both clip lists are kept ordered by; trimming past a neighbour left the track unsorted |
| The drag preview rebased nothing | Notes slid under the cursor for the whole drag and snapped back at the commit |
| `hw_setup.sh` ignored two `chmod` results | A failure left the key unusable by sshd while the script exited 0 reporting it added |

Fixing the first also closed a hole the fix itself opened: moving all lilv work
in `Lv2UiWindow::Open` ahead of the window (required, because `~Lv2UiWindow`
takes the world lock while holding the window lock — the opposite order to
`Open`, and therefore a deadlock) widened the gap between "is an editor already
open?" and registering the new one. The check is now a claim held across the
whole build, so the single-instance guarantee covers the slow part too.

One defect found during this work is NOT ours: `imgui_impl_opengl2.cpp` in
DPF-Widgets guards its `glPushMatrix()` calls behind `#ifndef IMGUI_DPF_BACKEND`
but leaves the matching pops unguarded, so every frame pops twice against zero
pushes. Platform-independent; visible on Haiku only because Mesa's software
rasteriser reports what desktop drivers ignore. Belongs upstream.

---

# Addendum 2: the last two work items

This closes work item 1 (the mixer half) and work item 3 (bypass + wet/dry).
Building them surfaced a defect in the half of item 1 that had already shipped.

## The bug the strip had all along

**Every effect edit made from the channel strip reached the model and the
drawing but never the audio.** Adding a plugin from the strip's empty row,
dragging to reorder, and the bypass dot itself all ran their command directly
and then asked for `kMsgUiRefresh` — which repaints the inspector, the timeline
and the mixer, and touches no engine. The one call to `Engine::SyncFx` in the
whole app sat in the `kMsgApplyFx` handler, which only the effects *editor*
posts. So while the transport was rolling, clicking the bypass dot dimmed the
row and changed nothing you could hear, until something unrelated happened to
rebuild the engine.

It was invisible when stopped, because Play rebuilds the engine from the project
anyway; it needed the transport rolling to show.

Fixed with `kMsgFxChanged`, posted by any strip that has just committed a chain
edit, handled by a new `MainWindow::SyncFxToEngine` that both it and
`kMsgApplyFx` now share. Deliberately NOT folded into `kMsgUiRefresh`: the
timeline posts that for clip moves and gain writes, and syncing the effect graph
on each would rebuild the engine at the playhead for edits that have nothing to
do with effects.

## Mixer-strip insert slots (item 1, the remaining half)

The strip's single "FX n" button is now the same slot list the inspector grew,
narrowed to a 96 px strip: a row per insert with its name and a bypass dot, a
trailing `+ Add` row, and a `+N more` overflow row. Capped at four rows rather
than the inspector's six, because a mixer strip shares its height with five
other sections, a pan knob, a fader and four buttons.

Every strip reserves the *same* block — the largest any strip needs — so the pan
knobs, faders and meters stay on one line across the rack the way a console
reads. The master strip reserves it and draws nothing into it.

The mixer is a separate looper editing a snapshot, so unlike the inspector it
cannot execute anything, and it does not hold descriptors to post back either:
its snapshot carries a label and a bypass flag per insert (`MixerInsertInfo`),
not an `EffectDesc`. Naming an insert needs a host lookup for LV2, which is main
-thread work. So each edit posts an **intent** and the main window turns it into
a command against the real chain:

| Gesture | Message | Command |
| --- | --- | --- |
| Bypass dot | `kMsgMixFxBypass` (+ `int32 "fx"`) | `SetFxBypassCommand` |
| Drag a row | `kMsgMixFxMove` (+ `"from"`, `"to"`) | `SetFxCommand` (permutation) |
| Click a row | `kMsgMixFx` (+ `int32 "slot"`) | — opens the editor on that insert |
| `+ Add` row | `kMsgMixFxAdd` | — opens the plugin browser |

The bypass message says *toggle this index*, not *set it to true*, so a click is
correct however stale the mixer's snapshot is: the main window reads the current
model value and inverts it, and two fast clicks are two toggles rather than two
writes of the same value.

Clicking a row prefers the plugin's own editor when it has one, exactly as the
inspector does — the two strips have to behave the same or the mixer reads as
broken by comparison.

`kMsgMixFxAdd` opens the browser pointed at `InspectorView`, which already turns
a chosen plugin into a `SetFxCommand` against whichever track the message names
— it reads the id from the message precisely so a browser can outlive the
selection it was opened from. That beat a second copy of the same handler.

The insert chains ride the `kMsgMixStrips` refresh as one flat run of `"fxn"` /
`"fxb"` sliced by each strip's `"fx"` count, because a `BMessage` has no
per-strip array to nest them in. Same shape `kMsgApplyFx` already uses for
parameters.

## Per-insert bypass + wet/dry (item 3)

A header row under each panel's title bar: a `Byp` button and a wet/dry slider
reading out in percent. Bypassed panels dim their title, matching how the strip
dims a bypassed row.

**Both commit on release, and the task doc's reasoning for that still holds.**
`kMsgFxLive` addresses a numbered param *slot*; package 01 models `bypassed` and
`mix` as separate per-insert atomics that `SyncFx` pushes, so there is no live
setter to call for either. Adding one is an engine change this package does not
make. The consequence is the specified one, not a bug: the slider tracks the
pointer continuously and the audio steps once, on mouse-up.

Drawn as a slider rather than the "small knob" the task doc asks for. The knob
widget's dial geometry is fixed by constants sized for the 68x86 knob cell, so a
knob here would have meant either a knob-sized header row or parameterising the
dial; the slider matches the generic parameter rows directly below it. Double-
clicking it restores fully wet, the same "back to the default" gesture the
parameter rows use.

## Verification

| Environment | Result |
| --- | --- |
| Haiku VM, LV2 enabled | **46/46**, 0 errors, **0 warnings** |
| Haiku VM, `-DDAW_LV2=OFF` | **43/43**, 0 errors, 0 warnings |
| Linux host | 46/46 |

None of it has been seen running — see "What is NOT verified" above, which still
applies in full. What *was* checked directly is the arithmetic that would fail
silently, lifted out and simulated on the host, then mutation-tested to confirm
the checks were not vacuous:

- the reorder permutation over every from/to pair for chains of 1..8, asserting
  the dragged item lands **on** the row it was dropped on and nothing is lost or
  duplicated (mutating it to insert-before fails 84 checks);
- the flat insert run encoded and decoded across strips of mixed length,
  asserting it round-trips and consumes exactly (mutating the running index to
  reset per strip fails it);
- row counting for chains of 0..9 against the 4-row block, asserting every
  insert is either drawn or counted in the overflow label and that no row ever
  indexes past the chain (mutating the overflow predicate to `>` fails it).

## The click-time crash, identified

Two core files off the user's Desktop closed this out. It was **not** the lilv
races. Closing an LV2 editor aborted the team, on the editor's own looper:

```
BLooper::_QuitRequested -> BLooper::Quit
  -> daw::Lv2UiWindow::~Lv2UiWindow + 0xb0      <- the cleanup(d->ui) call
    -> four_k_eq_2.so (six frames)
      -> BView::~BView + 0x2a
        -> _kern_debugger
```

`BView::~BView` opens with an unconditional `debugger("Trying to delete a view
that belongs to a window. Call RemoveSelf first.")`; `+0x2a` is precisely that
call instruction, guarded by a test of the member at `+0x60`. So the plugin
deleted one of its own views while it was still attached to our window.

The crash was inside the plugin. The cause was ours: we handed the plugin our
container as `LV2_UI__parent`, left the container in the window, and then called
`cleanup()`. Fixed by removing the container first, which clears `fOwner` across
the whole subtree because the BeAPI propagates the owner down to every
descendant, so the plugin's own delete takes the ordinary path.

The detach-and-delete loop that used to follow `cleanup()` collapses to a single
`delete d->container`. It was written for the *other* hazard — views the plugin
forgot to remove, which `~BWindow` would delete after the destructor returns —
and still covers it, since a view the plugin destroyed unlinked itself in
`~BView`. It could never have helped with this crash: it ran after the call that
caused it.

Evidence discipline worth repeating: the newer core's `.text` matches the
on-disk `daw` byte for byte (1666209 of 1666209), so its symbols are real. The
older core differs in 1586178 of those bytes — a different build — so its
`daw::` symbols are fiction and only its library frames were used. Both cores
carry the abort message on a thread stack, *in addition* to libbe's own copy of
the same string at `+0x2a9de0`, which is what makes the second copy meaningful.

**Not confirmed fixed.** Nobody has closed an editor against the new build.

## Still open
- The native editor is view-only. Whether it may touch the live instance was
  the user's decision, now made: yes, in a follow-up — spec'd as
  `07-lv2-live-editor.md`. Still view-only on this branch.
- Dynamic plugin latency (4K EQ 2 moves 0 -> 27 -> 0 at runtime) — also the
  user's decision.
- The wheel does not adjust the wet/dry slider; it scrolls the panel list past
  it, as it does over any control the wheel handler does not claim.
- A strip index posted by the mixer is resolved against the chain as it is when
  the message *arrives*. It is bounds-checked, so a shrunken chain is safe, but
  a chain edited from elsewhere in that window could in principle land the
  toggle on a neighbouring insert. The inspector has the same property.
