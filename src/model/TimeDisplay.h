// TimeDisplay — the transport's position, in the four formats it cycles
// through (plan M3.1): BBT, min:sec, samples and timecode, plus the inverse
// (what a user TYPED back to a frame).
//
// Kit-free (STL only) and header-only, like the other shared helpers in
// src/model/ — the display, the typed entry and the host tests all read this
// one copy, so a format cannot drift from its parse. The UI (TransportBar's
// time view, via MainWindow) never formats a position itself.
//
// BBT is bar.beat.tick with 480 ticks per beat, walked through the tempo map's
// meter changes; min:sec is m:ss.mmm; samples is the frame count; timecode is
// HH:MM:SS:FF at 30 fps (non-drop, matching the app's default frame rate).
#pragma once

#include "TempoMap.h"   // daw::Frame, TempoMap

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace daw {

enum class TimeMode { Bbt = 0, MinSec = 1, Samples = 2, Timecode = 3 };

constexpr int    kTimeModeCount  = 4;
inline TimeMode  NextTimeMode(TimeMode m) {
    return (TimeMode)(((int)m + 1) % kTimeModeCount);
}
// The three-letter tag drawn under the big position.
inline const char* TimeModeTag(TimeMode m) {
    switch (m) {
        case TimeMode::Bbt:      return "BBT";
        case TimeMode::MinSec:   return "M:S";
        case TimeMode::Samples:  return "SMP";
        case TimeMode::Timecode: return "TC";
    }
    return "BBT";
}
// What the typed entry expects, for the hint line.
inline const char* TimeModeHint(TimeMode m) {
    switch (m) {
        case TimeMode::Bbt:      return "bar.beat.tick";
        case TimeMode::MinSec:   return "min:sec";
        case TimeMode::Samples:  return "samples";
        case TimeMode::Timecode: return "hh:mm:ss:ff";
    }
    return "";
}

constexpr int    kBbtTicksPerBeat = 480;
constexpr double kTimecodeFps     = 30.0;

// The global beat of a bar/beat/tick position, walking the meter map — the
// inverse of TempoMap::BarBeat, which is the function this must agree with
// (tests/time_display_tests.cpp checks the round trip at meter changes).
// False for a position no bar/beat can name (bar < 1, beat < 1, or a tick
// outside [0, 480)).
inline bool BarBeatToBeat(const TempoMap& map, int bar, int beat, int tick,
                          double* outBeat) {
    if (bar < 1 || beat < 1 || tick < 0 || tick >= kBbtTicksPerBeat) return false;
    const std::vector<MeterChange>& ms = map.Meters();
    if (ms.empty()) return false;
    long barsLeft = bar - 1;
    for (std::size_t i = 0; i < ms.size(); i++) {
        const int    num          = ms[i].num > 0 ? ms[i].num : 4;
        const double segStartBeat = map.BeatAt(ms[i].frame);
        // How many whole bars this meter segment holds. The last one runs to
        // wherever the position lands, so it never "runs out".
        long barsHere = barsLeft + 1;
        if (i + 1 < ms.size())
            barsHere = (long)std::floor((map.BeatAt(ms[i + 1].frame) - segStartBeat)
                                        / num + 0.5);
        if (barsLeft >= barsHere) { barsLeft -= barsHere; continue; }
        if (beat > num) return false;   // that bar has no such beat
        if (outBeat)
            *outBeat = segStartBeat + barsLeft * num + (beat - 1)
                     + (double)tick / kBbtTicksPerBeat;
        return true;
    }
    return false;
}

// The frame a bar/beat/tick addresses. The caller must have set the map's
// sampleRate to the project's (as the window does before any readout).
inline Frame FrameAtBarBeat(const TempoMap& map, int bar, int beat, int tick = 0) {
    double b = 0.0;
    if (!BarBeatToBeat(map, bar, beat, tick, &b)) return -1;
    const Frame f = map.FrameAt(b);
    return f < 0 ? 0 : f;
}

inline std::string FormatTimePosition(TimeMode mode, Frame frame,
                                      double sampleRate, const TempoMap& map) {
    if (frame < 0) frame = 0;
    char buf[64];
    switch (mode) {
        case TimeMode::Bbt: {
            int bar = 1, beat = 1;
            map.BarBeat(frame, &bar, &beat);
            // The tick within the beat: frames since the beat's own frame,
            // mapped through that beat's length.
            const double beats = map.BeatAt(frame);
            const double into  = beats - std::floor(beats);
            int tick = (int)std::floor(into * kBbtTicksPerBeat);
            if (tick < 0) tick = 0;
            if (tick >= kBbtTicksPerBeat) tick = kBbtTicksPerBeat - 1;
            std::snprintf(buf, sizeof(buf), "%d.%d.%03d", bar, beat, tick);
            break;
        }
        case TimeMode::MinSec: {
            const double sec = sampleRate > 0 ? (double)frame / sampleRate : 0.0;
            const int    mins = (int)(sec / 60.0);
            const double rem  = sec - mins * 60.0;
            std::snprintf(buf, sizeof(buf), "%d:%06.3f", mins, rem);
            break;
        }
        case TimeMode::Samples:
            std::snprintf(buf, sizeof(buf), "%lld", (long long)frame);
            break;
        case TimeMode::Timecode: {
            const double sec = sampleRate > 0 ? (double)frame / sampleRate : 0.0;
            const long long total = (long long)std::floor(sec);
            const int ff = (int)std::floor((sec - (double)total) * kTimecodeFps);
            const int ss = (int)(total % 60);
            const int mm = (int)((total / 60) % 60);
            const int hh = (int)(total / 3600);
            std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", hh, mm, ss,
                          ff < 0 ? 0 : (ff >= (int)kTimecodeFps ? (int)kTimecodeFps - 1 : ff));
            break;
        }
    }
    return std::string(buf);
}

namespace timedisplay_detail {

inline bool AllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// Split on any of `seps`, dropping empty fields. False if a field is not a
// plain non-negative number.
inline bool SplitNumbers(const std::string& text, const char* seps,
                         std::vector<double>* out) {
    std::string cur;
    auto flush = [&]() -> bool {
        if (cur.empty()) return true;
        char* end = nullptr;
        const double v = std::strtod(cur.c_str(), &end);
        if (end == cur.c_str() || *end != '\0' || v < 0.0) return false;
        out->push_back(v);
        cur.clear();
        return true;
    };
    for (char c : text) {
        bool isSep = false;
        for (const char* s = seps; *s; ++s) if (c == *s) { isSep = true; break; }
        if (isSep) { if (!flush()) return false; continue; }
        if ((c >= '0' && c <= '9') || c == '.') { cur.push_back(c); continue; }
        if (c == ' ' || c == '\t') continue;   // stray blanks are harmless
        return false;                          // anything else is not a time
    }
    if (!flush()) return false;
    return !out->empty();
}

} // namespace timedisplay_detail

// The frame a typed position names, or false when the text is not a position
// in `mode` — the caller reports that in the status line and changes nothing
// (a bad entry must never seek somewhere plausible).
inline bool ParseTimePosition(TimeMode mode, const std::string& text,
                              double sampleRate, const TempoMap& map,
                              Frame* out) {
    using namespace timedisplay_detail;
    if (sampleRate <= 0.0) return false;
    std::string t;
    for (char c : text) if (c != ',' ) t.push_back(c);   // thousands separators
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    while (!t.empty() && (t.back()  == ' ' || t.back()  == '\t')) t.pop_back();
    if (t.empty()) return false;

    switch (mode) {
        case TimeMode::Bbt: {
            std::vector<double> f;
            if (!SplitNumbers(t, ".: ", &f) || f.size() > 3) return false;
            for (double v : f) if (std::floor(v) != v) return false;
            const int bar = (int)f[0];
            int beat = 1, tick = 0;
            if (f.size() == 3)      { beat = (int)f[1]; tick = (int)f[2]; }
            else if (f.size() == 2) { beat = (int)f[1]; }
            if (tick >= kBbtTicksPerBeat) tick = kBbtTicksPerBeat - 1;
            const Frame fr = FrameAtBarBeat(map, bar, beat, tick);
            if (fr < 0) return false;
            if (out) *out = fr;
            return true;
        }
        case TimeMode::MinSec: {
            std::vector<double> f;
            if (!SplitNumbers(t, ":", &f) || f.size() > 2) return false;
            const double sec = f.size() == 1 ? f[0] : f[0] * 60.0 + f[1];
            if (out) *out = (Frame)std::floor(sec * sampleRate + 0.5);
            return true;
        }
        case TimeMode::Samples: {
            if (!AllDigits(t)) return false;
            const long long v = std::strtoll(t.c_str(), nullptr, 10);
            if (v < 0) return false;
            if (out) *out = (Frame)v;
            return true;
        }
        case TimeMode::Timecode: {
            std::vector<double> f;
            if (!SplitNumbers(t, ":", &f) || f.size() < 2 || f.size() > 4)
                return false;
            for (double v : f) if (std::floor(v) != v) return false;
            // hh:mm:ss:ff, mm:ss:ff or mm:ss -- each field a whole number.
            // Two fields are MINUTES:SECONDS (not seconds:frames): "1:30" is
            // a minute and a half either way it could be read, but every other
            // time field in this app counts down from the largest unit.
            double hh = 0, mm = 0, ss = 0, ff = 0;
            if (f.size() == 4)      { hh = f[0]; mm = f[1]; ss = f[2]; ff = f[3]; }
            else if (f.size() == 3) { mm = f[0]; ss = f[1]; ff = f[2]; }
            else                    { mm = f[0]; ss = f[1]; }
            if (mm > 59 || ss > 59 || ff >= kTimecodeFps) return false;
            const double sec = hh * 3600.0 + mm * 60.0 + ss + ff / kTimecodeFps;
            if (out) *out = (Frame)std::floor(sec * sampleRate + 0.5);
            return true;
        }
    }
    return false;
}

} // namespace daw
