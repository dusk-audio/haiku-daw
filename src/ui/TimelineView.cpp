#include "TimelineView.h"

#include "UiMetrics.h"

#include <cstdio>

namespace daw {

TimelineView::TimelineView(BRect frame, const Project* project)
    : BView(frame, "timeline", B_FOLLOW_ALL_SIDES,
            B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
      fProject(project),
      fFramesPerPixel(kDefaultFramesPerPixel),
      fScrollFrame(0) {
    SetViewColor(ColBackground());
}

float TimelineView::FrameToX(Frame f) const {
    return kHeaderWidth
         + static_cast<float>((f - fScrollFrame) / fFramesPerPixel);
}

Frame TimelineView::XToFrame(float x) const {
    return fScrollFrame
         + static_cast<Frame>((x - kHeaderWidth) * fFramesPerPixel);
}

void TimelineView::Draw(BRect updateRect) {
    DrawLanes(updateRect);
    DrawPlayhead();          // over lanes, under the ruler
    DrawRuler(updateRect);   // ruler last so it sits above lane content
}

void TimelineView::SetPlayhead(Frame f) {
    if (f == fPlayhead)
        return;
    const float xOld = FrameToX(fPlayhead);
    const float xNew = FrameToX(f);
    fPlayhead = f;
    // Repaint the two 1-px columns (a hair wide for the AA'd line) from the
    // ruler bottom to the view bottom.
    BRect b = Bounds();
    Invalidate(BRect(xOld - 1, kRulerHeight, xOld + 1, b.bottom));
    Invalidate(BRect(xNew - 1, kRulerHeight, xNew + 1, b.bottom));
}

void TimelineView::DrawPlayhead() {
    const float x = FrameToX(fPlayhead);
    if (x < kHeaderWidth || x > Bounds().right)
        return;
    SetHighColor(ColPlayhead());
    StrokeLine(BPoint(x, kRulerHeight), BPoint(x, Bounds().bottom));
}

void TimelineView::DrawRuler(BRect update) {
    BRect r = Bounds();
    r.bottom = kRulerHeight;

    SetHighColor(ColRuler());
    FillRect(r);

    if (!fProject)
        return;

    // Tick marks every second along the visible span. Convert the content's
    // left/right pixels back to frames, then step in one-second increments.
    const double rate = fProject->sampleRate;
    const Frame  secFrames = static_cast<Frame>(rate);
    const Frame  leftFrame  = XToFrame(kHeaderWidth);
    const Frame  rightFrame = XToFrame(Bounds().right);

    Frame firstSec = (leftFrame / secFrames) * secFrames;
    if (firstSec < 0) firstSec = 0;

    SetHighColor(ColGrid());
    SetLowColor(ColRuler());
    for (Frame f = firstSec; f <= rightFrame; f += secFrames) {
        const float x = FrameToX(f);
        if (x < kHeaderWidth) continue;
        StrokeLine(BPoint(x, 0), BPoint(x, kRulerHeight));

        char label[16];
        std::snprintf(label, sizeof(label), "%lld s",
                      static_cast<long long>(f / secFrames));
        SetHighColor(ColText());
        DrawString(label, BPoint(x + 3, kRulerHeight - 8));
        SetHighColor(ColGrid());
    }
}

void TimelineView::DrawLanes(BRect update) {
    if (!fProject)
        return;

    float y = kRulerHeight;
    int   idx = 0;
    for (const Track& t : fProject->Tracks()) {
        BRect lane(0, y, Bounds().right, y + kTrackHeight);

        SetHighColor((idx & 1) ? ColLaneAlt() : ColLane());
        FillRect(lane);

        // Second-grid lines through the lane content area.
        const double rate = fProject->sampleRate;
        const Frame  secFrames = static_cast<Frame>(rate);
        const Frame  leftFrame  = XToFrame(kHeaderWidth);
        const Frame  rightFrame = XToFrame(Bounds().right);
        Frame firstSec = (leftFrame / secFrames) * secFrames;
        if (firstSec < 0) firstSec = 0;
        SetHighColor(ColGrid());
        for (Frame f = firstSec; f <= rightFrame; f += secFrames) {
            const float x = FrameToX(f);
            if (x < kHeaderWidth) continue;
            StrokeLine(BPoint(x, lane.top), BPoint(x, lane.bottom));
        }

        for (const Clip& c : t.clips)
            DrawClip(c, lane);

        y += kTrackHeight + kTrackGap;
        idx++;
    }
}

void TimelineView::DrawClip(const Clip& c, BRect lane) {
    float x0 = FrameToX(c.startFrame);
    float x1 = FrameToX(c.startFrame + c.lengthFrames);
    if (x1 < kHeaderWidth || x0 > lane.right)
        return;                       // fully outside the content area
    if (x0 < kHeaderWidth) x0 = kHeaderWidth;

    BRect block(x0, lane.top + 3, x1, lane.bottom - 3);
    SetHighColor(ColClip());
    FillRect(block);

    DrawClipWave(c, block);

    SetHighColor(ColClipBorder());
    StrokeRect(block);

    // Clip label (source file basename), clipped to the block width.
    const std::string& p = c.sourcePath;
    size_t slash = p.find_last_of('/');
    std::string name = (slash == std::string::npos) ? p : p.substr(slash + 1);
    SetHighColor(ColText());
    DrawString(name.c_str(), BPoint(block.left + 4, block.top + 14));
}

// Paint the min/max envelope inside a clip block: one vertical line per pixel
// column, from the column's min sample to its max. Reads a handful of peak
// buckets per column (never scans the audio). No cache for this clip's source
// -> just the flat filled block.
void TimelineView::DrawClipWave(const Clip& c, BRect block) {
    if (!fPeaks)
        return;
    auto it = fPeaks->find(c.sourcePath);
    if (it == fPeaks->end() || !it->second.IsValid())
        return;
    const PeakCache& pc = it->second;

    const float mid  = (block.top + block.bottom) * 0.5f;
    const float half = (block.bottom - block.top) * 0.5f - 1.0f;

    SetHighColor(ColWave());
    const int xL = static_cast<int>(block.left);
    const int xR = static_cast<int>(block.right);
    for (int x = xL; x <= xR; x++) {
        // Timeline frames this column spans -> source frames within the clip.
        const Frame tf0 = XToFrame(static_cast<float>(x));
        const Frame tf1 = XToFrame(static_cast<float>(x + 1));
        const Frame s0 = c.sourceOffset + (tf0 - c.startFrame);
        const Frame s1 = c.sourceOffset + (tf1 - c.startFrame);
        if (s1 <= 0)
            continue;

        Peak pk = pc.Range(s0 < 0 ? 0 : s0, s1);
        const float yMax = mid - pk.max * half;   // max amplitude -> up
        const float yMin = mid - pk.min * half;   // min amplitude -> down
        StrokeLine(BPoint(x, yMin), BPoint(x, yMax));
    }
}

} // namespace daw
