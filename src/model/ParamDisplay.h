// ParamDisplay — how a parameter's value becomes text.
//
// Two things a generic parameter row owes the user, and neither was there:
//   * an LV2 enumeration's value is an INDEX, so a filter type read "2" where
//     the plugin's own scale point says "Lowpass";
//   * a number with no unit is a number, not a value: "440" is a frequency
//     only because the port's name says so.
//
// Kit-free (STL only) and host-tested, deliberately: EffectsWindow.cpp cannot
// build on the host, and these rules are almost entirely edge cases -- a name
// with two bracketed groups, a scale point a float lands NEAR rather than
// exactly on, a value that is neither integral nor in range.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace daw {

// The unit a port NAME declares, as plugins conventionally write it:
// "Frequency (Hz)", "Threshold (dB)", "Attack (ms)", "Wet/Dry (%)". The LAST
// bracketed group wins, because that is the unit in the two-group style
// ("Gain (L/R) (dB)") the convention also allows; a name with no bracket has
// no declared unit, and a group that cannot be a unit at all ("a long
// parenthetical") is refused rather than printed as one. A bracketed WORD is
// taken as the unit -- nothing can tell "left" from "Hz" from the text alone,
// and the convention says that is where a unit lives.
//
// Bounded and short on purpose: a unit is one to six characters of letters,
// digits or '%'. Anything longer is a comment, not a unit.
inline std::string ParamUnitFromName(const std::string& name) {
    const size_t close = name.find_last_of(')');
    if (close == std::string::npos) return std::string();
    const size_t open = name.find_last_of('(', close);
    if (open == std::string::npos || close <= open + 1) return std::string();
    const std::string unit = name.substr(open + 1, close - open - 1);
    if (unit.size() > 6) return std::string();
    for (char c : unit) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '%' || c == '/'
                     || c == '#' || c == '\xC2' || c == '\xB0';   // the ° of "°C"
        if (!ok) return std::string();
    }
    return unit;
}

// The index of the scale point `value` lands on, or -1 for none. A tolerance
// rather than equality: a control port's value travels through float
// arithmetic (a drag, a preset, a host round trip), and an enumeration's
// points are exact -- "lands on" is what the drawing is asking about, not
// "was stored as".
inline int ScalePointIndex(const std::vector<float>& points, float value,
                           float tolerance = 1e-3f) {
    if (!(value == value)) return -1;             // NaN matches nothing
    for (size_t i = 0; i < points.size(); i++) {
        const float d = points[i] - value;
        if (d <= tolerance && d >= -tolerance) return (int)i;
    }
    return -1;
}

// The number a parameter row prints when there is no scale-point name: whole
// numbers for an integral domain or a wide range (a 20..20000 frequency is not
// helped by two decimals), two decimals otherwise.
inline std::string FormatParamNumber(float value, bool isInteger,
                                     float mn, float mx) {
    if (!(value == value)) return "--";           // NaN
    char buf[48];
    std::snprintf(buf, sizeof(buf),
                  (isInteger || (mx - mn) >= 100.0f) ? "%.0f" : "%.2f", value);
    return std::string(buf);
}

// The whole readout for a generic parameter row: the scale point's own name
// when the value lands on one, the number otherwise, and the port's declared
// unit appended where there is one. A scale-point name carries its own unit
// ("Lowpass" needs none), so the unit is NOT appended to it.
inline std::string ParamValueText(float value, bool isInteger, float mn,
                                  float mx, const std::string& unit,
                                  const std::vector<float>* scalePoints,
                                  const std::vector<std::string>* scaleNames) {
    if (scalePoints != nullptr && scaleNames != nullptr) {
        const int i = ScalePointIndex(*scalePoints, value);
        if (i >= 0 && (size_t)i < scaleNames->size()
            && !(*scaleNames)[(size_t)i].empty())
            return (*scaleNames)[(size_t)i];
    }
    std::string text = FormatParamNumber(value, isInteger, mn, mx);
    if (!unit.empty()) text += " " + unit;
    return text;
}

} // namespace daw
