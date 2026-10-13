// NewProjectWindow — File > New, with the templates (plan M3.7).
//
// Own looper and no model access, like the other dialogs: MainWindow scans the
// template directory and hands the rows in, and the choice travels back as one
// message (id + path). The project is built by MainWindow, after the
// unsaved-changes prompt the Create button leads to.
//
// The dialog ALWAYS has an Empty row: the first template when a directory has
// one, the built-in one when it does not, and a hint saying where it looked
// when it found nothing -- a missing template folder is a normal state, never a
// dialog that cannot be answered.
#pragma once

#include "Theme.h"                     // ThemeAware + the tokens
#include "../app/ProjectTemplates.h"   // TemplateInfo

#include <Messenger.h>
#include <Window.h>

#include <string>
#include <vector>

class BMenuField;
class BMenu;
class BStringView;

namespace daw {

// window -> MainWindow: Create was pressed. String "id" and string "path" (the
// latter empty for the built-in Empty).
constexpr uint32 kMsgNewProjectChosen  = 'npch';
// MainWindow -> window: bring the open dialog to the front.
constexpr uint32 kMsgNewProjectActivate = 'npac';

class NewProjectWindow : public BWindow, public ThemeAware {
public:
    // `templates` must hold at least one row (MainWindow guarantees Empty).
    // `lookedIn` names the directories that were searched, for the hint shown
    // when there was no template FILE at all.
    NewProjectWindow(std::vector<TemplateInfo> templates,
                     std::vector<std::string> lookedIn, BMessenger apply);

    void MessageReceived(BMessage* msg) override;
    void ApplyTheme() override;   // the description + hint lines

    // --- what the functional test drives ----------------------------------
    const std::vector<TemplateInfo>& Templates() const { return fTemplates; }
    std::string SelectedId() const;
    void SelectId(const std::string& id);   // marks that row (if it is there)
    void PostChosen();                      // the Create button's own path
    BMenuField*  TemplateField() const { return fTemplate; }
    BStringView* DescriptionView() const { return fDescription; }

private:
    void UpdateDescription();

    std::vector<TemplateInfo> fTemplates;
    std::vector<std::string>  fLookedIn;
    BMenuField*  fTemplate    = nullptr;
    BStringView* fDescription = nullptr;
    BStringView* fHint        = nullptr;
    BMessenger   fApply;
};

} // namespace daw
