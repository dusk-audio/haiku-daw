// MidiExpression — what a track's continuous controllers do to a sounding voice.
//
// The third piece of the MIDI-path trio, beside MidiControl.h (channel gain and
// pan) and Sustain.h (the CC64 latch): pitch bend, the mod wheel, and the
// evaluation helpers the engine and the offline Exporter share, so a bounce is
// the same arithmetic as playback.
//
// Pitch bend is the awkward one. A voice's oscillator phase is derived from
// (frame - noteStart), which is what keeps every voice a pure function of its
// block — but multiplying that elapsed time by a bend ratio would jump the
// phase the moment the ratio steps, and the jump grows with the note's age
// (440 Hz, a second in, a semitone of bend: 26 whole cycles). So the bend is
// INTEGRATED: a voice advances by
//
//     elapsed(g) = (g - noteStart) + [Δ(g) - Δ(noteStart)]
//
// where Δ(x) = ∫_0^x (ratio(t) - 1) dt is computed from the bend events by the
// helpers here. Δ is a pure function of the frame, so the voice stays stateless;
// it is exact at every block boundary (the ratio is piecewise constant, and a
// block supplies exactly its own ratio), so the phase is continuous and a bend
// never clicks.
//
// Kit-free, header-only, host-testable.
#pragma once

#include "MidiControl.h"
#include "Project.h"
#include "VoiceExpression.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace daw {

// Full-scale bend range, in semitones. The model has no RPN / bend-sensitivity
// event, so one range applies to every track: the General MIDI default.
constexpr double kBendRangeSemitones = 2.0;

// Frequency multiplier for a 14-bit bend value (8192 = centre, no bend).
inline float BendRatio(int value14) {
    const int v = value14 < 0 ? 0 : (value14 > 16383 ? 16383 : value14);
    const double semis = ((double)v - 8192.0) / 8192.0 * kBendRangeSemitones;
    return (float)std::pow(2.0, semis / 12.0);
}

// A bend event, digested once at Load so the per-block evaluation is a binary
// search instead of a scan of the whole event list.
struct BendPoint {
    Frame  frame = 0;      // frame the ratio takes effect at
    float  ratio = 1.0f;   // multiplier from `frame` on
    double phase = 0.0;    // Δ(frame): the phase advance (frames) owed up to here
};

// Sort a track's bend events and prefix-sum their phase, so BendPhaseAt() is a
// lookup. Allocates: call it when the graph is built, never from the RT thread.
inline std::vector<BendPoint> BuildBendTimeline(
        const std::vector<MidiClipEvent>& events) {
    std::vector<std::pair<Frame, float>> raw;
    raw.reserve(events.size());
    for (const MidiClipEvent& e : events) {
        if (e.type != MidiClipEvent::PitchBend) continue;
        raw.emplace_back(e.startFrame, BendRatio(e.value));
    }
    std::sort(raw.begin(), raw.end(),
              [](const std::pair<Frame, float>& a,
                 const std::pair<Frame, float>& b) {
                  return a.first < b.first;
              });

    std::vector<BendPoint> tl;
    tl.reserve(raw.size());
    double phase = 0.0;          // Δ at the previous point
    Frame  prevF = 0;            // ratio 1.0 before the first event
    float  prevR = 1.0f;
    size_t i = 0;
    while (i < raw.size()) {
        const Frame f = raw[i].first;
        // Several bends stamped on one frame are one value: the last wins, the
        // same rule CcValueAt() applies to a controller.
        float r = raw[i].second;
        size_t j = i;
        while (j < raw.size() && raw[j].first == f) { r = raw[j].second; ++j; }
        phase += ((double)prevR - 1.0) * (double)(f - prevF);
        tl.push_back(BendPoint{ f, r, phase });
        prevF = f; prevR = r;
        i = j;
    }
    return tl;
}

// The multiplier in force at `at` (1.0 before the first bend event).
inline float BendRatioAt(const std::vector<BendPoint>& tl, Frame at) {
    auto it = std::upper_bound(tl.begin(), tl.end(), at,
                               [](Frame a, const BendPoint& p) {
                                   return a < p.frame;
                               });
    return it == tl.begin() ? 1.0f : std::prev(it)->ratio;
}

// Δ(at) = ∫_0^at (ratio(t) - 1) dt, in frames: how far ahead of plain elapsed
// time the bend has pushed the phase. The caller scales it if the render's
// frames are not the project's (the Exporter renders at the output rate).
inline double BendPhaseAt(const std::vector<BendPoint>& tl, Frame at) {
    auto it = std::upper_bound(tl.begin(), tl.end(), at,
                               [](Frame a, const BendPoint& p) {
                                   return a < p.frame;
                               });
    if (it == tl.begin()) return 0.0;
    const BendPoint& p = *std::prev(it);
    return p.phase + ((double)p.ratio - 1.0) * (double)(at - p.frame);
}

// Pre-digest each note's Δ(start) into the render-only MidiNote field, so a
// voice can subtract it from the block's Δ without any history of its own.
// Called where the notes are flattened (engine Load, exporter), off the RT path.
inline void AnnotateBendPhase(std::vector<MidiNote>& notes,
                              const std::vector<BendPoint>& tl) {
    if (tl.empty()) return;      // no bend events: every field is already 0
    for (MidiNote& n : notes)
        n.bendPhaseFrames = BendPhaseAt(tl, n.startFrame);
}

// Everything a voice needs this block, from a track's timeline + events.
// `at` and `atEnd` bracket the block in the frames the timeline is in (the
// engine's block start and end; the exporter's project frames for the block).
// `scale` converts the bend phase to the frames the render is in — 1.0 live,
// outRate/sampleRate for a bounce at another rate; the ratio is dimensionless
// and needs no scaling.
inline VoiceExpression ExpressionAt(const std::vector<BendPoint>& tl,
                                    const std::vector<MidiClipEvent>& events,
                                    Frame at, Frame atEnd, double scale = 1.0) {
    VoiceExpression x;
    const double p0 = BendPhaseAt(tl, at);
    const double p1 = BendPhaseAt(tl, atEnd);
    x.bendPhase = p0 * scale;
    // The block's MEAN ratio, so the voice's phase lands exactly on Δ(atEnd) at
    // the block's end and the next block continues from there with no step.
    if (atEnd > at) x.bendRatio = (float)(1.0 + (p1 - p0) / (double)(atEnd - at));
    x.modWheel  = (float)ModWheelAt(events, at) / 127.0f;
    return x;
}

} // namespace daw
