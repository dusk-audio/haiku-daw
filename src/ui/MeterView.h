// MeterView — a small stereo output-level meter for the transport bar.
//
// The window pushes the engine's per-channel block peak each poll; this view
// just draws two horizontal bars. Levels are linear amplitude in [0, 1+];
// anything >= 1.0 lights the clip zone.
#pragma once


#include "Theme.h"   // ThemeAware (T1)
#include <View.h>

namespace daw {

class MeterView : public BView, public ThemeAware {
public:
    void ApplyTheme() override;   // T1: the cached panel colour

    explicit MeterView(BRect frame);

    void Draw(BRect updateRect) override;

    // Called from the UI poll (~60 Hz). Only repaints when the levels move.
    void SetLevels(float l, float r);

private:
    void DrawBar(BRect r, float level);

    float fL = 0.0f;
    float fR = 0.0f;
};

} // namespace daw
