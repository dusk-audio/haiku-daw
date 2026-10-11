// TransportBar — the custom-drawn transport strip (dark Play/Stop/Rec buttons
// with icons + state). Other transport widgets (time/BBT, Vol, BPM, loudness,
// master meter) are added to it as children by MainWindow. Buttons post their
// message to the window; SetPlaying/SetRecording light them.
#pragma once


#include "Theme.h"   // ThemeAware (T1)
#include <Messenger.h>
#include <View.h>

namespace daw {

class TransportBar : public BView, public ThemeAware {
public:
    void ApplyTheme() override;   // T1: the cached bar colour

    TransportBar(BRect frame, BMessenger target,
                 uint32 playWhat, uint32 stopWhat, uint32 recWhat,
                 uint32 zoomOutWhat, uint32 zoomInWhat);

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

private:
    BMessenger fTarget;
    uint32 fPlay, fStop, fRec, fZoomOut, fZoomIn;
    bool   fPlaying = false, fRecording = false;
};

} // namespace daw
