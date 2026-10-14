#include "ProjectSettingsWindow.h"

#include "../app/AppSettings.h"     // kSampleRateChoices (the one rate table)
#include "widgets/DawButton.h"      // the kit (M1.3)
#include "widgets/DawCheckBox.h"
#include "widgets/DawMenuField.h"
#include "widgets/DawTextField.h"

#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace daw {

namespace {

enum { MSG_APPLY = 'psap' };

// The meters the menu offers. A project already in another time signature gets
// its own row added (see BuildRows), so nothing is ever silently rewritten.
const int kMeters[6][2] = { {4,4}, {3,4}, {2,4}, {6,8}, {5,4}, {7,8} };

// A line of dim text (the hint under the sample-rate row).
class HintView : public BStringView, public ThemeAware {
public:
    HintView(const char* name, const char* text)
        : BStringView(name, text) {
        ApplyTheme();
    }
    void ApplyTheme() override {
        SetViewColor(ColHeader());
        SetLowColor(ColHeader());
        SetHighColor(ColTextDim());
    }

protected:
    void AttachedToWindow() override {
        BStringView::AttachedToWindow();
        ApplyTheme();
    }
};

int MarkedIndex(BMenu* menu, int count) {
    if (menu == nullptr) return 0;
    BMenuItem* m = menu->FindMarked();
    int index = m ? menu->IndexOf(m) : 0;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    return index;
}

double ParsedTempo(const char* text, double fallback) {
    if (text == nullptr) return fallback;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || !std::isfinite(v)) return fallback;
    if (v < 20.0)  return 20.0;      // the transport field's own range
    if (v > 300.0) return 300.0;
    return v;
}

// Two rates are "the same project rate" when they are the same number of Hz;
// a fraction of a Hz apart (a file rate like 44099.8) is not a change.
bool SameRate(double a, double b) { return std::fabs(a - b) < 0.5; }

} // namespace

ProjectSettingsWindow::ProjectSettingsWindow(
        const ProjectSettingsView& values, BMessenger apply)
    : BWindow(BRect(140, 140, 140 + 460, 140 + 420), "Project Settings",
              // Look, feel and flags as three arguments (see ExportWindow).
              B_TITLED_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS
              | B_CLOSE_ON_ESCAPE),
      fApply(apply) {
    BView* root = new ThemedView("root", B_WILL_DRAW);
    BLayoutBuilder::Group<>(this, B_VERTICAL).Add(root);
    BuildRows(root);
    SetValues(values);
    ResizeToPreferred();
}

void ProjectSettingsWindow::BuildRows(BView* root) {
    // --- sample rate ------------------------------------------------------
    // The offered rates come from AppSettings' table (the same one the
    // Preferences window's default-rate menu uses); the project's own rate is
    // appended when it is not among them, so opening the window never shows a
    // rate the project is not at.
    fRateValues.clear();
    for (int i = 0; i < kSampleRateChoiceCount; i++)
        fRateValues.push_back((double)kSampleRateChoices[i]);

    BMenu* rateMenu = new BPopUpMenu("rate");
    for (double r : fRateValues) {
        char label[32];
        std::snprintf(label, sizeof(label), "%d Hz", (int)std::llround(r));
        rateMenu->AddItem(new ThemedMenuItem(label, nullptr));
    }
    fRate = new DawMenuField("rt", "Sample rate:", rateMenu);

    fResample = new DawCheckBox("rm", "Resample the media to the new rate",
                                nullptr);

    // --- tempo ------------------------------------------------------------
    fTempo = new DawTextField("tp", "Tempo (BPM):", "120", nullptr);

    // --- meter ------------------------------------------------------------
    fMeterValues.clear();
    for (int i = 0; i < 6; i++)
        fMeterValues.push_back({ kMeters[i][0], kMeters[i][1] });
    BMenu* meterMenu = new BPopUpMenu("meter");
    for (const auto& m : fMeterValues) {
        char label[16];
        std::snprintf(label, sizeof(label), "%d/%d", m.first, m.second);
        meterMenu->AddItem(new ThemedMenuItem(label, nullptr));
    }
    fMeter = new DawMenuField("mt", "Meter:", meterMenu);

    // --- notes ------------------------------------------------------------
    // A text view, not a text field: project notes are lines of prose, and a
    // field would silently collapse the newlines a project already has.
    const rgb_color textCol = ColText();
    BFont font = ThemeFont();
    fNotes = new BTextView(BRect(0, 0, 300, 76), "nt", BRect(0, 0, 300, 76),
                           &font, &textCol, B_FOLLOW_ALL,
                           B_WILL_DRAW | B_NAVIGABLE);
    fNotes->SetViewColor(ColLcd());
    fNotes->SetLowColor(ColLcd());
    fNotes->SetHighColor(textCol);
    fNotes->SetInsets(Themed(4.0f), Themed(3.0f), Themed(4.0f), Themed(3.0f));
    fNotes->SetWordWrap(true);
    BScrollView* notesScroll = new BScrollView("nts", fNotes, B_FOLLOW_ALL, 0,
                                               false, true);
    notesScroll->SetExplicitMinSize(BSize(B_SIZE_UNSET, Themed(66.0f)));
    notesScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, Themed(96.0f)));

    fHint = new HintView("hn",
        "The rate rescales the timeline (frames are samples). Resampling "
        "rewrites each media file at the new rate beside itself, keeping the "
        "originals.");

    // The settings vocabulary (one pass, with Preferences): Cancel + Apply.
    // Cancel is what the old "Close" did -- leave without applying anything --
    // and naming it Cancel says that; Apply commits the whole view, and Return
    // presses it (the default button). Escape is the same as Cancel.
    DawButton* cancel = new DawButton("cx", "Cancel",
                                      new BMessage(B_QUIT_REQUESTED));
    DawButton* apply = new DawButton("ap", "Apply", new BMessage(MSG_APPLY));
    apply->MakeDefault(true);

    BLayoutBuilder::Group<>(root, B_VERTICAL, Themed(8.0f))
        .SetInsets(Themed(10.0f))
        .Add(fRate)
        .Add(fResample)
        .Add(fHint)
        .Add(fTempo)
        .Add(fMeter)
        .Add(new HintView("nh", "Notes:"))
        .Add(notesScroll)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(apply)
        .End()
        .End();
}

// What the controls say, in the shape the message carries.
ProjectSettingsView ProjectSettingsWindow::Values() const {
    ProjectSettingsView v;
    if (!fRateValues.empty())
        v.sampleRate = fRateValues[(size_t)MarkedIndex(
            fRate ? fRate->Menu() : nullptr, (int)fRateValues.size())];
    if (!fMeterValues.empty()) {
        const auto& m = fMeterValues[(size_t)MarkedIndex(
            fMeter ? fMeter->Menu() : nullptr, (int)fMeterValues.size())];
        v.meterNum = m.first;
        v.meterDenom = m.second;
    }
    v.tempoBPM = ParsedTempo(fTempo ? fTempo->Text() : nullptr, 120.0);
    v.notes = (fNotes != nullptr && fNotes->Text() != nullptr)
            ? fNotes->Text() : "";
    v.resampleMedia = fResample != nullptr
                   && fResample->Value() == B_CONTROL_ON;
    v.hasMedia = fHasMedia;
    return v;
}

void ProjectSettingsWindow::SetValues(const ProjectSettingsView& v) {
    // A rate the window does not offer yet gets its own row (the project's own,
    // so opening the window is never a lie about what the project is at).
    if (!fRateValues.empty()) {
        bool known = false;
        for (double r : fRateValues) if (SameRate(r, v.sampleRate)) known = true;
        if (!known && v.sampleRate > 0.0) {
            fRateValues.push_back(v.sampleRate);
            char label[32];
            std::snprintf(label, sizeof(label), "%d Hz",
                          (int)std::llround(v.sampleRate));
            fRate->Menu()->AddItem(new ThemedMenuItem(label, nullptr));
        }
    }
    if (!fMeterValues.empty()) {
        bool known = false;
        for (const auto& m : fMeterValues)
            if (m.first == v.meterNum && m.second == v.meterDenom) known = true;
        if (!known) {
            fMeterValues.push_back({ v.meterNum, v.meterDenom });
            char label[16];
            std::snprintf(label, sizeof(label), "%d/%d", v.meterNum,
                          v.meterDenom);
            fMeter->Menu()->AddItem(new ThemedMenuItem(label, nullptr));
        }
    }

    auto mark = [](BMenu* menu, int index) {
        if (menu == nullptr) return;
        for (int32 i = 0; i < menu->CountItems(); i++)
            if (BMenuItem* it = menu->ItemAt(i)) it->SetMarked(i == index);
    };
    if (fRate) {
        int index = 0;
        for (size_t i = 0; i < fRateValues.size(); i++)
            if (SameRate(fRateValues[i], v.sampleRate)) index = (int)i;
        mark(fRate->Menu(), index);
    }
    if (fMeter) {
        int index = 0;
        for (size_t i = 0; i < fMeterValues.size(); i++)
            if (fMeterValues[i].first == v.meterNum
                && fMeterValues[i].second == v.meterDenom) index = (int)i;
        mark(fMeter->Menu(), index);
    }
    if (fTempo) {
        char buf[24];
        // Enough digits that an UNCHANGED Apply writes back the tempo it
        // showed: at four significant digits a stored 133.33 displayed as
        // "133.3", and pressing Apply without touching anything moved the
        // tempo by a third of a beat per minute.
        std::snprintf(buf, sizeof(buf), "%.10g", v.tempoBPM);
        fTempo->SetText(buf);
    }
    if (fNotes) {
        // Not while the user is mid-edit: a push that lands between keystrokes
        // would move the caret. SetText is skipped when the text is the same,
        // which is every push that did not come from an apply.
        if (v.notes != fNotes->Text()) fNotes->SetText(v.notes.c_str());
    }
    fHasMedia = v.hasMedia;
    if (fResample) {
        fResample->SetEnabled(v.hasMedia);
        // A disabled offer is cleared: leaving it ticked would claim the next
        // rate change converts media that is not there.
        if (!v.hasMedia) fResample->SetValue(B_CONTROL_OFF);
        else fResample->SetValue(v.resampleMedia ? B_CONTROL_ON
                                                 : B_CONTROL_OFF);
    }
    RefreshEnabled();
}

void ProjectSettingsWindow::RefreshEnabled() {
    if (fResample == nullptr) return;
    if (!fResample->IsEnabled() && fResample->Value() == B_CONTROL_ON)
        fResample->SetValue(B_CONTROL_OFF);
}

void ProjectSettingsWindow::ApplyTheme() {
    const rgb_color textCol = ColText();
    const BFont font = ThemeFont();
    if (fNotes != nullptr) {
        fNotes->SetViewColor(ColLcd());
        fNotes->SetLowColor(ColLcd());
        fNotes->SetHighColor(textCol);
        if (fNotes->TextLength() > 0)
            fNotes->SetFontAndColor(0, fNotes->TextLength(), &font, B_FONT_ALL,
                                    &textCol);
    }
    ApplyThemeToWindowViews(this);
}

// The Apply button's own path, as a method so the functional test can take it
// without pressing a button.
void ProjectSettingsWindow::PostChanges() {
    if (!fApply.IsValid()) return;
    const ProjectSettingsView v = Values();
    BMessage m(kMsgProjSettingsChanged);
    m.AddDouble("rate", v.sampleRate);
    m.AddDouble("tempo", v.tempoBPM);
    m.AddInt32("mnum", v.meterNum);
    m.AddInt32("mdenom", v.meterDenom);
    m.AddString("notes", v.notes.c_str());
    m.AddBool("resample", v.resampleMedia);
    fApply.SendMessage(&m);
}

void ProjectSettingsWindow::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case B_QUIT_REQUESTED:
            Quit();
            return;
        case MSG_APPLY:
            PostChanges();
            return;
        case kMsgProjSettingsActivate:
            Activate(true);
            return;
        case kMsgProjSettingsValues: {
            // The app's values (full set in practice; anything missing keeps
            // what the window already shows).
            ProjectSettingsView v = Values();
            double d = 0.0;
            int32 i = 0;
            bool b = false;
            const char* s = nullptr;
            if (msg->FindDouble("rate", &d) == B_OK)  v.sampleRate = d;
            if (msg->FindDouble("tempo", &d) == B_OK) v.tempoBPM = d;
            if (msg->FindInt32("mnum", &i) == B_OK)   v.meterNum = i;
            if (msg->FindInt32("mdenom", &i) == B_OK) v.meterDenom = i;
            if (msg->FindString("notes", &s) == B_OK && s) v.notes = s;
            if (msg->FindBool("hasmedia", &b) == B_OK) v.hasMedia = b;
            if (msg->FindBool("resample", &b) == B_OK) v.resampleMedia = b;
            SetValues(v);
            break;
        }
        default:
            BWindow::MessageReceived(msg);
            break;
    }
}

} // namespace daw
