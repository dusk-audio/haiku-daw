// TempoMap — variable tempo + meter over the timeline (step changes).
//
// Replaces the single project tempo/meter with a sorted list of frame-anchored
// tempo changes and meter (time-signature) changes. Tempo is constant between
// changes (instant jumps; no ramps in v1). Anchoring at frames keeps frame<->
// beat conversion exact and non-circular: a beat position is just the integral
// of tempo over frames.
//
// There is always a change at frame 0 (the project's initial tempo/meter), so
// every query has a defined answer. Meter changes are expected to land on bar
// boundaries (the UI aligns them); bar numbering assumes that.
//
// Kit-free (STL only), header-only, host-testable.
#pragma once

#include "types.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace daw {

struct TempoChange {
    Frame  frame = 0;
    double bpm   = 120.0;
    // When true, tempo ramps linearly (in frames) from this change's bpm to the
    // next change's bpm across the segment; when false it holds constant (a step
    // jump at the next change). Ignored on the last change (no target).
    bool   ramp  = false;
};

struct MeterChange {
    Frame frame = 0;
    int   num   = 4;   // beats per bar
    int   denom = 4;   // beat unit (4 = quarter)
};

class TempoMap {
public:
    double sampleRate = 48000.0;

    TempoMap() {
        fTempos.push_back({0, 120.0});
        fMeters.push_back({0, 4, 4});
    }

    const std::vector<TempoChange>& Tempos() const { return fTempos; }
    const std::vector<MeterChange>& Meters() const { return fMeters; }

    // Insert or replace the tempo/meter at a frame. A change at frame 0 always
    // exists and can be replaced but not added twice. Kept sorted by frame.
    void SetTempoAt(Frame f, double bpm, bool ramp = false) {
        if (f < 0) f = 0;
        if (bpm < 1.0) bpm = 1.0;
        for (auto& t : fTempos)
            if (t.frame == f) { t.bpm = bpm; t.ramp = ramp; return; }
        fTempos.push_back({f, bpm, ramp});
        std::sort(fTempos.begin(), fTempos.end(),
                  [](const TempoChange& a, const TempoChange& b) {
                      return a.frame < b.frame;
                  });
    }
    void SetMeterAt(Frame f, int num, int denom) {
        if (f < 0) f = 0;
        if (num < 1) num = 1;
        if (denom < 1) denom = 1;
        for (auto& m : fMeters)
            if (m.frame == f) { m.num = num; m.denom = denom; return; }
        fMeters.push_back({f, num, denom});
        std::sort(fMeters.begin(), fMeters.end(),
                  [](const MeterChange& a, const MeterChange& b) {
                      return a.frame < b.frame;
                  });
    }
    // Remove a change; the mandatory frame-0 anchor is never removed.
    void RemoveTempoAt(Frame f) {
        if (f == 0) return;
        fTempos.erase(std::remove_if(fTempos.begin(), fTempos.end(),
            [&](const TempoChange& t) { return t.frame == f; }), fTempos.end());
    }
    void RemoveMeterAt(Frame f) {
        if (f == 0) return;
        fMeters.erase(std::remove_if(fMeters.begin(), fMeters.end(),
            [&](const MeterChange& m) { return m.frame == f; }), fMeters.end());
    }

    // Clear to a single initial tempo/meter (used when seeding from the old
    // single-value fields, and by ProjectIO before loading).
    void Reset(double bpm, int num, int denom) {
        fTempos.assign(1, {0, bpm < 1.0 ? 1.0 : bpm});
        fMeters.assign(1, {0, num < 1 ? 1 : num, denom < 1 ? 1 : denom});
    }

    // Instantaneous tempo at `f`. In a ramp segment it interpolates linearly
    // (in frames) between the segment's bpm and the next change's bpm.
    double BpmAt(Frame f) const {
        if (f < 0) f = 0;
        std::size_t i = SegIndex(f);
        const bool last = (i + 1 >= fTempos.size());
        if (fTempos[i].ramp && !last) {
            const Frame s = fTempos[i].frame, e = fTempos[i + 1].frame;
            if (e > s) {
                double t = (double)(f - s) / (double)(e - s);
                if (t < 0) t = 0; if (t > 1) t = 1;
                return fTempos[i].bpm + (fTempos[i + 1].bpm - fTempos[i].bpm) * t;
            }
        }
        return fTempos[i].bpm;
    }
    double FramesPerBeatAt(Frame f) const {
        return sampleRate * 60.0 / BpmAt(f);
    }

    // Cumulative beats from frame 0 to `f` (integral of tempo over frames).
    double BeatAt(Frame f) const {
        if (f <= 0) return 0.0;
        const double k = 1.0 / (sampleRate * 60.0);   // beats per (bpm*frame)
        double beats = 0.0;
        for (std::size_t i = 0; i < fTempos.size(); i++) {
            const Frame segStart = fTempos[i].frame;
            if (f <= segStart) break;
            const bool  last = (i + 1 >= fTempos.size());
            const Frame segEnd = last ? f : fTempos[i + 1].frame;
            const Frame end = std::min(f, segEnd);
            const double b0 = fTempos[i].bpm;
            if (fTempos[i].ramp && !last && segEnd > segStart) {
                const double b1 = fTempos[i + 1].bpm;
                const double N = (double)(segEnd - segStart);
                const double x = (double)(end - segStart);
                // integral of (b0 + (b1-b0)*t/N) dt from 0..x, times k
                beats += k * (b0 * x + (b1 - b0) * x * x / (2.0 * N));
            } else {
                beats += k * b0 * (double)(end - segStart);
            }
            if (f <= segEnd) break;
        }
        return beats;
    }

    // Inverse of BeatAt: the frame at a cumulative beat position.
    Frame FrameAt(double beat) const {
        if (beat <= 0.0) return 0;
        const double k = 1.0 / (sampleRate * 60.0);
        double acc = 0.0;
        for (std::size_t i = 0; i < fTempos.size(); i++) {
            const Frame segStart = fTempos[i].frame;
            const bool  last = (i + 1 >= fTempos.size());
            const Frame segEnd = last ? 0 : fTempos[i + 1].frame;
            const double b0 = fTempos[i].bpm;

            if (fTempos[i].ramp && !last && segEnd > segStart) {
                const double b1 = fTempos[i + 1].bpm;
                const double N = (double)(segEnd - segStart);
                const double segBeats = k * N * (b0 + b1) / 2.0;   // avg tempo
                if (acc + segBeats >= beat) {
                    // Solve k*(b0*x + (b1-b0)*x^2/(2N)) = beat-acc for x>=0.
                    const double rhs = beat - acc;
                    const double A = k * (b1 - b0) / (2.0 * N);
                    const double B = k * b0;
                    double x;
                    if (std::fabs(A) < 1e-18) {
                        x = rhs / B;
                    } else {
                        double disc = B * B + 4.0 * A * rhs;
                        if (disc < 0) disc = 0;
                        x = (-B + std::sqrt(disc)) / (2.0 * A);
                    }
                    return segStart + (Frame)std::llround(x);
                }
                acc += segBeats;
            } else {
                const double fpb = sampleRate * 60.0 / b0;
                const double segBeats = last ? 1e18
                                             : (double)(segEnd - segStart) / fpb;
                if (last || acc + segBeats >= beat)
                    return segStart + (Frame)std::llround((beat - acc) * fpb);
                acc += segBeats;
            }
        }
        return 0;
    }

private:
    // Index of the tempo segment containing frame `f` (last change at or before).
    std::size_t SegIndex(Frame f) const {
        std::size_t idx = 0;
        for (std::size_t i = 0; i < fTempos.size(); i++) {
            if (fTempos[i].frame <= f) idx = i; else break;
        }
        return idx;
    }
public:

    void Meter(Frame f, int* num, int* denom) const {
        const MeterChange* m = &fMeters.front();
        for (const auto& mc : fMeters) {
            if (mc.frame > f) break;
            m = &mc;
        }
        if (num)   *num = m->num;
        if (denom) *denom = m->denom;
    }

    // 1-indexed bar and beat-within-bar at a frame (BBT), meter-aware. Assumes
    // meter changes fall on bar boundaries.
    void BarBeat(Frame f, int* barOut, int* beatOut) const {
        if (f < 0) f = 0;
        const double total = BeatAt(f);
        long bars = 0;
        double segStartBeat = 0.0;
        int num = fMeters.front().num;
        for (std::size_t i = 0; i < fMeters.size(); i++) {
            num = fMeters[i].num;
            segStartBeat = BeatAt(fMeters[i].frame);
            const double segEndBeat = (i + 1 < fMeters.size())
                                      ? BeatAt(fMeters[i + 1].frame) : 1e18;
            if (total < segEndBeat) {
                const double into = total - segStartBeat;
                const long barsIn = (long)std::floor(into / num);
                bars += barsIn;
                const double beatWithin = into - barsIn * num;
                if (barOut)  *barOut  = (int)bars + 1;
                if (beatOut) *beatOut = (int)std::floor(beatWithin) + 1;
                return;
            }
            const double segBeats = segEndBeat - segStartBeat;
            bars += (long)std::floor(segBeats / num + 0.5);
        }
        if (barOut)  *barOut  = (int)bars + 1;
        if (beatOut) *beatOut = 1;
    }

private:
    std::vector<TempoChange> fTempos;   // sorted; [0].frame == 0
    std::vector<MeterChange> fMeters;   // sorted; [0].frame == 0
};

} // namespace daw
