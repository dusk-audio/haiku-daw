// QuantizeWindow — the piano roll's quantize settings (grid / strength / swing /
// note ends).
//
// Like RenameWindow it never touches the model: the default button posts the
// chosen settings to the roll, which runs the transform on its snapshot and
// commits the result (kMsgApplyMidiOp). The dialog is also how the roll's 'q'
// shortcut learns its "last used" settings, so it is the single place the
// numbers come from.
#pragma once

#include "../model/MidiOps.h"

#include <Messenger.h>
#include <Window.h>

class BCheckBox;
class BPopUpMenu;
class BSlider;

namespace daw {

// QuantizeWindow -> piano roll: apply a quantize with these settings.
// int32 "grid" (QuantGrid), int32 "strength" (0..100), int32 "swing" (0..100),
// bool "lengths".
constexpr uint32 kMsgRollQuantize = 'rqnt';

class QuantizeWindow : public BWindow {
public:
    QuantizeWindow(BRect frame, const QuantizeOpts& opts, BMessenger apply);
    void MessageReceived(BMessage* msg) override;

private:
    BMessenger   fApply;
    BPopUpMenu*  fGrid     = nullptr;
    BSlider*     fStrength = nullptr;
    BSlider*     fSwing    = nullptr;
    BCheckBox*   fLengths  = nullptr;
};

} // namespace daw
