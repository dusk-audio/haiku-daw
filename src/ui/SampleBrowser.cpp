#include "SampleBrowser.h"

#include "UiMetrics.h"
#include "../storage/BfsAttr.h"

#include "widgets/DawButton.h"      // the kit (M1.3)
#include <Entry.h>
#include <ListView.h>
#include <Path.h>
#include <Query.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <ScrollView.h>
#include <StringItem.h>
#include "widgets/DawTextField.h"
#include <Volume.h>
#include <VolumeRoster.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace daw {

enum {
    MSG_SEARCH = 'srch',   // filter changed / Search pressed
    MSG_PICK   = 'pick',   // list invoked (double-click)
    MSG_TAGBPM = 'tbpm',   // write DAW:bpm on the selection
};

// A list view whose rows can be dragged onto the timeline. It carries a pointer
// to the browser's parallel path vector so the drag message holds the file path.
class DragListView : public BListView {
public:
    DragListView(const char* n, list_view_type t)
        : BListView(n, t) {}
    const std::vector<std::string>* fPaths = nullptr;
    bool InitiateDrag(BPoint, int32 index, bool) override {
        if (!fPaths || index < 0 || index >= (int)fPaths->size()) return false;
        BMessage drag(kMsgSampleDrag);
        drag.AddString("path", (*fPaths)[(size_t)index].c_str());
        DragMessage(&drag, ItemFrame(index), this);
        return true;
    }
};

// True if the path looks like an audio file we can import.
static bool IsAudioPath(const char* p) {
    const char* dot = std::strrchr(p, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".aif") == 0
        || strcasecmp(dot, ".aiff") == 0;
}

void SampleBrowserView::ApplyTheme() {
    ApplyWellColors(fList);
    ApplyWellColors(fScroll);
}

SampleBrowserView::SampleBrowserView(BMessenger target)
    : BGroupView("samplebrowserview", B_VERTICAL, 0.0f), fTarget(target) {
    SetViewColor(ColHeader());
    SetLowColor(ColHeader());
    BGroupLayout* g = GroupLayout();
    g->SetInsets(8.0f, 8.0f, 8.0f, 8.0f);
    g->SetSpacing(6.0f);

    fFilter = new DawTextField("filter", "Find:", "", new BMessage(MSG_SEARCH));
    fFilter->SetDivider(36.0f);
    fFilter->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, Themed(24.0f)));
    DawButton* search = new DawButton("search", "Search", new BMessage(MSG_SEARCH));
    search->SetExplicitMaxSize(BSize(Themed(90.0f), Themed(24.0f)));
    {
        BGroupView* row = new BGroupView(B_HORIZONTAL, 6.0f);
        row->GroupLayout()->AddView(fFilter, 1.0f);
        row->GroupLayout()->AddView(search, 0.0f);
        g->AddView(row);
    }

    DragListView* list = new DragListView("list", B_SINGLE_SELECTION_LIST);
    list->fPaths = &fPaths;   // stable member; drag rows carry their path
    fList = list;
    fList->SetInvocationMessage(new BMessage(MSG_PICK));
    fScroll = new BScrollView("sv", fList, 0, false, true);
    g->AddView(fScroll, 1.0f);

    fBpm = new DawTextField("bpm", "BPM:", "", new BMessage(MSG_TAGBPM));
    fBpm->SetDivider(34.0f);
    fBpm->SetExplicitMaxSize(BSize(Themed(150.0f), Themed(24.0f)));
    DawButton* tag = new DawButton("tag", "Tag BPM", new BMessage(MSG_TAGBPM));
    tag->SetExplicitMaxSize(BSize(Themed(110.0f), Themed(24.0f)));
    {
        BGroupView* row = new BGroupView(B_HORIZONTAL, 6.0f);
        row->GroupLayout()->AddView(fBpm, 0.0f);
        row->GroupLayout()->AddView(tag, 0.0f);
        row->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
        g->AddView(row);
    }
}

void SampleBrowserView::AttachedToWindow() {
    BGroupView::AttachedToWindow();
    ApplyTheme();   // the list is a well: the theme's colour, not the stock one
    if (fPaths.empty()) RunQuery();   // an empty query on the first showing
}

std::string SampleBrowserView::SelectedPath() const {
    const int32 sel = fList->CurrentSelection();
    if (sel < 0 || sel >= (int32)fPaths.size()) return std::string();
    return fPaths[(size_t)sel];
}

void SampleBrowserView::RunQuery() {
    fList->MakeEmpty();
    fPaths.clear();

    BVolume boot;
    if (BVolumeRoster().GetBootVolume(&boot) != B_OK) return;

    std::string filter = fFilter ? fFilter->Text() : "";
    if (filter.empty()) filter = ".wav";   // default: all wavs

    BQuery query;
    query.SetVolume(&boot);
    query.PushAttr("name");
    query.PushString(filter.c_str(), true /*case-insensitive*/);
    query.PushOp(B_CONTAINS);
    if (query.Fetch() != B_OK) return;

    BEntry entry;
    int count = 0;
    while (query.GetNextEntry(&entry) == B_OK && count < 500) {
        BPath p;
        if (entry.GetPath(&p) != B_OK) continue;
        if (!IsAudioPath(p.Path())) continue;

        float dur = 0.0f, bpm = 0.0f;
        ReadAttrFloat(p.Path(), kAttrDuration, &dur);
        ReadAttrFloat(p.Path(), kAttrBpm, &bpm);

        char line[512];
        const char* leaf = p.Leaf();
        if (bpm > 0.0f)
            std::snprintf(line, sizeof(line), "%s   [%.1fs, %.0f BPM]",
                          leaf, dur, bpm);
        else
            std::snprintf(line, sizeof(line), "%s   [%.1fs]", leaf, dur);
        fList->AddItem(new BStringItem(line));
        fPaths.push_back(p.Path());
        count++;
    }
}

void SampleBrowserView::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case MSG_SEARCH:
            RunQuery();
            break;
        case MSG_PICK: {
            const std::string path = SelectedPath();
            if (!path.empty()) {
                BMessage m(kMsgBrowserImport);
                m.AddString("path", path.c_str());
                fTarget.SendMessage(&m);
            }
            break;
        }
        case MSG_TAGBPM: {
            const std::string path = SelectedPath();
            if (!path.empty() && fBpm) {
                const float v = (float)atof(fBpm->Text());
                if (v > 0.0f) {
                    EnsureDawIndexes(path.c_str());
                    WriteAttrFloat(path.c_str(), kAttrBpm, v);
                    RunQuery();   // refresh the shown BPM
                }
            }
            break;
        }
        default:
            BGroupView::MessageReceived(msg);
    }
}

// --- the standalone window -------------------------------------------------

SampleBrowser::SampleBrowser(BRect frame, BMessenger target)
    : BWindow(frame, "Sample Browser", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS) {
    fView = new SampleBrowserView(target);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0.0f).Add(fView).End();
}

} // namespace daw
