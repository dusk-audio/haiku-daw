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

#include <View.h>

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

private:
    void DrawRuler(BRect update);
    void DrawLanes(BRect update);
    void DrawClip(const Clip& c, BRect lane);

    const Project* fProject;          // non-owning
    double         fFramesPerPixel;   // horizontal zoom
    Frame          fScrollFrame;      // leftmost visible frame (content x=0)
};

} // namespace daw
