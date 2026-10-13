// ProjectSettingsWindow — the project's own settings (plan M3.6).
//
// Own looper and no model access, like the Preferences window: MainWindow
// pushes the current values in, the window posts the whole set back when
// Apply is pressed, and MainWindow turns the differences into ONE undoable
// command-stack edit ("Project Settings"), so a change to four fields is one
// undo step.
//
// Apply, not apply-as-you-type: the sample rate is a whole-project edit (it
// rescales the timeline and can rewrite the media), and a tempo field cannot
// apply on every keystroke ("1" on the way to "128"). The window shows what is
// changed, the button commits it.
#pragma once

#include "Theme.h"   // ThemeAware + the tokens

#include <Messenger.h>
#include <Window.h>

#include <string>
#include <utility>
#include <vector>

class BCheckBox;
class BMenu;
class BMenuField;
class BTextView;
class BStringView;
class BTextControl;

namespace daw {

// What the window shows and what it posts back: one shape for both directions.
struct ProjectSettingsView {
    double      sampleRate    = 48000.0;
    double      tempoBPM      = 120.0;
    int         meterNum      = 4;
    int         meterDenom    = 4;
    std::string notes;
    // Does the project have any audio media? The media resample offer is only
    // meaningful then, and a control that cannot do anything is disabled.
    bool        hasMedia      = false;
    // The user's answer to that offer. Not a project field: it is how the
    // sample-rate change should be performed.
    bool        resampleMedia = false;
};

// window -> MainWindow: Apply was pressed (all of the fields above; bool
// "resample", double "rate", "tempo", int32 "mnum"/"mdenom", string "notes").
constexpr uint32 kMsgProjSettingsChanged  = 'psch';
// MainWindow -> window: the current values.
constexpr uint32 kMsgProjSettingsValues   = 'psvl';
// MainWindow -> window: bring the open window to the front.
constexpr uint32 kMsgProjSettingsActivate = 'psac';

class ProjectSettingsWindow : public BWindow, public ThemeAware {
public:
    ProjectSettingsWindow(const ProjectSettingsView& values, BMessenger apply);

    void MessageReceived(BMessage* msg) override;
    void ApplyTheme() override;   // the notes well + the hint line

    // --- what the functional test drives ----------------------------------
    ProjectSettingsView Values() const;
    void SetValues(const ProjectSettingsView& v);
    void PostChanges();           // the Apply button's own path

    BMenuField*   RateField()   const { return fRate; }
    BCheckBox*    ResampleCheck() const { return fResample; }
    BTextControl* TempoField()  const { return fTempo; }
    BMenuField*   MeterField()  const { return fMeter; }
    BTextView*    NotesView()   const { return fNotes; }

private:
    void BuildRows(class BView* root);
    void RefreshEnabled();        // the resample box follows hasMedia

    BMenuField*   fRate     = nullptr;
    BCheckBox*    fResample = nullptr;
    BTextControl* fTempo    = nullptr;
    BMenuField*   fMeter    = nullptr;
    BTextView*    fNotes    = nullptr;
    BStringView*  fHint     = nullptr;
    bool          fHasMedia = false;   // does the project have audio media?
    // The rates and meters the menus offer, in row order: the tables the fixed
    // choices come from, plus the project's own value when it is not one of
    // them (a project can legally sit at 22 050 Hz or in 11/8).
    std::vector<double> fRateValues;
    std::vector<std::pair<int, int>> fMeterValues;
    BMessenger    fApply;
};

} // namespace daw
