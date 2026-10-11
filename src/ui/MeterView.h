// MeterView — the master output meter in the transport bar (M3.3).
//
// Two dB-scaled bars (L over R), each with an RMS line, a peak-hold tick and a
// clip lamp, plus a correlation strip for the stereo image. The window pushes
// the engine's readings on every ~60 Hz poll; this view owns the hold decay
// and the clip latch, and a click anywhere on it resets the latch (what a user
// does about a red light).
//
// The maths is kit-free (src/dsp/MeterMath.h) and the thresholds are the
// theme's one set (Theme.h) — this view decides nothing about levels itself.
#pragma once

#include "../dsp/MeterMath.h"


#include "Theme.h"   // ThemeAware (T1)
#include <View.h>

namespace daw {

// One poll's worth of readings, straight from the engine. During a take the
// peaks come from the recorder instead (the meter then shows the input) and
// the RMS/correlation fields are the engine's mix — the window says so where
// it builds them.
struct MeterReadings {
    float peakL = 0.0f, peakR = 0.0f;    // linear amplitude of the last block
    float rmsL  = 0.0f, rmsR  = 0.0f;    // linear RMS over the meter window
    float correlation = 0.0f;            // -1..+1 (0 = nothing to report)
    bool  clipL = false, clipR = false;  // the engine saw a clip since the reset
};

class MeterView : public BView, public ThemeAware {
public:
    void ApplyTheme() override;   // T1: the cached panel colour

    explicit MeterView(BRect frame);

    void Draw(BRect updateRect) override;
    void MouseDown(BPoint where) override;

    // Called from the UI poll. Only repaints when something moved (including
    // a still-decaying peak hold).
    void SetReadings(const MeterReadings& r);

    void ResetClip();   // the click; also clears the engine's flag via the window

    // Test hooks (ui_functional_tests), like MainWindow::IsPlaying.
    const MeterReadings& Readings() const { return fIn; }
    bool  ClipLatched() const { return fClipL.Clipped() || fClipR.Clipped(); }
    float HoldFracL() const { return fHoldL.Frac(); }
    float HoldFracR() const { return fHoldR.Frac(); }

private:
    void DrawCorrelation(BRect r);
    void DrawScaleTicks(BRect bar);   // the dB marks inside a bar

    MeterReadings fIn;
    PeakHold      fHoldL, fHoldR;
    ClipLatch     fClipL, fClipR;
    int64_t       fLastUpdateUs = 0;
};

} // namespace daw
