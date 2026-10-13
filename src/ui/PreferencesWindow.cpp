#include "PreferencesWindow.h"

#include "widgets/DawButton.h"      // the kit (M1.3)
#include "widgets/DawCheckBox.h"
#include "widgets/DawMenuField.h"
#include "widgets/DawSlider.h"
#include "widgets/DawTextField.h"

#include <Directory.h>
#include <Entry.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuItem.h>
#include <Node.h>        // B_DIRECTORY_NODE (the folder panel)
#include <Path.h>
#include <PopUpMenu.h>
#include <StringView.h>
#include <TextControl.h>

#include <cstdio>
#include <string>
#include <vector>

namespace daw {

namespace {

// The window's own messages (nothing outside needs them).
enum { MSG_CHOOSE_DIR = 'prcd', MSG_LEVEL_MOVED = 'prlm',
       MSG_APPLY = 'prap' };

// A text line in this window. A BStringView caches the view colour it was
// created with, so a theme switch has to re-take it (T1) -- the kit controls
// do that themselves, a stock string view does not.
class PrefsLabel : public BStringView, public ThemeAware {
public:
    PrefsLabel(const char* name, const char* text)
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

// A menu of labels. `what` 0 means "a setting": choosing a row marks it and
// nothing else happens -- the window applies the whole view on Apply, so a pick
// is not a commit (the settings vocabulary, one pass with Project Settings).
BMenu* PickMenu(const char* name, const char* const* labels, int count,
                int marked, uint32 what) {
    BMenu* menu = new BPopUpMenu(name);
    for (int i = 0; i < count; i++) {
        BMenuItem* it = new ThemedMenuItem(
            labels[i], what != 0 ? new BMessage(what) : nullptr);
        if (i == marked) it->SetMarked(true);
        menu->AddItem(it);
    }
    return menu;
}

// The index of the marked item, clamped: a menu always shows something, and a
// read of it must never index a table out of range.
int MarkedIndex(BMenu* menu, int count) {
    if (menu == nullptr) return 0;
    BMenuItem* m = menu->FindMarked();
    int index = m ? menu->IndexOf(m) : 0;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    return index;
}

// `v`'s value in `set`, or `fallback` when it is not there (the window is
// opened from a settings file that may hold anything -- Sanitize() runs on
// load, but the push does not have to trust it either).
int IndexInSet(int v, const int* set, int count, int fallback = 0) {
    for (int i = 0; i < count; i++) if (set[i] == v) return i;
    return fallback;
}

int IndexInFloatSet(float v, const float* set, int count) {
    for (int i = 0; i < count; i++) if (set[i] == v) return i;
    return 0;
}

} // namespace

PreferencesWindow::PreferencesWindow(const PrefsView& values, BMessenger apply)
    : BWindow(BRect(120, 120, 120 + 460, 120 + 400), "Preferences",
              // Look, feel and flags as three arguments (see ExportWindow).
              B_TITLED_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS
              | B_CLOSE_ON_ESCAPE),
      fApply(apply) {
    // Layout Kit (M1.4): the window sizes itself to its contents.
    BView* root = new ThemedView("root", B_WILL_DRAW);
    BLayoutBuilder::Group<>(this, B_VERTICAL).Add(root);
    BuildRows(root);
    SetValues(values);
    ResizeToPreferred();
}

void PreferencesWindow::BuildRows(BView* root) {
    // Label tables. They are built HERE, from AppSettings' own choice tables,
    // so what the menus offer and what a load accepts cannot drift apart.
    std::vector<std::string> bufferLabels, rateLabels, countLabels, soundLabels,
                             scaleLabels, autosaveLabels;
    for (int i = 0; i < kBufferFrameChoiceCount; i++) {
        char b[48];
        std::snprintf(b, sizeof(b), "%d frames", kBufferFrameChoices[i]);
        bufferLabels.push_back(b);
    }
    for (int i = 0; i < kSampleRateChoiceCount; i++) {
        char b[48];
        std::snprintf(b, sizeof(b), "%d Hz", kSampleRateChoices[i]);
        rateLabels.push_back(b);
    }
    for (int i = 0; i < kCountInChoiceCount; i++) {
        char b[32];
        const int n = kCountInChoices[i];
        if (n == 0) std::snprintf(b, sizeof(b), "Off");
        else        std::snprintf(b, sizeof(b), "%d bar%s", n, n > 1 ? "s" : "");
        countLabels.push_back(b);
    }
    static const char* const kSounds[kMetronomeSoundCount] =
        { "Click", "Beep", "Wood block" };
    for (int i = 0; i < kMetronomeSoundCount; i++)
        soundLabels.push_back(kSounds[i]);
    static const char* const kScales[kUiScaleChoiceCount] =
        { "Follow the system font", "100%", "125%", "150%" };
    for (int i = 0; i < kUiScaleChoiceCount; i++)
        scaleLabels.push_back(kScales[i]);
    static const char* const kAutosave[5] =
        { "Off", "Every 30 seconds", "Every minute", "Every 5 minutes",
          "Every 10 minutes" };
    for (int i = 0; i < kAutosaveChoiceCount; i++)
        autosaveLabels.push_back(kAutosave[i]);

    auto labels = [](std::vector<std::string>& v) {
        std::vector<const char*> out;
        for (const std::string& s : v) out.push_back(s.c_str());
        return out;
    };
    std::vector<const char*> bl = labels(bufferLabels);
    std::vector<const char*> rl = labels(rateLabels);
    std::vector<const char*> cl = labels(countLabels);
    std::vector<const char*> sl = labels(soundLabels);
    std::vector<const char*> scl = labels(scaleLabels);
    std::vector<const char*> al = labels(autosaveLabels);

    // No message on any row: a pick or a tick is a SETTING until Apply (the
    // window holds the values and posts them as one; see NotifyChanged).
    BMenu* bufferMenu = PickMenu("buffer", bl.data(), (int)bl.size(), 0, 0);
    BMenu* rateMenu   = PickMenu("rate", rl.data(), (int)rl.size(), 0, 0);
    BMenu* countMenu  = PickMenu("countin", cl.data(), (int)cl.size(), 0, 0);
    BMenu* soundMenu  = PickMenu("sound", sl.data(), (int)sl.size(), 0, 0);
    BMenu* scaleMenu  = PickMenu("scale", scl.data(), (int)scl.size(), 0, 0);
    BMenu* autoMenu   = PickMenu("autosave", al.data(), (int)al.size(), 1, 0);

    // ---- M5.3 SEAM: the audio output device ------------------------------
    // The multichannel-input package owns device enumeration (plan M5.3) and
    // adds the picker HERE: a menu of the live output nodes, a settings key for
    // the chosen one, and the engine told at the next Play. Until then the row
    // says what is actually happening -- the system's default -- and the note
    // under it says choosing is not available yet, in the user's words rather
    // than the roadmap's. The control is disabled because one that cannot do
    // what it says is worse than no control.
    static const char* const kDeviceLabels[1] = { "System default" };
    BMenu* deviceMenu = PickMenu("device", kDeviceLabels, 1, 0, 0);
    fDevice = new DawMenuField("dv", "Output device:", deviceMenu);
    fDevice->SetEnabled(false);

    fBuffer   = new DawMenuField("bf", "Buffer size:", bufferMenu);
    fRate     = new DawMenuField("sr", "Default sample rate:", rateMenu);
    fCountIn  = new DawMenuField("ci", "Count-in:", countMenu);
    fSound    = new DawMenuField("ms", "Metronome sound:", soundMenu);

    // The level slider: MSG_LEVEL_MOVED only refreshes the readout beside it
    // (the value itself is a setting like any other -- it goes to the app when
    // Apply is pressed).
    fLevel = new DawSlider("ml", "", new BMessage(MSG_LEVEL_MOVED), 0, 100);
    fLevel->SetModificationMessage(new BMessage(MSG_LEVEL_MOVED));
    fLevel->SetValue(30);
    fLevelText = new PrefsLabel("mlv", "30%");

    fMonitor = new DawCheckBox("mi", "Monitor the input while recording",
                               nullptr);
    // The MIDI editor's audition (package 51). The roll always asks for it;
    // MainWindow is where this decides (EnsureAuditionEngine), so the switch
    // silences the clicks without the editor having to know. Like every other
    // row here it carries no message: a setting until Apply.
    fPreview = new DawCheckBox("pn", "Preview notes when editing MIDI",
                               nullptr);
    fDark    = new DawCheckBox("dm", "Dark mode (this app's windows only)",
                               nullptr);
    fScale   = new DawMenuField("us", "UI scale:", scaleMenu);

    fDir = new DawTextField("pd", "Default project folder:", "", nullptr);
    DawButton* chooseDir = new DawButton("cd", "Choose" B_UTF8_ELLIPSIS,
                                         new BMessage(MSG_CHOOSE_DIR));

    fAutosave = new DawMenuField("as", "Autosave:", autoMenu);

    // The settings vocabulary (one pass, with Project Settings): Cancel + Apply,
    // whose names say what they do -- Apply commits the whole view, Cancel (and
    // Escape, B_CLOSE_ON_ESCAPE above) leaves without applying what was not.
    // The old "Close" meant "done" only by convention, next to a window whose
    // Close discarded.
    DawButton* cancel = new DawButton("cx", "Cancel",
                                      new BMessage(B_QUIT_REQUESTED));
    DawButton* applyBtn = new DawButton("ap", "Apply",
                                        new BMessage(MSG_APPLY));
    applyBtn->MakeDefault(true);   // Return applies

    BLayoutBuilder::Group<>(root, B_VERTICAL, Themed(8.0f))
        .SetInsets(Themed(10.0f))
        .Add(fDevice)
        .Add(new PrefsLabel("dvh", "The system's default output device is "
                                   "used; choosing one is not available yet."))
        .Add(fBuffer)
        // What the row PROMISES, next to the row: the buffer is the audio
        // device's block size, and a running device is not re-opened under the
        // transport (the Audio menu has behaved this way since it existed), so
        // the new size takes effect when playback next starts.
        .Add(new PrefsLabel("bfh", "Applied at the next Play."))
        .Add(fRate)
        .Add(fCountIn)
        .Add(fSound)
        // The level: the slider takes the room, the readout keeps its width,
        // so the number does not move while the slider is dragged.
        .AddGroup(B_HORIZONTAL, Themed(6.0f))
            .Add(new PrefsLabel("lev", "Level:"))
            .Add(fLevel)
            .Add(fLevelText)
        .End()
        .Add(fMonitor)
        .Add(fPreview)
        .Add(fDark)
        .Add(fScale)
        // The scale IS applied now (the whole process re-lays out at it); what
        // waits for a launch is the window FRAMES, which stay where the user
        // put them rather than jumping under the pointer.
        .Add(new PrefsLabel("ush", "Applied on Apply; window sizes settle at "
                                   "the next launch."))
        .AddGroup(B_HORIZONTAL, Themed(6.0f))
            .Add(fDir)
            .Add(chooseDir)
        .End()
        .Add(fAutosave)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(applyBtn)
        .End()
        .End();
}

// The one place the controls are read. Every change path calls this, so a
// message can never carry a half-updated set.
PrefsView PreferencesWindow::Values() const {
    PrefsView v;
    v.bufferFrames = kBufferFrameChoices[
        MarkedIndex(fBuffer ? fBuffer->Menu() : nullptr,
                    kBufferFrameChoiceCount)];
    v.defaultSampleRate = kSampleRateChoices[
        MarkedIndex(fRate ? fRate->Menu() : nullptr, kSampleRateChoiceCount)];
    v.countInBars = kCountInChoices[
        MarkedIndex(fCountIn ? fCountIn->Menu() : nullptr, kCountInChoiceCount)];
    v.metronomeSound = MarkedIndex(fSound ? fSound->Menu() : nullptr,
                                   kMetronomeSoundCount);
    v.metronomeLevel = fLevel ? (float)fLevel->Value() / 100.0f : 0.30f;
    v.monitorInput = fMonitor != nullptr && fMonitor->Value() == B_CONTROL_ON;
    v.previewNotes = fPreview != nullptr && fPreview->Value() == B_CONTROL_ON;
    v.darkMode     = fDark != nullptr && fDark->Value() == B_CONTROL_ON;
    v.uiScale = kUiScaleChoices[
        MarkedIndex(fScale ? fScale->Menu() : nullptr, kUiScaleChoiceCount)];
    v.projectDir = (fDir != nullptr && fDir->Text() != nullptr)
                 ? fDir->Text() : "";
    v.autosaveSeconds = kAutosaveChoices[
        MarkedIndex(fAutosave ? fAutosave->Menu() : nullptr,
                    kAutosaveChoiceCount)];
    return v;
}

void PreferencesWindow::SetValues(const PrefsView& v) {
    auto mark = [](BMenu* menu, int index) {
        if (menu == nullptr) return;
        for (int32 i = 0; i < menu->CountItems(); i++)
            if (BMenuItem* it = menu->ItemAt(i)) it->SetMarked(i == index);
    };
    if (fBuffer) mark(fBuffer->Menu(), IndexInSet(v.bufferFrames,
                                                  kBufferFrameChoices,
                                                  kBufferFrameChoiceCount));
    if (fRate) mark(fRate->Menu(), IndexInSet(v.defaultSampleRate,
                                              kSampleRateChoices,
                                              kSampleRateChoiceCount));
    if (fCountIn) mark(fCountIn->Menu(), IndexInSet(v.countInBars,
                                                    kCountInChoices,
                                                    kCountInChoiceCount));
    if (fSound) mark(fSound->Menu(),
                     v.metronomeSound >= 0 && v.metronomeSound < kMetronomeSoundCount
                     ? v.metronomeSound : 0);
    if (fLevel) {
        int pct = (int)(v.metronomeLevel * 100.0f + 0.5f);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        fLevel->SetValue(pct);
    }
    if (fMonitor) fMonitor->SetValue(v.monitorInput ? B_CONTROL_ON
                                                    : B_CONTROL_OFF);
    if (fPreview) fPreview->SetValue(v.previewNotes ? B_CONTROL_ON
                                                    : B_CONTROL_OFF);
    if (fDark)    fDark->SetValue(v.darkMode ? B_CONTROL_ON : B_CONTROL_OFF);
    if (fScale) mark(fScale->Menu(), IndexInFloatSet(v.uiScale,
                                                     kUiScaleChoices,
                                                     kUiScaleChoiceCount));
    if (fDir) fDir->SetText(v.projectDir.c_str());
    if (fAutosave) mark(fAutosave->Menu(), IndexInSet(v.autosaveSeconds,
                                                      kAutosaveChoices,
                                                      kAutosaveChoiceCount, 1));
    UpdateLevelLabel();
}

void PreferencesWindow::UpdateLevelLabel() {
    if (fLevelText == nullptr) return;
    char b[32];
    const int pct = fLevel ? fLevel->Value() : 0;
    std::snprintf(b, sizeof(b), "%d%%", pct);
    fLevelText->SetText(b);
}

// Push the whole set to the app -- on Apply, and on anything that means the
// same thing (this is the ONE place a set leaves the window). The controls
// themselves post nothing: a pick or a tick is a setting the app only sees when
// the user applies it, or forgets when Cancel (or Escape) closes the window.
void PreferencesWindow::NotifyChanged() {
    if (!fApply.IsValid()) return;
    const PrefsView v = Values();
    BMessage m(kMsgPrefsChanged);
    m.AddInt32("buffer", v.bufferFrames);
    m.AddInt32("defrate", v.defaultSampleRate);
    m.AddInt32("countin", v.countInBars);
    m.AddInt32("metsnd", v.metronomeSound);
    m.AddFloat("metlvl", v.metronomeLevel);
    m.AddBool("monin", v.monitorInput);
    m.AddBool("preview", v.previewNotes);
    m.AddBool("dark", v.darkMode);
    m.AddFloat("uiscale", v.uiScale);
    m.AddString("projdir", v.projectDir.c_str());
    m.AddInt32("autosv", v.autosaveSeconds);
    fApply.SendMessage(&m);
}

void PreferencesWindow::ChooseDir() {
    if (fDirPanel == nullptr) {
        BMessenger to(this);
        // A folder, not a file: the panel is in directory mode and picking one
        // sends its ref (B_DIRECTORY_NODE), which is what the row wants.
        fDirPanel = new BFilePanel(B_OPEN_PANEL, &to, NULL, B_DIRECTORY_NODE,
                                   false, new BMessage(kMsgPrefsDirRef),
                                   NULL, false, true);
    }
    if (fDirPanel->Window() != nullptr)
        fDirPanel->Window()->SetTitle("Default Project Folder");
    fDirPanel->Show();
    // A stock panel: its colours are the system's, which the app does not
    // rewrite (T1) -- the DAW's own go on explicitly (a no-op in System mode).
    ThemeStockWindow(fDirPanel->Window());
}

void PreferencesWindow::MessageReceived(BMessage* msg) {
    if (msg->what == B_QUIT_REQUESTED) {
        Quit();
        return;
    }
    switch (msg->what) {
        case kMsgPrefsValues: {
            // The app's current values. Anything the message does not carry
            // keeps the field's own default, so a partial push cannot blank a
            // control (the same policy AppSettings has for a missing line).
            PrefsView v;
            int32 i = 0;
            float f = 0.0f;
            bool b = false;
            const char* s = nullptr;
            if (msg->FindInt32("buffer", &i)  == B_OK) v.bufferFrames = i;
            if (msg->FindInt32("defrate", &i) == B_OK) v.defaultSampleRate = i;
            if (msg->FindInt32("countin", &i) == B_OK) v.countInBars = i;
            if (msg->FindInt32("metsnd", &i)  == B_OK) v.metronomeSound = i;
            if (msg->FindFloat("metlvl", &f)  == B_OK) v.metronomeLevel = f;
            if (msg->FindBool("monin", &b)    == B_OK) v.monitorInput = b;
            if (msg->FindBool("dark", &b)     == B_OK) v.darkMode = b;
            if (msg->FindFloat("uiscale", &f) == B_OK) v.uiScale = f;
            if (msg->FindString("projdir", &s) == B_OK && s) v.projectDir = s;
            if (msg->FindInt32("autosv", &i)  == B_OK) v.autosaveSeconds = i;
            SetValues(v);
            break;
        }
        case MSG_LEVEL_MOVED:
            // The slider's live readout; the value itself waits for Apply.
            UpdateLevelLabel();
            break;
        case MSG_APPLY:
            NotifyChanged();
            break;
        case MSG_CHOOSE_DIR:
            ChooseDir();
            break;
        case kMsgPrefsDirRef: {
            entry_ref ref;
            if (msg->FindRef("refs", &ref) == B_OK) {
                BPath path(&ref);
                if (path.InitCheck() == B_OK && fDir != nullptr) {
                    fDir->SetText(path.Path());
                    // A setting like any other: it reaches the app on Apply.
                }
            }
            break;
        }
        case kMsgPrefsActivate:
            Activate(true);
            break;
        default:
            BWindow::MessageReceived(msg);
            break;
    }
}

void PreferencesWindow::ApplyTheme() {
    // The window itself caches nothing -- its root is a ThemedView and its
    // readout lines are ThemeAware views -- so the views carry the switch.
    // ApplyThemeToAllWindows walks them right after this returns; this is here
    // for the caller that only asks the window (and it is idempotent).
    ApplyThemeToWindowViews(this);
}

} // namespace daw
