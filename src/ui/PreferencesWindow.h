// PreferencesWindow — the app's preferences (plan M3.5).
//
// Own looper and no model access, like the mixer and the markers window: the
// main window pushes the current values IN, the window holds them as SETTINGS
// (a pick or a tick changes nothing outside the window), and Apply reads the
// whole view and posts it back as ONE message carrying the whole set
// (kMsgPrefsChanged). Cancel -- and Escape, the same B_QUIT_REQUESTED -- closes
// without posting, so what was not applied is discarded. MainWindow is the only
// place that applies and persists anything; the window never reads a Project
// and never writes the settings file. That is the settings vocabulary this
// package settled on, shared with Project Settings (Cancel + Apply).
//
// The audio-output-device row is deliberately a disabled placeholder: this
// package does not build device enumeration (the multichannel-input package,
// plan M5.3, owns that and adds the picker here — see the seam in the .cpp).
// The row says what a user can see for themselves (the system default) and the
// note under it says choosing is not available yet.
#pragma once

#include "Theme.h"                 // ThemeAware + the tokens
#include "../app/AppSettings.h"    // PrefsView mirrors its preference keys

#include <Messenger.h>
#include <Window.h>

#include <string>

class BCheckBox;
class BFilePanel;
class BMenuField;
class BMenu;
class BSlider;
class BStringView;
class BTextControl;

namespace daw {

// Everything the window shows, in one struct: what MainWindow pushes in and
// what the window posts back are the same shape, so the two cannot drift.
struct PrefsView {
    int         bufferFrames      = 512;
    int         defaultSampleRate = 48000;
    int         countInBars       = 0;
    int         metronomeSound    = 0;      // 0 click, 1 beep, 2 wood block
    float       metronomeLevel    = 0.30f;  // 0..1
    bool        monitorInput      = false;
    bool        previewNotes      = true;   // the MIDI editor's audition (51)
    bool        darkMode          = false;
    float       uiScale           = 0.0f;   // 0 = follow the system font
    std::string projectDir;
    int         autosaveSeconds   = 30;     // 0 = off
};

// window -> MainWindow: a preference changed. Carries every field of PrefsView
// (int32 "buffer", "defrate", "countin", "metsnd", "autosv"; float "metlvl",
// "uiscale"; bool "monin", "dark"; string "projdir"), so one message is the
// whole state and no field can be applied twice or missed.
constexpr uint32 kMsgPrefsChanged  = 'prch';
// MainWindow -> window: the current values (when it opens, and after a change
// that came from anywhere else).
constexpr uint32 kMsgPrefsValues   = 'prvl';
// MainWindow -> window: bring the open window to the front (single instance).
constexpr uint32 kMsgPrefsActivate = 'prac';
// The default-folder panel's answer (an entry_ref).
constexpr uint32 kMsgPrefsDirRef   = 'pdrf';

class PreferencesWindow : public BWindow, public ThemeAware {
public:
    PreferencesWindow(const PrefsView& values, BMessenger apply);

    void MessageReceived(BMessage* msg) override;
    // The cached colours of the readout lines re-taken on a theme switch (the
    // kit controls do this themselves; a BStringView does not).
    void ApplyTheme() override;

    // --- what the functional test drives ---------------------------------
    // A test cannot press a menu (BPopUpMenu::Go blocks until a choice is
    // made), so it flips a control and calls NotifyChanged() -- the Apply
    // path, so nothing the test does can go through a door a user does not
    // have -- under the window lock.
    PrefsView Values() const;             // the controls, as they stand
    void SetValues(const PrefsView& v);   // ... and push a set in
    void NotifyChanged();                 // apply: post them to the app

    BMenuField*   BufferField()   const { return fBuffer; }
    BMenuField*   RateField()     const { return fRate; }
    BMenuField*   CountInField()  const { return fCountIn; }
    BMenuField*   SoundField()    const { return fSound; }
    BSlider*      LevelSlider()   const { return fLevel; }
    BCheckBox*    MonitorCheck()  const { return fMonitor; }
    BCheckBox*    PreviewCheck()  const { return fPreview; }
    BCheckBox*    DarkCheck()     const { return fDark; }
    BMenuField*   ScaleField()    const { return fScale; }
    BTextControl* DirField()      const { return fDir; }
    BMenuField*   AutosaveField() const { return fAutosave; }
    BMenuField*   DeviceField()   const { return fDevice; }   // the M5.3 seam

private:
    void ChooseDir();             // "Choose…": the folder panel
    void BuildRows(class BView* root);
    void UpdateLevelLabel();
    void UpdateDirLabel();

    BMenuField*   fDevice    = nullptr;   // disabled: see the M5.3 seam
    BMenuField*   fBuffer    = nullptr;
    BMenuField*   fRate      = nullptr;
    BMenuField*   fCountIn   = nullptr;
    BMenuField*   fSound     = nullptr;
    BSlider*      fLevel     = nullptr;
    BStringView*  fLevelText = nullptr;
    BCheckBox*    fMonitor   = nullptr;
    BCheckBox*    fPreview   = nullptr;   // preview notes while editing
    BCheckBox*    fDark      = nullptr;
    BMenuField*   fScale     = nullptr;
    BTextControl* fDir       = nullptr;
    BMenuField*   fAutosave  = nullptr;
    BFilePanel*   fDirPanel  = nullptr;
    BMessenger    fApply;      // the main window
};

} // namespace daw
