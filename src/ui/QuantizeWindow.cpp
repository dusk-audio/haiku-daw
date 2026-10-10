#include "QuantizeWindow.h"

#include "UiMetrics.h"
#include "widgets/DawButton.h"      // the kit (M1.3)
#include "widgets/DawCheckBox.h"
#include "widgets/DawMenuField.h"
#include "widgets/DawSlider.h"

#include <LayoutBuilder.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <View.h>

#include <cmath>

namespace daw {

enum { MSG_APPLY = 'qzok' };

namespace {

// Grid choices in QuantGrid declaration order, so the menu index IS the enum
// value the roll receives (it casts the int32 straight back).
const QuantGrid kGrids[7] = {
    QuantGrid::Quarter, QuantGrid::Eighth, QuantGrid::Sixteenth,
    QuantGrid::ThirtySecond, QuantGrid::QuarterTriplet,
    QuantGrid::EighthTriplet, QuantGrid::SixteenthTriplet,
};

BSlider* PercentRow(const char* name, const char* label, int value,
                    BWindow* win) {
    // No modification message: these are settings, not edits. Applying (and
    // closing) happens on the button, so a drag across the slider is one
    // quantize with the value you let go on, not one per pixel.
    BSlider* s = new DawSlider(name, label, nullptr, 0, 100, B_HORIZONTAL);
    s->SetValue(value);
    s->SetHashMarks(B_HASH_MARKS_BOTTOM);
    s->SetHashMarkCount(5);
    s->SetTarget(win);
    return s;
}

} // namespace

QuantizeWindow::QuantizeWindow(BRect frame, const QuantizeOpts& opts,
                               BMessenger apply)
    : BWindow(frame, "Quantize", B_TITLED_WINDOW, B_FLOATING_APP_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS),
      fApply(apply) {
    // Layout Kit (M1.4): the window sizes itself to its contents.
    BView* root = new BView("root", B_WILL_DRAW);
    root->SetViewColor(ColHeader());
    BLayoutBuilder::Group<>(this, B_VERTICAL).Add(root);

    // Grid. The items carry NO message: a popup menu is radio by default, so
    // choosing one marks it (and updates the field's label) but must not send
    // anything -- picking a grid is a setting, and applying belongs to the
    // button. With a message here, choosing a grid would quantize with the
    // strength and swing the user had not set yet.
    BPopUpMenu* menu = new BPopUpMenu("grid");
    for (int i = 0; i < 7; i++) {
        BMenuItem* it = new BMenuItem(GridName(kGrids[i]), nullptr);
        if (kGrids[i] == opts.grid) it->SetMarked(true);
        menu->AddItem(it);
    }
    fGrid = menu;

    const int strength = (int)std::lround(
        std::min(1.0f, std::max(0.0f, opts.strength)) * 100.0f);
    const int swing = (int)std::lround(
        std::min(100.0f, std::max(0.0f, opts.swingPct)));
    fStrength = PercentRow("st", "Strength", strength, this);
    fSwing    = PercentRow("sw", "Swing", swing, this);

    // Same rule as the grid: the box is a setting, read when the button is
    // pressed. A message here would apply and close on the tick itself.
    fLengths = new DawCheckBox("ln", "Quantize note ends", nullptr);
    fLengths->SetValue(opts.quantizeLengths ? B_CONTROL_ON : B_CONTROL_OFF);

    DawButton* ok = new DawButton("ok", "Quantize", new BMessage(MSG_APPLY));
    ok->MakeDefault(true);

    BLayoutBuilder::Group<>(root, B_VERTICAL, Themed(10.0f))
        .SetInsets(Themed(8.0f))
        .Add(new DawMenuField("gd", "Grid:", menu))
        .Add(fStrength)
        .Add(fSwing)
        .Add(fLengths)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(ok)
        .End()
        .End();

    ResizeToPreferred();
}

void QuantizeWindow::MessageReceived(BMessage* msg) {
    if (msg->what == MSG_APPLY) {
        BMessage m(kMsgRollQuantize);
        int32 grid = 0;
        if (fGrid) {
            BMenuItem* marked = fGrid->FindMarked();
            if (marked) grid = (int32)fGrid->IndexOf(marked);
        }
        m.AddInt32("grid", grid);
        m.AddInt32("strength", fStrength ? fStrength->Value() : 100);
        m.AddInt32("swing", fSwing ? fSwing->Value() : 0);
        m.AddBool("lengths", fLengths && fLengths->Value() == B_CONTROL_ON);
        fApply.SendMessage(&m);
        Quit();
        return;
    }
    BWindow::MessageReceived(msg);
}

} // namespace daw
