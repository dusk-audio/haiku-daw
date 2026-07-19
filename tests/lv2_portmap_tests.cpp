// Host-buildable tests for the LV2 port-mapping rules.
//
// These are the decisions that make a plugin hostable or not, and they contain
// no lilv: BuildLv2PortLayout takes a flat port table and returns a connection
// plan. So the whole of it is testable against hand-written port tables, on a
// machine with no LV2 installed at all — which is deliberate, because that is
// exactly the machine where a regression here would otherwise never be run.
//
// The tables below are not invented shapes; the "real-world" cases mirror the
// actual topologies of plugins installed on the development host (a 2-in/2-out
// reverb with a latency port and two atom ports; a compressor whose extra
// sidechain inputs are connectionOptional; instruments and analyzers that must
// be rejected).

#include "../src/plugin/Lv2PortMap.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

namespace {

Lv2PortSpec Port(uint32_t index, Lv2PortRole role, bool optional = false) {
    Lv2PortSpec s;
    s.index    = index;
    s.role     = role;
    s.optional = optional;
    return s;
}

Lv2PortSpec ControlOutPort(uint32_t index, bool designation, bool property) {
    Lv2PortSpec s = Port(index, Lv2PortRole::ControlOut);
    s.latencyDesignation = designation;
    s.latencyProperty    = property;
    return s;
}

} // namespace

int main() {
    // --- Topology classification ----------------------------------------
    {
        CHECK(ClassifyLv2Topology(2, 2) == Lv2Topology::Stereo);
        CHECK(ClassifyLv2Topology(1, 1) == Lv2Topology::MonoDual);

        // Everything else is out of scope for v1 and must be declined rather
        // than approximated. Each of these is a real installed plugin's shape:
        CHECK(ClassifyLv2Topology(0, 2) == Lv2Topology::Unsupported); // instrument
        CHECK(ClassifyLv2Topology(2, 0) == Lv2Topology::Unsupported); // analyzer
        CHECK(ClassifyLv2Topology(1, 2) == Lv2Topology::Unsupported); // mono->stereo amp
        CHECK(ClassifyLv2Topology(4, 2) == Lv2Topology::Unsupported); // mandatory sidechain
        CHECK(ClassifyLv2Topology(0, 0) == Lv2Topology::Unsupported); // headless
        CHECK(ClassifyLv2Topology(3, 3) == Lv2Topology::Unsupported);

        CHECK(Lv2InstanceCount(Lv2Topology::Stereo) == 1);
        CHECK(Lv2InstanceCount(Lv2Topology::MonoDual) == 2);   // one per channel
        CHECK(Lv2InstanceCount(Lv2Topology::Unsupported) == 0);
    }

    // --- A real stereo effect's shape -----------------------------------
    // 2 audio in, 2 audio out, 2 input controls, 1 output control carrying
    // latency, 2 atom ports. This is the exact port census of the reverbs and
    // EQs installed on the dev host.
    {
        std::vector<Lv2PortSpec> ports = {
            Port(0, Lv2PortRole::AudioIn),
            Port(1, Lv2PortRole::AudioIn),
            Port(2, Lv2PortRole::AudioOut),
            Port(3, Lv2PortRole::AudioOut),
            Port(4, Lv2PortRole::ControlIn),
            Port(5, Lv2PortRole::ControlIn),
            ControlOutPort(6, true, true),
            Port(7, Lv2PortRole::AtomIn),
            Port(8, Lv2PortRole::AtomOut),
        };
        Lv2PortLayout L = BuildLv2PortLayout(ports);

        CHECK(L.topology == Lv2Topology::Stereo);
        CHECK(L.instances == 1);
        CHECK(L.audioIn.size() == 2 && L.audioIn[0] == 0 && L.audioIn[1] == 1);
        CHECK(L.audioOut.size() == 2 && L.audioOut[0] == 2 && L.audioOut[1] == 3);
        CHECK(L.controlIn.size() == 2 && L.controlIn[0] == 4 && L.controlIn[1] == 5);
        CHECK(L.controlOut.size() == 1 && L.controlOut[0] == 6);
        CHECK(L.atomIn.size() == 1 && L.atomIn[0] == 7);
        CHECK(L.atomOut.size() == 1 && L.atomOut[0] == 8);
        CHECK(L.nullPorts.empty());
        CHECK(!L.hasUnsupportedPort);
        CHECK(L.latencyOut == 0);        // index into controlOut, not port index
    }

    // --- Optional audio ports do not count toward topology ---------------
    // A stereo compressor with an OPTIONAL stereo sidechain (Multi-Comp: 4 audio
    // in, 2 out) is hostable as a plain stereo insert. Counting all four inputs
    // would reject a perfectly usable compressor.
    //
    // The unrouted pair is connected to SILENCE, not left at NULL. The spec
    // permits NULL, but JUCE-generated plugins memmove every audio port they
    // declare without checking, and segfault on the first run() — which is
    // precisely how this was found. Silence is equally legal and survivable.
    {
        std::vector<Lv2PortSpec> ports = {
            Port(0, Lv2PortRole::AudioIn),
            Port(1, Lv2PortRole::AudioIn),
            Port(2, Lv2PortRole::AudioIn,  /*optional=*/true),   // sidechain L
            Port(3, Lv2PortRole::AudioIn,  /*optional=*/true),   // sidechain R
            Port(4, Lv2PortRole::AudioOut),
            Port(5, Lv2PortRole::AudioOut),
            Port(6, Lv2PortRole::AudioOut, /*optional=*/true),   // aux send
        };
        Lv2PortLayout L = BuildLv2PortLayout(ports);

        CHECK(L.topology == Lv2Topology::Stereo);
        CHECK(L.audioIn.size() == 2);              // only the required pair
        CHECK(L.audioOut.size() == 2);
        CHECK(L.silencePorts.size() == 2);
        CHECK(L.silencePorts[0] == 2 && L.silencePorts[1] == 3);
        CHECK(L.discardPorts.size() == 1 && L.discardPorts[0] == 6);
        CHECK(L.nullPorts.empty());                // no audio port is ever NULL
    }

    // --- A required port we cannot size is fatal; an optional one is not --
    // A CV port wants one float per FRAME. There is no buffer we can hand it
    // that is both correct and inert, so hosting the plugin anyway would invite
    // a write past the end of whatever we did hand it. Declining is the safe
    // answer even though the audio topology itself looks fine.
    {
        std::vector<Lv2PortSpec> ports = {
            Port(0, Lv2PortRole::AudioIn),  Port(1, Lv2PortRole::AudioIn),
            Port(2, Lv2PortRole::AudioOut), Port(3, Lv2PortRole::AudioOut),
            Port(4, Lv2PortRole::Unknown),                       // required CV
        };
        Lv2PortLayout L = BuildLv2PortLayout(ports);
        CHECK(L.hasUnsupportedPort);
        CHECK(L.topology == Lv2Topology::Unsupported);
        CHECK(L.instances == 0);
        CHECK(L.audioIn.size() == 2);   // the audio census itself was fine...
        CHECK(L.audioOut.size() == 2);  // ...the unknown port is what rejected it

        // The same plugin with that port marked optional IS hostable.
        ports[4].optional = true;
        Lv2PortLayout M = BuildLv2PortLayout(ports);
        CHECK(!M.hasUnsupportedPort);
        CHECK(M.topology == Lv2Topology::Stereo);
        CHECK(M.nullPorts.size() == 1 && M.nullPorts[0] == 4);
    }

    // --- Latency: designation beats the deprecated property ---------------
    // lv2:designation lv2:latency is the current mechanism; lv2:reportsLatency
    // is deprecated. When a plugin has BOTH on DIFFERENT ports, the designation
    // must win. Put the property first in port order so a naive
    // first-match-wins implementation picks the wrong one and fails here.
    {
        std::vector<Lv2PortSpec> ports = {
            Port(0, Lv2PortRole::AudioIn),  Port(1, Lv2PortRole::AudioIn),
            Port(2, Lv2PortRole::AudioOut), Port(3, Lv2PortRole::AudioOut),
            ControlOutPort(4, /*designation=*/false, /*property=*/true),
            ControlOutPort(5, /*designation=*/false, /*property=*/false),
            ControlOutPort(6, /*designation=*/true,  /*property=*/false),
        };
        Lv2PortLayout L = BuildLv2PortLayout(ports);
        CHECK(L.controlOut.size() == 3);
        CHECK(L.latencyOut == 2);        // controlOut[2] == port 6, the designated one
    }

    // Property-only is still honoured, for plugins predating the designation.
    {
        std::vector<Lv2PortSpec> ports = {
            Port(0, Lv2PortRole::AudioIn),  Port(1, Lv2PortRole::AudioIn),
            Port(2, Lv2PortRole::AudioOut), Port(3, Lv2PortRole::AudioOut),
            ControlOutPort(4, false, false),
            ControlOutPort(5, false, true),
        };
        Lv2PortLayout L = BuildLv2PortLayout(ports);
        CHECK(L.latencyOut == 1);
    }

    // No latency port at all reports -1, which Lv2Effect turns into 0 frames.
    {
        std::vector<Lv2PortSpec> ports = {
            Port(0, Lv2PortRole::AudioIn),  Port(1, Lv2PortRole::AudioIn),
            Port(2, Lv2PortRole::AudioOut), Port(3, Lv2PortRole::AudioOut),
            ControlOutPort(4, false, false),
        };
        CHECK(BuildLv2PortLayout(ports).latencyOut == -1);
        CHECK(BuildLv2PortLayout({}).latencyOut == -1);
    }

    // latencyOut indexes controlOut, NOT the port table. With sparse, high port
    // indices the two numbers cannot coincide, so a confusion between them shows
    // up here instead of as a silently wrong latency at runtime.
    {
        std::vector<Lv2PortSpec> ports = {
            Port(10, Lv2PortRole::AudioIn),  Port(11, Lv2PortRole::AudioIn),
            Port(12, Lv2PortRole::AudioOut), Port(13, Lv2PortRole::AudioOut),
            ControlOutPort(40, false, false),
            ControlOutPort(41, true,  false),
        };
        Lv2PortLayout L = BuildLv2PortLayout(ports);
        CHECK(L.latencyOut == 1);            // an index, not the port number 41
        CHECK(L.controlOut[(size_t)L.latencyOut] == 41);
    }

    // --- Param slot ordering is the persistence contract ------------------
    // EffectDesc.params[i] is control port controlIn[i]. Slot order must follow
    // the plugin's own declared port order and must not be perturbed by audio,
    // atom or output-control ports interleaved among them — otherwise every
    // saved project's knob values would land on different ports.
    {
        std::vector<Lv2PortSpec> ports = {
            Port(0, Lv2PortRole::ControlIn),        // slot 0
            Port(1, Lv2PortRole::AudioIn),
            Port(2, Lv2PortRole::ControlIn),        // slot 1
            Port(3, Lv2PortRole::AtomIn),
            Port(4, Lv2PortRole::AudioIn),
            ControlOutPort(5, true, false),
            Port(6, Lv2PortRole::ControlIn),        // slot 2
            Port(7, Lv2PortRole::AudioOut),
            Port(8, Lv2PortRole::AudioOut),
            Port(9, Lv2PortRole::ControlIn),        // slot 3
        };
        Lv2PortLayout L = BuildLv2PortLayout(ports);
        CHECK(L.topology == Lv2Topology::Stereo);
        CHECK(L.controlIn.size() == 4);
        CHECK(L.controlIn[0] == 0);
        CHECK(L.controlIn[1] == 2);
        CHECK(L.controlIn[2] == 6);
        CHECK(L.controlIn[3] == 9);
    }

    // A mono plugin: one instance per channel, sharing one control array.
    {
        std::vector<Lv2PortSpec> ports = {
            Port(0, Lv2PortRole::AudioIn),
            Port(1, Lv2PortRole::AudioOut),
            Port(2, Lv2PortRole::ControlIn),
        };
        Lv2PortLayout L = BuildLv2PortLayout(ports);
        CHECK(L.topology == Lv2Topology::MonoDual);
        CHECK(L.instances == 2);
        CHECK(L.audioIn.size() == 1 && L.audioOut.size() == 1);
    }

    // --- Block chunking ---------------------------------------------------
    // Nothing bounds Engine::SetBufferFrames from above, so Process must survive
    // any block size without allocating. These are the arithmetic guarantees the
    // chunk loop rests on.
    {
        CHECK(Lv2ChunkFrames(0) == 0);
        CHECK(Lv2ChunkFrames(-1) == 0);
        CHECK(Lv2ChunkFrames(1) == 1);
        CHECK(Lv2ChunkFrames(512) == 512);
        CHECK(Lv2ChunkFrames(kMaxLv2BlockFrames - 1) == kMaxLv2BlockFrames - 1);
        CHECK(Lv2ChunkFrames(kMaxLv2BlockFrames) == kMaxLv2BlockFrames);
        CHECK(Lv2ChunkFrames(kMaxLv2BlockFrames + 1) == kMaxLv2BlockFrames);
        CHECK(Lv2ChunkFrames(1 << 20) == kMaxLv2BlockFrames);

        // The constant must cover the Exporter's offline block (8192 frames), or
        // every offline bounce would chunk needlessly.
        CHECK(kMaxLv2BlockFrames >= 8192);

        // Drive the actual loop shape for a block far larger than the maximum:
        // it must terminate, cover the input exactly once, and never hand a
        // single run() more than the maximum it promised the plugin.
        for (int total : { 1, 4096, kMaxLv2BlockFrames, kMaxLv2BlockFrames + 1,
                           3 * kMaxLv2BlockFrames, 100000 }) {
            int done = 0, iterations = 0;
            while (done < total) {
                const int n = Lv2ChunkFrames(total - done);
                CHECK(n > 0);                          // or the loop hangs
                CHECK(n <= kMaxLv2BlockFrames);        // the promise to the plugin
                done += n;
                if (++iterations > 1000) break;        // hang guard
            }
            CHECK(done == total);                      // exactly once, no overrun
        }
    }

    // --- Param clamping ---------------------------------------------------
    {
        // Declared bounds are enforced.
        CHECK(ClampLv2Param(5.0f, 0.0f, 1.0f, true, true) == 1.0f);
        CHECK(ClampLv2Param(-5.0f, 0.0f, 1.0f, true, true) == 0.0f);
        CHECK(ClampLv2Param(0.5f, 0.0f, 1.0f, true, true) == 0.5f);

        // An UNDECLARED bound must not be invented: a port with no lv2:minimum
        // keeps accepting the whole float range instead of being pinned to a
        // fabricated one.
        CHECK(ClampLv2Param(-500.0f, 0.0f, 1.0f, false, true) == -500.0f);
        CHECK(ClampLv2Param(500.0f, 0.0f, 1.0f, true, false) == 500.0f);
        CHECK(ClampLv2Param(-500.0f, 0.0f, 1.0f, false, false) == -500.0f);

        // NaN never reaches a control port: automation or a corrupt project can
        // produce one, and a NaN in a control port propagates into the plugin's
        // output and from there into the mix bus.
        const float nan = std::numeric_limits<float>::quiet_NaN();
        CHECK(ClampLv2Param(nan, -3.0f, 1.0f, true, true) == -3.0f);
        CHECK(ClampLv2Param(nan, -3.0f, 1.0f, false, false) == 0.0f);
        CHECK(!std::isnan(ClampLv2Param(nan, 0.0f, 1.0f, true, true)));

        // An integer/toggled/enumeration port snaps to whole numbers. Without
        // this a continuous slider writes values like 0.03 into a two-state
        // control, which means nothing to the plugin.
        CHECK(ClampLv2Param(0.03f, 0.0f, 1.0f, true, true, true) == 0.0f);
        CHECK(ClampLv2Param(0.5f,  0.0f, 1.0f, true, true, true) == 1.0f);  // .5 rounds up
        CHECK(ClampLv2Param(0.49f, 0.0f, 1.0f, true, true, true) == 0.0f);
        CHECK(ClampLv2Param(1.7f,  0.0f, 3.0f, true, true, true) == 2.0f);
        CHECK(ClampLv2Param(-0.4f, -3.0f, 3.0f, true, true, true) == 0.0f);
        CHECK(ClampLv2Param(-1.6f, -3.0f, 3.0f, true, true, true) == -2.0f);
        // Out of range first, then snapped.
        CHECK(ClampLv2Param(99.0f, 0.0f, 2.0f, true, true, true) == 2.0f);
        // NaN into an integer port still lands on a defined whole number.
        CHECK(ClampLv2Param(nan, 0.0f, 1.0f, true, true, true) == 0.0f);
        // Rounding must not escape a FRACTIONAL bound: 0.5..3.5 is a legal
        // integer port, and 0.6 rounds to 1 which is inside it, but 0.51 must
        // not round to 1 and then be reported below the minimum.
        CHECK(ClampLv2Param(0.51f, 0.5f, 3.5f, true, true, true) == 1.0f);
        CHECK(ClampLv2Param(3.9f,  0.5f, 3.5f, true, true, true) == 3.5f);
        // The same values are left alone when the port is continuous.
        CHECK(ClampLv2Param(0.03f, 0.0f, 1.0f, true, true, false) == 0.03f);

        // Infinities are ordinary out-of-range values where a bound exists.
        const float inf = std::numeric_limits<float>::infinity();
        CHECK(ClampLv2Param(inf, 0.0f, 1.0f, true, true) == 1.0f);
        CHECK(ClampLv2Param(-inf, 0.0f, 1.0f, true, true) == 0.0f);
    }

    // --- Required-feature filtering ---------------------------------------
    {
        const std::vector<std::string> supported = { "a", "b", "c" };
        CHECK(Lv2FeaturesSatisfied({}, supported));            // requires nothing
        CHECK(Lv2FeaturesSatisfied({ "a" }, supported));
        CHECK(Lv2FeaturesSatisfied({ "a", "c" }, supported));
        CHECK(!Lv2FeaturesSatisfied({ "d" }, supported));
        CHECK(!Lv2FeaturesSatisfied({ "a", "d" }, supported));  // one miss is enough
        CHECK(!Lv2FeaturesSatisfied({ "a" }, {}));              // host supports none
    }

    std::printf("lv2_portmap_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
