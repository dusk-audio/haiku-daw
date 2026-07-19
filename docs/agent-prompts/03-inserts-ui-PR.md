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
