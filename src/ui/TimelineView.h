// TimelineView — the custom-drawn arrangement view.
//
// Draws (left→right = time, top→bottom = tracks): a time ruler, one lane per
// track, and each clip as a block. Waveforms (from PeakCache) land in M4c;
// playhead + click-seek in M4d; clip drag in M4e. For now it renders a static
// snapshot of the Project it is given (non-owning pointer).
//
// Coordinate model: a frame maps to x via FrameToX(); horizontal scroll is a
// frame offset, zoom is frames-per-pixel. The header column (kHeaderWidth) is
// a fixed gutter on the left; time content starts after it.
#pragma once

#include "../model/Project.h"
#include "../model/PeakCache.h"

#include <View.h>

#include <map>
#include <string>

namespace daw {

class TimelineView : public BView {
public:
    // BView already has a Frame() method; without this typedef every
    // unqualified `Frame` in this class would bind to BView::Frame() instead
    // of the model's frame type. A member typedef hides the inherited name.
    using Frame = daw::Frame;

    explicit TimelineView(BRect frame, const Project* project);

    void Draw(BRect updateRect) override;

    // Frame <-> pixel mapping (content area, i.e. right of the header gutter).
    float FrameToX(Frame f) const;
    Frame XToFrame(float x) const;

    void SetProject(const Project* p) { fProject = p; Invalidate(); }

    // Waveform envelopes, keyed by clip source path. Non-owning; built once
    // on import (M4c) and shared across clips that reference the same file.
    using PeakMap = std::map<std::string, PeakCache>;
    void SetPeaks(const PeakMap* peaks) { fPeaks = peaks; Invalidate(); }

    // Move the playhead. Invalidates only the old + new columns, so the 60 Hz
    // poll doesn't repaint the whole view each tick.
    void SetPlayhead(Frame f);

private:
    void DrawRuler(BRect update);
    void DrawLanes(BRect update);
    void DrawClip(const Clip& c, BRect lane);
    void DrawClipWave(const Clip& c, BRect block);
    void DrawPlayhead();

    const Project* fProject;          // non-owning
    const PeakMap* fPeaks = nullptr;  // non-owning
    double         fFramesPerPixel;   // horizontal zoom
    Frame          fScrollFrame;      // leftmost visible frame (content x=0)
    Frame          fPlayhead = 0;     // in project frames
};

} // namespace daw
