# M1.3 dark widget kit — PR record

Branch `feature/widget-kit` off `master` (`487a41e`). Spec: `21-widget-kit.md`.
The item is multi-slice; this record grows as the slices land.

## Slice 1 — the foundation and the first window

The kit's shared base and its first three controls, with `RenameWindow` as the
end-to-end proof that a converted window behaves exactly as before.

| File | What it is |
|---|---|
| `src/ui/widgets/DawControl.h` | the base: hover / pressed / focus states, the wheel hook, double-click reset, `AdjustStep()` for Shift-fine, a tooltip (the control's label when the caller sets none), `Themed()` geometry and the Layout Kit sizes (`GetPreferredSize`) |
| `src/ui/widgets/DawButton.h` | `DawButton` and `DawToggle`, drawn with `Widgets.h`'s shared `DrawButton()` so a kit button and a timeline lamp stay the same object; hover is a light wash, pressed a dark one, the focus ring is the theme accent, and `MakeDefault()` draws the accent border (BControl has no notion of a default button — that is BButton's) |
| `src/ui/widgets/DawTextField.h` | a `BTextControl` whose label half takes the panel colour and whose well is the recessed LCD with bright text |
| `src/ui/RenameWindow.cpp` | converted: `DawTextField` + `DawButton`, insets through `Themed()` |
| `src/main.cpp` | `ApplyThemeColors()` in `ReadyToRun` — the system colours (panel, panel text, document, control background/text/border, menu background/item/selected, scrollbar thumb, keyboard navigation) now come from `Theme.h`, so what is still stock is dark rather than punching light holes in the converted windows |

Notes:

- The kit is **header-only** for now, so nothing joins `daw_ui`'s source list
  (M1.1's lesson does not bite yet); when a control needs a `.cpp`, the list
  must be updated in the same commit.
- Both constructors are offered per control — the rectangle form today's
  windows use, and the Layout Kit form M1.4 will use. `GetMinSize()` does NOT
  exist in this Haiku's `BView` (only `GetPreferredSize`), so the kit does not
  override it.
- The system colour set is applied before any window exists; a side effect is
  that LV2 plugin UIs which do not set their own colours will now draw on the
  dark defaults, which is the intent (the app is dark) and not a regression.
- `B_TEXT_SELECTION_COLOR` does not exist in this Haiku's `InterfaceDefs.h`
  (the selection colour comes from `B_MENU_SELECTION_BACKGROUND_COLOR` /
  navigation colours); the helper sets what exists.

## The kit's first real bug (found by its first test)

`DawControl` sent its message **nowhere**: `BControl::Invoke()` with no target
goes to the invoker's owner, and `BControl` — unlike `BButton` — does not make
the window the default target on attach. The probe window never saw the click,
`Invoke()` still returned `B_OK`, and only a test that asserts the message
ARRIVES catches that. `AttachedToWindow` now does what `BButton` does:

```cpp
if (Target() == nullptr && Window() != nullptr) SetTarget(Window());
```

Two more things the test taught, both recorded in it as comments: a control's
synthetic click must aim at the control's OWN coordinates (screen_where is the
control's screen rect, not the window's), and a push button reports
`be:value = 0` (`B_CONTROL_OFF`) — the value belongs to toggles.

## Verification (slice 1)

- Host suite `build-host`: **52/52** (unchanged — Haiku-only code).
- `scripts/haiku_syntax_check.sh`: **10 OK, 0 FAIL**.
- VM `build`: ctest **54/54**, `ui_functional_tests` **159 checks, 0 failures**
  (was 155; this slice adds 4 deliberate checks).
- VM `build-off`: ctest **50/50**.
- Click list for Marc (visual, cannot be asserted programmatically): the rename
  dialog (double-click a track name) shows a dark field with bright text, an
  accent-bordered OK, a hover wash, and a focus ring; menus are dark now.

## Slice 2 — the value controls, and two more windows

| File | What it is |
|---|---|
| `src/ui/widgets/DawSlider.h` | a `BSlider` subclass. Dragging, the keyboard, the hash marks, the focus model and the message protocol stay stock — the kit overrides only `DrawBar`, `DrawThumb`, `DrawHashMarks`, `DrawText` and `DrawFocusMark`. Subclassing rather than re-implementing is what keeps a converted window behaving like the one it replaced |
| `src/ui/widgets/DawCheckBox.h` | a `BCheckBox` subclass: the click, the value flip and the `B_CONTROL_ON/OFF` message stay stock, the box and its tick are drawn dark |
| `src/ui/widgets/DawMenuField.h` | a `BMenuField` subclass: the popup, the marked item and the label stay stock, the well, the label and the accent arrow are drawn here (the stock control look would paint a light island) |
| `src/ui/QuantizeWindow.cpp` | converted: `DawSlider` ×2, `DawCheckBox`, `DawMenuField`, `DawButton`, rows and insets through `Themed()` |
| `src/ui/SendsWindow.cpp` | converted: `DawMenuField`, `DawCheckBox`, `DawButton`, `DawSlider` per send, rows through `Themed()` |

The two windows keep their base-class pointers in their headers (`BSlider*`,
`BCheckBox*`), so the conversion did not touch the headers at all: the kit's
controls ARE the stock types.

The probe test grows two controls: a click ticks the box on and off with the
value on the message, and a click near the slider's right end moves its value
there — checked against the control's own `Value()`, not only the message.

### Verification (slice 2)

- Host suite: **52/52**.
- `haiku_syntax_check.sh`: **10 OK, 0 FAIL**.
- VM `build`: ctest **54/54**, `ui_functional_tests` **163 checks, 0 failures**
  (+4 deliberate).
- VM `build-off`: ctest **50/50**.
- Click list for Marc: the Quantize window (MIDI menu → Quantize) now shows
  dark sliders with hash marks, a dark tick box and a dark grid field; the
  Sends window the same, with the accent arrow on each `To:` field.

## Still to come

Slices 2+ per the spec: `DawSlider`/`DawFader`/`DawKnob`/`DawCheckBox`/
`DawMenuField`/`DawListView`/`DawProgress`/`DawAlert` and the `BControlLook`
subclass, then the window conversions (QuantizeWindow, SendsWindow,
SampleBrowser, PluginBrowser, InstrumentWindow, ExportWindow,
ExportProgressWindow, TransportBar internals).
