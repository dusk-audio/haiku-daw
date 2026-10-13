#include "NewProjectWindow.h"

#include "widgets/DawButton.h"      // the kit (M1.3)
#include "widgets/DawMenuField.h"

#include <OS.h>   // find_thread (PostChosen quits from its own thread)
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <StringView.h>

namespace daw {

namespace {

enum { MSG_CREATE = 'npcr', MSG_TEMPLATE_PICKED = 'nptp' };

// A text line in this dialog. A BStringView caches the view colour it was made
// with, so a theme switch re-takes it (T1); the kit controls do this
// themselves.
class Line : public BStringView, public ThemeAware {
public:
    // `dim` for a secondary line (the hint), not for a colour: the token is
    // read at DRAW time, so a theme switch cannot leave the old mode's text on
    // the new mode's panel.
    Line(const char* name, const char* text, bool dim)
        : BStringView(name, text), fDim(dim) {
        ApplyTheme();
    }
    void ApplyTheme() override {
        SetViewColor(ColHeader());
        SetLowColor(ColHeader());
        SetHighColor(fDim ? ColTextDim() : ColText());
    }

protected:
    void AttachedToWindow() override {
        BStringView::AttachedToWindow();
        ApplyTheme();
    }

private:
    bool fDim;
};

int MarkedIndex(BMenu* menu) {
    if (menu == nullptr) return 0;
    BMenuItem* m = menu->FindMarked();
    return m ? menu->IndexOf(m) : 0;
}

} // namespace

NewProjectWindow::NewProjectWindow(std::vector<TemplateInfo> templates,
                                   std::vector<std::string> lookedIn,
                                   BMessenger apply)
    : BWindow(BRect(160, 160, 160 + 440, 160 + 220), "New Project",
              B_TITLED_WINDOW,
              B_FLOATING_APP_WINDOW_FEEL | B_NOT_ZOOMABLE | B_NOT_RESIZABLE
              | B_ASYNCHRONOUS_CONTROLS),
      fTemplates(std::move(templates)), fLookedIn(std::move(lookedIn)),
      fApply(apply) {
    // A dialog with no rows at all would be a dead end; the caller adds the
    // built-in Empty, and this is the belt to that braces.
    if (fTemplates.empty()) fTemplates.push_back(EmptyTemplate());

    BMenu* menu = new BPopUpMenu("tpl");
    for (const TemplateInfo& t : fTemplates)
        // Each row carries its own message: a null one leaves DawMenuField
        // able to mark the choice but with nothing to send, so the description
        // under the menu kept describing whichever template the dialog opened
        // with (Empty, always) while the Create button made another one.
        menu->AddItem(new ThemedMenuItem(t.name.c_str(),
                                         new BMessage(MSG_TEMPLATE_PICKED)));
    menu->ItemAt(0)->SetMarked(true);
    fTemplate = new DawMenuField("tf", "Template:", menu);

    fDescription = new Line("ds", "", false);
    fDescription->SetExplicitMinSize(BSize(B_SIZE_UNSET, Themed(32.0f)));

    // The hint's TEXT is set by UpdateDescription: it is shown only when there
    // was no template FILE to offer, so a user with a normal install never
    // reads a path. Empty rather than hidden -- a hidden view keeps its space
    // in this Haiku, and a gap where an explanation used to be is a hole in
    // the dialog.
    fHint = new Line("hn", "", true);

    DawButton* cancel = new DawButton("cx", "Cancel",
                                      new BMessage(B_QUIT_REQUESTED));
    DawButton* create = new DawButton("ok", "Create", new BMessage(MSG_CREATE));
    create->MakeDefault(true);

    BView* root = new ThemedView("root", B_WILL_DRAW);
    BLayoutBuilder::Group<>(this, B_VERTICAL).Add(root);
    BLayoutBuilder::Group<>(root, B_VERTICAL, Themed(8.0f))
        .SetInsets(Themed(10.0f))
        .Add(fTemplate)
        .Add(fDescription)
        .Add(fHint)
        .AddGroup(B_HORIZONTAL)
            .AddGlue()
            .Add(cancel)
            .Add(create)
        .End()
        .End();
    // The TEXT first, then the fit: a BStringView reports the size of the text
    // it holds, so sizing before the description is set made the window as wide
    // as an empty string -- the description ran off the right edge (clipped
    // mid-word) and the Create button was pushed out of the window entirely.
    // (UpdateDescription re-fits for the same reason: choosing another template
    // brings a longer text.)
    UpdateDescription();
    ResizeToPreferred();
}

void NewProjectWindow::UpdateDescription() {
    const int index = MarkedIndex(fTemplate ? fTemplate->Menu() : nullptr);
    const bool have = index >= 0 && index < (int)fTemplates.size();
    BString text;
    if (have) {
        const TemplateInfo& t = fTemplates[(size_t)index];
        text << t.description.c_str();
        if (t.path.empty()) text << "\n(A new session at the default rate.)";
    }
    fDescription->SetText(text.String());
    // The "where did it look" hint is only for the case it explains: no
    // template file at all, i.e. the built-in Empty and nothing beside it.
    const bool noFiles = fTemplates.size() == 1 && fTemplates[0].path.empty();
    if (fHint != nullptr) {
        if (!noFiles) {
            fHint->SetText("");
        } else {
            std::string lookedFor;
            for (const std::string& d : fLookedIn) {
                if (!lookedFor.empty()) lookedFor += "\n";
                lookedFor += d;
            }
            BString hint;
            hint << "No templates folder with project files was found.";
            if (!lookedFor.empty()) hint << "\nLooked in:\n" << lookedFor.c_str();
            fHint->SetText(hint.String());
        }
    }
    // The dialog is as wide as the text it is showing, and no wider (it is not
    // resizable): a longer description must not be clipped, and a shorter one
    // must not leave the buttons stranded at the right of empty space.
    ResizeToPreferred();
}

std::string NewProjectWindow::SelectedId() const {
    const int index = MarkedIndex(fTemplate ? fTemplate->Menu() : nullptr);
    if (index < 0 || index >= (int)fTemplates.size()) return "empty";
    return fTemplates[(size_t)index].id;
}

void NewProjectWindow::SelectId(const std::string& id) {
    if (fTemplate == nullptr) return;
    BMenu* menu = fTemplate->Menu();
    for (int32 i = 0; i < menu->CountItems(); i++)
        if (BMenuItem* it = menu->ItemAt(i))
            it->SetMarked((size_t)i < fTemplates.size()
                          && fTemplates[(size_t)i].id == id);
    UpdateDescription();
}

void NewProjectWindow::PostChosen() {
    const int index = MarkedIndex(fTemplate ? fTemplate->Menu() : nullptr);
    const bool have = index >= 0 && index < (int)fTemplates.size();
    const std::string id = have ? fTemplates[(size_t)index].id : "empty";
    const std::string path = have ? fTemplates[(size_t)index].path : "";
    if (fApply.IsValid()) {
        BMessage m(kMsgNewProjectChosen);
        m.AddString("id", id.c_str());
        m.AddString("path", path.c_str());
        fApply.SendMessage(&m);
    }
    // The window puts itself away — but ONLY from its own thread. Quit()
    // deletes the looper, and a caller that holds this window's lock (the
    // functional test drives the dialog exactly that way) would then unlock a
    // destroyed looper: "looper must be locked before proceeding", which does
    // not fail a check, it stops the whole team and wedges the run. Asking the
    // window's own looper to quit cannot race a caller's lock: the request is
    // served after the caller has unlocked.
    if (find_thread(NULL) == Thread())
        Quit();
    else
        PostMessage(B_QUIT_REQUESTED);
}

void NewProjectWindow::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case B_QUIT_REQUESTED:
            Quit();
            return;
        case MSG_CREATE:
            PostChosen();
            return;
        case MSG_TEMPLATE_PICKED:
            UpdateDescription();   // the row the user just chose
            return;
        case kMsgNewProjectActivate:
            Activate(true);
            return;
        default:
            BWindow::MessageReceived(msg);
            break;
    }
}

void NewProjectWindow::ApplyTheme() {
    ApplyThemeToWindowViews(this);
}

} // namespace daw
