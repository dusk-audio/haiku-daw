#include "SampleBrowser.h"

#include "UiMetrics.h"
#include "../storage/BfsAttr.h"

#include <Button.h>
#include <Entry.h>
#include <ListView.h>
#include <Path.h>
#include <Query.h>
#include <ScrollView.h>
#include <StringItem.h>
#include <TextControl.h>
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
    DragListView(BRect f, const char* n, list_view_type t, uint32 mode)
        : BListView(f, n, t, mode) {}
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

SampleBrowser::SampleBrowser(BRect frame, BMessenger target)
    : BWindow(frame, "Sample Browser", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fTarget(target) {
    BView* root = new BView(Bounds(), "root", B_FOLLOW_ALL_SIDES, B_WILL_DRAW);
    root->SetViewColor(ColHeader());
    AddChild(root);

    const float w = Bounds().Width();
    fFilter = new BTextControl(BRect(8, 8, w - 96, 30), "filter", "Find:",
                               "", new BMessage(MSG_SEARCH));
    fFilter->SetDivider(36.0f);
    root->AddChild(fFilter);
    BButton* search = new BButton(BRect(w - 88, 6, w - 8, 30), "search",
                                  "Search", new BMessage(MSG_SEARCH));
    root->AddChild(search);

    BRect lr(8, 38, w - 8 - B_V_SCROLL_BAR_WIDTH, Bounds().Height() - 40);
    DragListView* dlv = new DragListView(lr, "list", B_SINGLE_SELECTION_LIST,
                                         B_FOLLOW_ALL_SIDES);
    dlv->fPaths = &fPaths;   // stable member; drag rows carry their path
    fList = dlv;
    fList->SetInvocationMessage(new BMessage(MSG_PICK));
    BScrollView* sv = new BScrollView("sv", fList, B_FOLLOW_ALL_SIDES, 0,
                                      false, true);
    root->AddChild(sv);

    const float by = Bounds().Height() - 34;
    fBpm = new BTextControl(BRect(8, by, 150, by + 22), "bpm", "BPM:", "",
                            new BMessage(MSG_TAGBPM));
    fBpm->SetDivider(34.0f);
    root->AddChild(fBpm);
    BButton* tag = new BButton(BRect(158, by - 2, 260, by + 22), "tag",
                               "Tag BPM", new BMessage(MSG_TAGBPM));
    root->AddChild(tag);

    RunQuery();
}

std::string SampleBrowser::SelectedPath() const {
    const int32 sel = fList->CurrentSelection();
    if (sel < 0 || sel >= (int32)fPaths.size()) return std::string();
    return fPaths[(size_t)sel];
}

void SampleBrowser::RunQuery() {
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

void SampleBrowser::MessageReceived(BMessage* msg) {
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
            BWindow::MessageReceived(msg);
    }
}

} // namespace daw
