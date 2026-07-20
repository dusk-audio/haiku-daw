// Lv2PortMap — the pure decisions the LV2 host makes about a plugin's ports.
//
// Everything here is plain data + arithmetic: no lilv, no Haiku kits, no I/O.
// Lv2Host.cpp reads a plugin's RDF with lilv, flattens it into a vector of
// Lv2PortSpec, and hands it to BuildLv2PortLayout — so the rules that actually
// decide whether a plugin is hostable (topology, which port gets connected
// where, which control port is THE latency port, what order param slots come
// in) are testable on any machine, with or without lilv installed, against
// hand-written port tables. That is deliberate: those rules are where the bugs
// live, and they are exactly the part lilv contributes nothing to.
//
// Kit-free (STL only).
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace daw {

// The largest block, in frames, this host will ever hand to a single
// lilv_instance_run(). Every scratch buffer in an Lv2Effect is sized to it once,
// off the RT thread, and Process() chunks anything longer.
//
// It is a hard promise, not a guess, and both halves matter:
//   - We tell plugins about it. Nearly every real-world plugin requires
//     bufsz:boundedBlockLength and options:options; we satisfy both by
//     advertising this value as bufsz:maxBlockLength. A plugin may size its own
//     internal buffers from it, so exceeding it corrupts memory inside the
//     plugin rather than merely glitching.
//   - Nothing bounds the caller. Engine::SetBufferFrames accepts any n >= 32
//     (src/engine/Engine.h) with no upper limit, so NO constant can be proven
//     large enough by inspecting the callers. Chunking in Process is therefore
//     the only correct design, and this constant is what makes the promise above
//     keepable however the device is configured.
// 8192 matches the Exporter's offline block size (Exporter::applyFx), so an
// offline bounce never chunks; live buffers are far smaller still.
inline constexpr int kMaxLv2BlockFrames = 8192;

// How one port participates.
//
// Atom ports are called out separately from Unknown for a memory-safety reason,
// not a tidiness one. Every port MUST be connected (LV2 spec), and an atom port
// can be satisfied with an inert empty-sequence buffer whose capacity WE choose.
// A CV port cannot: it expects one float per frame, so pointing it at that same
// small buffer invites the plugin to write a whole block past the end of it.
// Anything we cannot size correctly is Unknown, and a REQUIRED Unknown port
// makes the plugin unhostable rather than hosted unsafely.
enum class Lv2PortRole {
    AudioIn, AudioOut,
    ControlIn, ControlOut,
    AtomIn, AtomOut,
    Unknown,
};

struct Lv2PortSpec {
    uint32_t    index    = 0;
    Lv2PortRole role     = Lv2PortRole::Unknown;
    bool        optional = false;   // lv2:portProperty lv2:connectionOptional

    // The two RDF mechanisms for "this output control port reports latency".
    // They are NOT interchangeable and are read separately:
    //   designation — `lv2:designation lv2:latency`, the current, correct one.
    //   property    — `lv2:portProperty lv2:reportsLatency`, deprecated.
    // BuildLv2PortLayout prefers the designation and only falls back to the
    // property, so a plugin declaring both (every one installed on this machine
    // does) resolves through the supported mechanism.
    bool latencyDesignation = false;
    bool latencyProperty    = false;

    // Control ports only. A plugin need not declare a range; hasMin/hasMax say
    // whether it did, so an absent bound means "unbounded" instead of silently
    // clamping every value to a fabricated 0..1.
    float mn = 0.0f, mx = 1.0f, def = 0.0f;
    bool  hasMin = false, hasMax = false;

    // What values the port actually accepts. These are three DIFFERENT domains
    // and collapsing them to "whole numbers" is wrong for two of them:
    //   isInteger     — lv2:integer: any whole number in range.
    //   isToggled     — lv2:toggled: only the two endpoints, nothing between.
    //   scalePoints   — lv2:enumeration: only the listed values, which may be
    //                   SPARSE (0, 2, 5) so "an integer in range" would happily
    //                   pass 1, 3 and 4, none of which the plugin defines.
    // isInteger stays true for all three (they are all non-continuous), so
    // existing callers keep working; the finer flags refine it.
    bool  isInteger = false;
    bool  isToggled = false;
    std::vector<float> scalePoints;   // lv2:enumeration values, if declared

    std::string name;
};

// Audio-port shapes v1 can host.
//   Stereo   — 2 required audio in, 2 required audio out: one instance.
//   MonoDual — 1 in, 1 out: TWO instances, one per channel, sharing one set of
//              control values so both channels track a single knob.
//   Unsupported — everything else: instruments (no audio in), analyzers (no
//              audio out), and plugins whose extra audio ports are mandatory.
enum class Lv2Topology { Unsupported, Stereo, MonoDual };

// Classify from the count of REQUIRED audio ports — ports the plugin marked
// lv2:connectionOptional do not count, because the spec lets a host leave them
// connected to NULL. That is what makes a stereo compressor with an optional
// stereo sidechain (Multi-Comp: 4 audio in, 2 out) hostable as a plain stereo
// insert today — its sidechain pair is optional, so it classifies as Stereo and
// those two ports get NULL. Feeding them a real signal is the sidechain
// package's job; this rule is what leaves the door open for it.
inline Lv2Topology ClassifyLv2Topology(int requiredIn, int requiredOut) {
    if (requiredIn == 2 && requiredOut == 2) return Lv2Topology::Stereo;
    if (requiredIn == 1 && requiredOut == 1) return Lv2Topology::MonoDual;
    return Lv2Topology::Unsupported;
}

// Number of plugin instances a topology needs.
inline int Lv2InstanceCount(Lv2Topology t) {
    switch (t) {
        case Lv2Topology::Stereo:   return 1;
        case Lv2Topology::MonoDual: return 2;
        case Lv2Topology::Unsupported: break;
    }
    return 0;
}

// The resolved plan for one plugin: which port index gets connected to what.
struct Lv2PortLayout {
    Lv2Topology topology  = Lv2Topology::Unsupported;
    int         instances = 0;

    std::vector<uint32_t> audioIn;    // required, port-index order
    std::vector<uint32_t> audioOut;   // required, port-index order
    std::vector<uint32_t> controlIn;  // PARAM SLOT ORDER: slot i == [i]
    std::vector<uint32_t> controlOut;
    std::vector<uint32_t> atomIn;     // inert empty-sequence buffer
    std::vector<uint32_t> atomOut;    // scratch, contents discarded

    // Optional audio ports we are not routing: fed a silent buffer / given a
    // scratch buffer to write into, rather than left at NULL. See below.
    std::vector<uint32_t> silencePorts;
    std::vector<uint32_t> discardPorts;

    std::vector<uint32_t> nullPorts;  // optional ports of an unknown class

    // A required port we have no safe buffer for (CV, or anything unrecognised).
    // Forces topology to Unsupported: connecting it to something the wrong size
    // is worse than declining the plugin.
    bool hasUnsupportedPort = false;

    // Index into `controlOut` of the latency-reporting port, or -1. NOT a port
    // index — Lv2Effect stores output control values in a vector parallel to
    // controlOut and reads the latched latency straight out of that slot.
    int latencyOut = -1;
};

// Flatten a port table into the connection plan.
//
// Every output vector follows ascending port index. For controlIn that ordering
// is a PERSISTENCE GUARANTEE, not a convenience: EffectDesc.params[i] is control
// port controlIn[i], so any reordering here would silently repoint every saved
// project's stored knob values onto different ports. It is the order the
// plugin's own RDF declares, which is stable across runs and machines.
inline Lv2PortLayout BuildLv2PortLayout(const std::vector<Lv2PortSpec>& ports) {
    Lv2PortLayout L;
    int latencyByProperty = -1;

    for (const Lv2PortSpec& p : ports) {
        switch (p.role) {
            // An optional audio port is NOT left at NULL, even though the spec
            // allows it. JUCE-generated plugins (a large share of the real
            // world, and the compressor this was found on) memmove every audio
            // port they declare into their internal layout without checking for
            // NULL, and segfault on the first run(). Connecting an unrouted
            // input to silence and an unrouted output to scratch is equally
            // spec-legal, costs two buffers, and is what "nothing is plugged
            // into the sidechain" should mean anyway — which is exactly what
            // the sidechain package will later replace with a real signal.
            case Lv2PortRole::AudioIn:
                (p.optional ? L.silencePorts : L.audioIn).push_back(p.index);
                break;
            case Lv2PortRole::AudioOut:
                (p.optional ? L.discardPorts : L.audioOut).push_back(p.index);
                break;
            // Atom ports get a buffer whether or not they are optional, for the
            // same reason: a buffer is always safe, NULL depends on the plugin
            // being careful.
            case Lv2PortRole::AtomIn:     L.atomIn.push_back(p.index);     break;
            case Lv2PortRole::AtomOut:    L.atomOut.push_back(p.index);    break;
            case Lv2PortRole::ControlIn:  L.controlIn.push_back(p.index);  break;
            case Lv2PortRole::ControlOut:
                L.controlOut.push_back(p.index);
                if (p.latencyDesignation && L.latencyOut < 0)
                    L.latencyOut = (int)L.controlOut.size() - 1;
                else if (p.latencyProperty && latencyByProperty < 0)
                    latencyByProperty = (int)L.controlOut.size() - 1;
                break;
            // A port class we do not recognise. If the plugin can run without
            // it, skip it; if it cannot, we have no buffer of the right size or
            // shape to offer and must decline the plugin entirely. (A CV port
            // lands here: it is audio-rate, so a too-small buffer would be
            // written a whole block past its end.)
            case Lv2PortRole::Unknown:
                if (p.optional) L.nullPorts.push_back(p.index);
                else            L.hasUnsupportedPort = true;
                break;
        }
    }

    // Designation wins outright; the deprecated property is consulted only when
    // no port carried the designation at all.
    if (L.latencyOut < 0) L.latencyOut = latencyByProperty;

    L.topology = L.hasUnsupportedPort
                     ? Lv2Topology::Unsupported
                     : ClassifyLv2Topology((int)L.audioIn.size(),
                                           (int)L.audioOut.size());
    L.instances = Lv2InstanceCount(L.topology);
    return L;
}

// Frames to hand the next lilv_instance_run() given `remaining` still to do.
// Split out so the chunking arithmetic is unit-testable without a plugin.
inline int Lv2ChunkFrames(int remaining) {
    if (remaining <= 0) return 0;
    return remaining < kMaxLv2BlockFrames ? remaining : kMaxLv2BlockFrames;
}

// Coerce an incoming param value into the port's declared domain.
//
// Bounds are applied only where the plugin actually declared one, so a port with
// no lv2:minimum keeps accepting the full float range instead of being pinned to
// a made-up bound. NaN is mapped to a defined value rather than passed through:
// automation or a corrupt project file can produce one, and a NaN in a control
// port propagates into the plugin's output and from there into the mix bus.
// Snap to the nearest of `pts`, considering only points the port's own bounds
// allow. Ties go to the lower value, which only matters for exact midpoints and
// keeps the result deterministic.
//
// `found` reports whether any candidate qualified. Metadata can contradict
// itself -- an enumeration whose scale points lie outside its own
// lv2:minimum/lv2:maximum -- and there the two rules cannot both hold. Snapping
// regardless would hand the plugin a value its range forbids; re-clamping after
// the snap would hand it a value that is not one of its enumerated states. So
// out-of-range points are not candidates at all, and if that leaves none the
// caller falls back to ordinary numeric handling rather than inventing a value.
inline float SnapToScalePoints(float v, const std::vector<float>& pts,
                               float mn, float mx, bool hasMin, bool hasMax,
                               bool* found = nullptr) {
    bool  any  = false;
    float best = v;
    float bestDist = 0.0f;
    for (float p : pts) {
        if (hasMin && p < mn) continue;
        if (hasMax && p > mx) continue;
        const float d = std::fabs(v - p);
        if (!any || d < bestDist) { any = true; bestDist = d; best = p; }
    }
    if (found) *found = any;
    return any ? best : v;
}

inline float ClampLv2Param(float v, float mn, float mx,
                           bool hasMin, bool hasMax, bool isInteger = false,
                           bool isToggled = false,
                           const std::vector<float>* scalePoints = nullptr) {
    if (!(v == v)) v = hasMin ? mn : 0.0f;       // NaN
    if (hasMin && v < mn) v = mn;
    if (hasMax && v > mx) v = mx;

    // An enumeration accepts ONLY its declared values. They can be sparse, so
    // rounding to a whole number is not enough -- snap to the set itself.
    if (scalePoints != nullptr && !scalePoints->empty()) {
        bool snapped = false;
        const float e = SnapToScalePoints(v, *scalePoints, mn, mx,
                                          hasMin, hasMax, &snapped);
        if (snapped) return e;
        // Every declared point sits outside the declared range: the metadata is
        // self-contradictory, so treat the port as an ordinary numeric one
        // rather than forcing a value neither rule permits.
    }

    // A toggle has exactly two states: its endpoints. Anything between them is
    // meaningless to the plugin, and 0/1 is the spec's default pair when the
    // port declares no range of its own.
    if (isToggled) {
        const float lo = hasMin ? mn : 0.0f;
        const float hi = hasMax ? mx : 1.0f;
        return (v - lo) <= (hi - v) ? lo : hi;
    }

    if (isInteger) {
        v = std::floor(v + 0.5f);   // halfway goes up; -0.5 -> 0, -1.5 -> -1
        // The declared bounds may themselves be fractional (a 0.5..3.5 integer
        // port is legal), so the integral range is [ceil(mn), floor(mx)].
        // Re-clamping to the RAW bounds was wrong: it could hand back 3.5 for a
        // 0.5..3.5 port -- a fractional value written into an integer port,
        // which is exactly what this branch exists to prevent.
        const float lo = hasMin ? std::ceil(mn)  : 0.0f;
        const float hi = hasMax ? std::floor(mx) : 0.0f;
        // Metadata can describe a range containing no integer at all (0.2..0.8).
        // Nothing can satisfy both "integral" and "in range" there; stay
        // integral, since a fractional value is the thing the plugin cannot use.
        if (hasMin && hasMax && lo > hi) return v;
        if (hasMin && v < lo) v = lo;
        if (hasMax && v > hi) v = hi;
    }
    return v;
}

// Can we instantiate a plugin that requires exactly `required`? Feature URIs are
// compared as opaque strings; Lv2Host supplies `supported` from the LV2 header
// macros, so the list can never drift from what it actually passes to
// lilv_plugin_instantiate. A plugin requiring anything we do not implement
// (worker:schedule, state:loadDefaultState, ...) must be skipped rather than
// instantiated and hoped for — the spec permits it to fail hard otherwise.
inline bool Lv2FeaturesSatisfied(const std::vector<std::string>& required,
                                 const std::vector<std::string>& supported) {
    for (const std::string& r : required) {
        bool ok = false;
        for (const std::string& s : supported)
            if (r == s) { ok = true; break; }
        if (!ok) return false;
    }
    return true;
}

} // namespace daw
