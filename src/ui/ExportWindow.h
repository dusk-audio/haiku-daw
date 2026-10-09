// ExportWindow — the bounce options: what to render, in what format, and what
// to do to it on the way out.
//
// Like RenameWindow/QuantizeWindow it never touches the model or the engine:
// the Export button posts its choices to MainWindow, which remembers them
// (AppSettings), asks for a destination with the usual file panel and runs the
// export on a worker thread with progress + cancel.
#pragma once

#include <Messenger.h>
#include <Window.h>

class BCheckBox;
class BMenuField;
class BPopUpMenu;
class BTextControl;

namespace daw {

// ExportWindow -> MainWindow: render with these choices.
// int32 "bits" (16/24/32), bool "dither", int32 "rate" (0 = project rate),
// bool "norm", float "lufs", float "ceil", bool "lim",
// int32 "range" (0 = whole project, 1 = loop range), int32 "stems" (0/1).
constexpr uint32 kMsgExportOptions = 'exop';

// MainWindow -> the progress window: float "f" (0..1). Sent on the pulse while
// an export runs.
constexpr uint32 kMsgExportProgress = 'expg';

// The progress window's Cancel -> MainWindow: stop the running export.
constexpr uint32 kMsgExportCancel = 'excn';

// The export choices, as the dialog reads and writes them. Kept as its own
// small struct so the dialog and MainWindow agree without either including the
// other's headers.
struct ExportChoices {
    int   bitDepth   = 16;
    bool  dither     = true;
    int   sampleRate = 0;      // 0 = the project's own rate
    bool  normalize  = false;
    float targetLufs = -14.0f;
    float truePeak   = -1.0f;
    bool  limiter    = false;
    int   range      = 0;      // 0 = whole project, 1 = loop range
    bool  stems      = false;
};

class ExportWindow : public BWindow {
public:
    ExportWindow(BRect frame, const ExportChoices& current, bool stems,
                 BMessenger apply);
    void MessageReceived(BMessage* msg) override;

private:
    ExportChoices fCur;
    BMessenger    fApply;
    BPopUpMenu*   fBits    = nullptr;
    BCheckBox*    fDither  = nullptr;
    BPopUpMenu*   fRate    = nullptr;
    BCheckBox*    fNorm    = nullptr;
    BTextControl* fLufs    = nullptr;
    BTextControl* fCeil    = nullptr;
    BCheckBox*    fLim     = nullptr;
    BPopUpMenu*   fRange   = nullptr;
    BCheckBox*    fStems   = nullptr;
};

} // namespace daw
