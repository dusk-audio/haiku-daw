#include "ProjectTimebase.h"

#include <cmath>
#include <vector>

namespace daw {

Frame ScaleFrame(Frame f, double ratio) {
    if (ratio == 1.0) return f;
    return (Frame)std::llround((double)f * ratio);
}

namespace {

// A lane's breakpoints, rebuilt at the new scale. AutomationLane only mutates
// through AddPoint (which keeps the lane sorted), which is all this needs:
// scaling preserves the order.
void ScaleLane(AutomationLane& lane, double ratio) {
    if (lane.Count() == 0) return;
    std::vector<AutoPoint> points;
    points.reserve(lane.Count());
    for (size_t i = 0; i < lane.Count(); i++) {
        const AutoPoint& p = lane.At(i);
        points.push_back({ ScaleFrame(p.frame, ratio), p.value });
    }
    lane.Clear();
    for (const AutoPoint& p : points)
        lane.AddPoint(p.frame, p.value);
}

void ScaleClip(Clip& c, double ratio) {
    c.startFrame    = ScaleFrame(c.startFrame, ratio);
    c.lengthFrames  = ScaleFrame(c.lengthFrames, ratio);
    c.sourceOffset  = ScaleFrame(c.sourceOffset, ratio);
    c.fadeInFrames  = ScaleFrame(c.fadeInFrames, ratio);
    c.fadeOutFrames = ScaleFrame(c.fadeOutFrames, ratio);
}

void ScaleMidiClip(MidiClip& c, double ratio) {
    c.startFrame    = ScaleFrame(c.startFrame, ratio);
    c.lengthFrames  = ScaleFrame(c.lengthFrames, ratio);
    c.fadeInFrames  = ScaleFrame(c.fadeInFrames, ratio);
    c.fadeOutFrames = ScaleFrame(c.fadeOutFrames, ratio);
    // Notes and controller events are clip-relative frames, so they scale with
    // the clip they live in (their length too: a note's duration is time).
    for (MidiNote& n : c.notes) {
        n.startFrame   = ScaleFrame(n.startFrame, ratio);
        n.lengthFrames = ScaleFrame(n.lengthFrames, ratio);
    }
    for (MidiClipEvent& e : c.events)
        e.startFrame = ScaleFrame(e.startFrame, ratio);
}

} // namespace

void ScaleProjectTimebase(Project& p, double ratio) {
    if (!(ratio > 0.0) || !std::isfinite(ratio) || ratio == 1.0) return;

    // The transport: where the playhead is, and the loop/punch range.
    p.transport.playhead = ScaleFrame(p.transport.playhead, ratio);
    p.transport.loopStart = ScaleFrame(p.transport.loopStart, ratio);
    p.transport.loopEnd   = ScaleFrame(p.transport.loopEnd, ratio);
    p.transport.punchIn   = ScaleFrame(p.transport.punchIn, ratio);
    p.transport.punchOut  = ScaleFrame(p.transport.punchOut, ratio);

    for (Marker& m : p.markers)
        m.frame = ScaleFrame(m.frame, ratio);

    // The tempo/meter map. Its changes are frame-anchored, so they move with
    // everything else -- otherwise a tempo change at bar 5 would land in the
    // middle of bar 3. The map's own sampleRate is the caller's business (it is
    // the project's rate, and SetSampleRateCommand sets it).
    {
        const std::vector<TempoChange> tempos = p.tempoMap.Tempos();
        const std::vector<MeterChange> meters = p.tempoMap.Meters();
        // Reset() puts back a single frame-0 change, which is the anchor every
        // map must have; the rest are re-applied at the new scale.
        p.tempoMap.Reset(tempos.empty() ? p.tempoBPM : tempos.front().bpm,
                         meters.empty() ? 4 : meters.front().num,
                         meters.empty() ? 4 : meters.front().denom);
        for (const TempoChange& t : tempos)
            p.tempoMap.SetTempoAt(ScaleFrame(t.frame, ratio), t.bpm, t.ramp);
        for (const MeterChange& m : meters)
            p.tempoMap.SetMeterAt(ScaleFrame(m.frame, ratio), m.num, m.denom);
    }

    for (Track& t : p.Tracks()) {
        for (Clip& c : t.clips)          ScaleClip(c, ratio);
        for (MidiClip& c : t.midiClips)  ScaleMidiClip(c, ratio);
        // A frozen track keeps the pre-freeze copies of its own content; they
        // are part of the model (thawing restores them), so they scale too.
        for (Clip& c : t.freezeClips)         ScaleClip(c, ratio);
        for (MidiClip& c : t.freezeMidi)      ScaleMidiClip(c, ratio);
        ScaleLane(t.gainAuto, ratio);
        ScaleLane(t.panAuto, ratio);
        for (FxAutoLane& lane : t.fxAuto) ScaleLane(lane.lane, ratio);
    }
}

} // namespace daw
