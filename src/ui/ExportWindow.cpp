#include "ExportWindow.h"

#include "UiMetrics.h"

#include "widgets/DawButton.h"      // the kit (M1.3)
#include "widgets/DawCheckBox.h"
#include "widgets/DawMenuField.h"
#include "widgets/DawTextField.h"


#include <cmath>
#include <cstdlib>
#include <LayoutBuilder.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <String.h>
#include <View.h>

namespace daw {

enum { MSG_GO = 'exok' };

namespace {

// Menus are read by INDEX, so the tables below are also the wire values.
const int kBitDepths[3] = { 16, 24, 32 };
const char* const kBitLabels[3] = { "16-bit PCM", "24-bit PCM", "32-bit float" };
const char* const kRangeLabels[2] = { "Whole project", "Loop range" };
const int kRates[4] = { 0, 44100, 48000, 96000 };
const char* const kRateLabels[4] = { "Project rate", "44 100 Hz", "48 000 Hz",
                                     "96 000 Hz" };

// A menu whose items carry NO message: choosing one is a setting, and applying
// belongs to the button (a BPopUpMenu is radio-mode by default, so the item is
// still marked -- which is what the menu field's label shows).
BPopUpMenu* PickMenu(const char* name, const char* const* labels, int count,
                     int marked) {
    BPopUpMenu* menu = new BPopUpMenu(name);
    for (int i = 0; i < count; i++) {
        BMenuItem* it = new BMenuItem(labels[i], nullptr);
        if (i == marked) it->SetMarked(true);
        menu->AddItem(it);
    }
    return menu;
}

// A number out of a text field, clamped to a sane range; anything
// unparseable keeps `fallback`.
float ParseNumber(const char* text, float lo, float hi, float fallback) {
    if (!text) return fallback;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || !std::isfinite(v)) return fallback;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (float)v;
}

int MarkedIndex(BPopUpMenu* menu) {
    if (!menu) return 0;
    BMenuItem* m = menu->FindMarked();
    return m ? menu->IndexOf(m) : 0;
}

} // namespace

ExportWindow::ExportWindow(BRect frame, const ExportChoices& current,
                           bool stems, BMessenger apply)
    : BWindow(frame, "Export", B_TITLED_WINDOW, B_FLOATING_APP_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS),
      fCur(current), fApply(apply) {
    // Layout Kit (M1.4): the window sizes itself to its contents.
    BView* root = new BView("root", B_WILL_DRAW);
    root->SetViewColor(ColHeader());
    BLayoutBuilder::Group<>(this, B_VERTICAL).Add(root);

    // What to render.
    fStems = new DawCheckBox("st", "Separate stems (one file per track)",
                             nullptr);
    fStems->SetValue(stems ? B_CONTROL_ON : B_CONTROL_OFF);

    fRange = PickMenu("range", kRangeLabels, 2, current.range);
    int markedRate = 0;
    for (int i = 0; i < 4; i++)
        if (kRates[i] == current.sampleRate) markedRate = i;
    fRate = PickMenu("rate", kRateLabels, 4, markedRate);
    int markedBits = 0;
    for (int i = 0; i < 3; i++)
        if (kBitDepths[i] == current.bitDepth) markedBits = i;
    fBits = PickMenu("bits", kBitLabels, 3, markedBits);

    // What to do to it on the way out.
    fDither = new DawCheckBox("dt", "Dither (16-bit only)", nullptr);
    fDither->SetValue(current.dither ? B_CONTROL_ON : B_CONTROL_OFF);
    fNorm = new DawCheckBox("nz", "Normalize loudness", nullptr);
    fNorm->SetValue(current.normalize ? B_CONTROL_ON : B_CONTROL_OFF);

    // Text fields, not sliders: a BSlider draws only its label (Haiku never
    // shows the value), and a loudness target is a number the user wants to
    // read back. The transport bar's tempo field is the same idiom.
    BString lufsText;
    lufsText << current.targetLufs;
    fLufs = new DawTextField("lf", "Target LUFS:", lufsText.String(), nullptr);

    fLim = new DawCheckBox("lm", "True-peak limiter", nullptr);
    fLim->SetValue(current.limiter ? B_CONTROL_ON : B_CONTROL_OFF);

    BString ceilText;
    ceilText << current.truePeak;
    fCeil = new DawTextField("cl", "Ceiling dBTP:", ceilText.String(), nullptr);

    DawButton* cancel = new DawButton("cx", "Cancel",
                                      new BMessage(B_QUIT_REQUESTED));
    DawButton* go = new DawButton("ok", stems ? "Export stems" : "Export",
                                  new BMessage(MSG_GO));
    go->MakeDefault(true);

    BLayoutBuilder::Group<>(root, B_VERTICAL, Themed(8.0f))
        .SetInsets(Themed(10.0f))
        .Add(fStems)
        .Add(new DawMenuField("rg", "Range:", fRange))
        .Add(new DawMenuField("sr", "Sample rate:", fRate))
        .Add(new DawMenuField("bd", "Bit depth:", fBits))
        .Add(fDither)
        .Add(fNorm)
        .Add(fLufs)
        .Add(fLim)
        .Add(fCeil)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(go)
        .End()
        .End();

    ResizeToPreferred();
}

void ExportWindow::MessageReceived(BMessage* msg) {
    if (msg->what == B_QUIT_REQUESTED) {
        Quit();
        return;
    }
    if (msg->what == MSG_GO) {
        fCur.bitDepth   = kBitDepths[MarkedIndex(fBits)];
        fCur.sampleRate = kRates[MarkedIndex(fRate)];
        fCur.range      = MarkedIndex(fRange);
        fCur.stems      = fStems && fStems->Value() == B_CONTROL_ON;
        fCur.dither     = fDither && fDither->Value() == B_CONTROL_ON;
        fCur.normalize  = fNorm && fNorm->Value() == B_CONTROL_ON;
        fCur.limiter    = fLim && fLim->Value() == B_CONTROL_ON;
        // Parsed, not trusted: a field left holding garbage keeps the value
        // it had (the dialog's own defaults, or the last good one).
        fCur.targetLufs = ParseNumber(fLufs ? fLufs->Text() : nullptr,
                                      -40.0f, -1.0f, fCur.targetLufs);
        fCur.truePeak   = ParseNumber(fCeil ? fCeil->Text() : nullptr,
                                      -20.0f, 0.0f, fCur.truePeak);

        BMessage m(kMsgExportOptions);
        m.AddInt32("bits", fCur.bitDepth);
        m.AddBool("dither", fCur.dither);
        m.AddInt32("rate", fCur.sampleRate);
        m.AddBool("norm", fCur.normalize);
        m.AddFloat("lufs", fCur.targetLufs);
        m.AddFloat("ceil", fCur.truePeak);
        m.AddBool("lim", fCur.limiter);
        m.AddInt32("range", fCur.range);
        m.AddInt32("stems", fCur.stems ? 1 : 0);
        fApply.SendMessage(&m);
        Quit();
        return;
    }
    BWindow::MessageReceived(msg);
}

} // namespace daw
