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

## Slice 3 — the knob and the dark control look

| File | What it is |
|---|---|
| `src/ui/widgets/DawKnob.h` | the rotary. The dial is `Widgets.h`'s `DrawKnob()`, which grew a `fromCentre` flag so a unipolar knob lights its ring from the minimum end and a bipolar one from 12 o'clock. The control adds the vertical drag (Shift-fine), the wheel, arrow keys, Home/End, a double-click back to the default, a tooltip, and an `Invoke()` that carries the value as a float (`"value"`) beside `BControl`'s own `"be:value"` — a knob's value does not fit in an int32 |
| `src/ui/widgets/DawControlLook.{h,cpp}` | the dark look for everything the kit has not replaced: menus, menu bars, menu items, scroll fields, scrollbars and their arrows, tabs, splitters, borders, group frames, status bars, check boxes, radio buttons, sliders, text-control borders and labels. The header declares EVERY virtual the API has, so a Haiku that adds one breaks the build rather than silently using a light default |
| `CMakeLists.txt` | `DawControlLook.cpp` joins `daw_ui` |
| `src/main.cpp` | the look is installed (`be_control_look`) before the first window exists |

Two things the VM caught that the syntax check cannot:

- The header's `_ReservedControlLook6..10` are declared but never defined by the
  base. Overriding them put symbols in the vtable that do not exist, and only
  the LINK saw it (`undefined reference to daw::DawControlLook::_ReservedControlLook7()`).
  They are not overridden now. This is M1.1's lesson in a new costume: the
  syntax check compiles one file at a time.
- Nothing else: the app links, the suites pass, and the menus/alerts that draw
  through the look do not crash the functional run.

## Slice 4 — the rest of the windows

| File | Change |
|---|---|
| `InstrumentWindow.cpp` | the ADSR row builder returns a `DawSlider`; three `DawMenuField`s; `Load...` is a `DawButton` |
| `ExportWindow.cpp` | four `DawCheckBox`, three `DawMenuField`, two `DawTextField`, both buttons |
| `ExportProgressWindow.cpp` | Cancel is a `DawButton` (the bar itself draws through the look's `DrawStatusBar`) |
| `PluginBrowser.cpp`, `SampleBrowser.cpp` | filter/BPM fields and the buttons |
| `MainWindow.cpp` | the master slider is a `DawSlider`, the tempo field a `DawTextField` |
| `src/main.cpp` | the `B_LIST_*` system colours join `ApplyThemeColors`, so the browsers' stock `BListView` rows come out dark without a subclass |

That last row is a deviation from the spec worth stating: `DawListView`,
`DawProgress` and `DawAlert` were dropped as separate classes. A stock
`BListView`, `BStatusBar` and `BAlert` already draw from the system colours and
the control look, which is the same result with three fewer subclasses to keep
in step. `TransportBar`'s play/stop/record lamps are custom-drawn already (not
stock light controls), so nothing there needs the kit; its internals stay
M3.1's rebuild.

## Slice 5 — the slider's own five behaviours

`BSlider`'s drag maps the pointer's absolute position, which leaves no room for
a fine rate. `DawSlider` now takes the grab point on mouse-down and moves the
value by the distance dragged — a tenth of the rate while Shift is held —
keeping click-to-jump (a click outside the thumb still jumps there first) and
adding a double-click back to `SetDefaultValue()`. With that, the kit's own
value controls (knob, slider) have all five behaviours; the stock-subclass
controls (`DawCheckBox`, `DawMenuField`, `DawTextField`) keep the stock
interaction, which is what "a conversion must not change behaviour" asks for.

### Verification (slices 3–5)

- Host suite: **52/52**. `haiku_syntax_check.sh`: **10 OK, 0 FAIL**.
- VM `build`: ctest **54/54**, `ui_functional_tests` **165 checks, 0 failures**
  (the knob drag and its float value join the probe).
- VM `build-off`: ctest **50/50**.
- Click list for Marc: menus, scrollbars, alerts and any remaining stock control
  now draw dark; the quantize/sends/instrument/export dialogs and the master
  slider and tempo field are kit controls; the knob is not on screen yet (the
  effects panel gets it in M1.4) — the probe window in the test suite is where
  it is exercised today.

## Process note

Slice 2's two commits were made while HEAD was on `master` (the branch was left
behind after slice 1's merge), so they landed on master directly instead of on
`feature/widget-kit` with a `--no-ff` merge. The content is identical and the
history is still linear; it was already pushed when noticed, so it was left
alone rather than rewriting published history. The branch pointer is moved to
master, and slice 3 continues on the branch as the plan's rule says.

## Still to come

- `DawFader` (a vertical channel fader for the mixer strips) — M1.4 restyles the
  mixer, so it lands there rather than here.
- `EffectsWindow`'s hand-rolled knob becomes a `DawKnob` when that window is
  rebuilt (M1.4), so the two do not drift in the meantime.
- The transport bar's internals are M3.1's rebuild (they are custom-drawn, not
  stock controls).
