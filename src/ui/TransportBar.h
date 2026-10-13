// TransportBar — the custom-drawn transport strip (M3.1): the transport
// buttons and the zoom pair, the LCD well with the big position and its mode,
// and the two status lines. The other transport widgets (the loudness readout,
// the master volume, the tempo field, the master meter) are added to it as
// children by MainWindow, which pins them (see MainWindow::LayoutTransportBar).
//
// Every button draws its own state and posts one message to the window; the
// state itself lives in the window, the transport controller and the model
// (playing, recording, loop, metronome, punch) — this view only shows it.
#pragma once


#include "Theme.h"   // ThemeAware (T1)
#include "UiMetrics.h"
#include <Messenger.h>
#include <View.h>

#include <string>

namespace daw {

// The message each control posts. The window owns the handlers; the ids come
// from UiMetrics.h's cross-view block (and MainWindow's own MSG_PLAY/STOP/REC
// for the three the bar has always had).
struct TransportActions {
    uint32 play       = 0;
    uint32 stop       = 0;
    uint32 rec        = 0;
    uint32 zoomOut    = 0;
    uint32 zoomIn     = 0;
    uint32 rtz        = 0;   // return to zero
    uint32 rewind     = 0;
    uint32 forward    = 0;
    uint32 loop       = 0;
    uint32 click      = 0;   // metronome ("click")
    uint32 punch      = 0;
};

// The bar's controls, in drawing order. Hit-testing and drawing both come
// through ButtonRect(), so a click cannot land somewhere else than the button
// that was drawn (the functional tests ask for these rects too instead of
// computing an offset — docs/HANDOFF.md, "a synthetic click ...").
enum class TransportButton {
    Rtz, Rewind, Forward, Play, Stop, Rec, Loop, Click, Punch, ZoomOut, ZoomIn,
    Count
};
BRect ButtonRect(TransportButton which);

// The LCD well: the big position and its mode tag. Not a control the window
// places — the bar owns it, so a width change cannot leave it behind.
BRect LcdWellRect(float barHeight);

// The big position display (M3.1). Click cycles the format (the window owns
// the mode and pushes the formatted text back); double-click starts typed
// entry — the view takes focus, shows what is typed with a caret, Enter posts
// kMsgTimeEntry with the text, Escape cancels. A click that lands while
// editing ends the gesture without a seek.
class TransportTimeView : public BView, public ThemeAware {
public:
    void ApplyTheme() override;   // T1: the LCD well's cached colour

    TransportTimeView(BRect frame, BMessenger target);

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void KeyDown(const char* bytes, int32 numBytes) override;
    void MakeFocus(bool focused = true) override;
    void Pulse() override {}   // no blinking caret: a redraw on the poll is enough

    void SetPosition(const char* text, const char* tag);
    // The readout line under the position (tempo, meter, DSP load, xruns).
    void SetInfoLine(const char* text);
    bool Editing() const { return fEditing; }
    // Test hooks (ui_functional_tests): what the display is showing, and what
    // is being typed into it.
    const std::string& Typed() const { return fTyped; }
    const std::string& PositionText() const { return fText; }
    const std::string& TagText() const { return fTag; }
    const std::string& InfoText() const { return fInfo; }

private:
    BMessenger  fTarget;
    std::string fText;     // what the window formatted
    std::string fTag;      // "BBT" / "M:S" / "SMP" / "TC"
    std::string fInfo;     // the readout line
    std::string fTyped;    // the in-progress entry
    bool        fEditing = false;
};

class TransportBar : public BView, public ThemeAware {
public:
    void ApplyTheme() override;   // T1: the cached bar colour

    TransportBar(BRect frame, BMessenger target,
                 const TransportActions& actions);

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void GetPreferredSize(float* width, float* height) override;
    BSize MinSize() override;
    BSize MaxSize() override;
    // The bar's CONTENTS are pinned offsets the window places (see
    // MainWindow::LayoutTransportBar). The bar's width can change without the
    // WINDOW resizing -- a pane re-layout does it -- so the window has to hear
    // about it, or everything pinned to the right edge (the master meter)
    // stays where the last window resize put it.
    void FrameResized(float width, float height) override;

    void SetPlaying(bool p)   { if (p != fPlaying)   { fPlaying = p;   Invalidate(); } }
    void SetRecording(bool r) { if (r != fRecording) { fRecording = r; Invalidate(); } }
    void SetLoop(bool on)     { if (on != fLoop)     { fLoop = on;     Invalidate(); } }
    void SetClick(bool on)    { if (on != fClick)    { fClick = on;    Invalidate(); } }
    void SetPunch(bool on)    { if (on != fPunch)    { fPunch = on;    Invalidate(); } }

    // The status row: what just happened (left) and the tempo/meter/load
    // readouts (right). The window owns both strings — it is the only place
    // that knows the tempo map, the engine and the jobs.
    void SetStatus(const char* text);
    void SetInfo(const char* text);
    // Test hook: the status line as it was last set.
    const std::string& StatusText() const { return fStatus; }
    // The LCD needs a legible width: the window's layout hides it (and the
    // time view with it) before it would be squeezed under that.
    void SetLcdVisible(bool on);

    TransportTimeView* TimeView() const { return fTime; }

private:
    TransportButton ButtonAt(BPoint where) const;
    uint32 MessageFor(TransportButton which) const;

    BMessenger       fTarget;
    TransportActions fAct;
    TransportTimeView* fTime = nullptr;
    std::string      fStatus;
    std::string      fInfo;
    bool fPlaying = false, fRecording = false;
    bool fLoop = false, fClick = false, fPunch = false;
    bool fLcdOn = true;
};

} // namespace daw
