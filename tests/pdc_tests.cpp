// Host-buildable tests for the PDC latency solver (model/Pdc.h): per-node
// input/output latency, the master alignment target, and the per-edge
// compensating delay that keeps sibling signal paths time-aligned. Uses the
// same {id, output}+send edge model the Engine and Exporter feed it.

#include "../src/model/Pdc.h"

#include <cstdio>
#include <utility>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

using Edge  = std::pair<TrackId, TrackId>;
using Edges = std::vector<Edge>;

int main() {
    // Empty graph: solves, no latency.
    {
        PdcGraph g;
        CHECK(ComputePdc({}, {}, g));
        CHECK(g.masterInLat == 0);
        CHECK(g.totalLatency == 0);
    }

    // Two dry tracks -> master: no latency, no compensation anywhere.
    {
        std::vector<PdcNode> n = {{1, 0}, {2, 0}};
        Edges e = {{1, kRoutingMaster}, {2, kRoutingMaster}};
        PdcGraph g;
        CHECK(ComputePdc(n, e, g));
        CHECK(g.masterInLat == 0);
        CHECK(g.EdgeDelay(1, kRoutingMaster) == 0);
        CHECK(g.EdgeDelay(2, kRoutingMaster) == 0);
    }

    // Single latent source -> master: the whole render lags by its latency, but
    // being the only feeder it needs no compensating delay itself.
    {
        std::vector<PdcNode> n = {{1, 100}};
        Edges e = {{1, kRoutingMaster}};
        PdcGraph g;
        CHECK(ComputePdc(n, e, g));
        CHECK(g.OutLat(1) == 100);
        CHECK(g.InLat(1) == 0);
        CHECK(g.masterInLat == 100);
        CHECK(g.totalLatency == 100);
        CHECK(g.EdgeDelay(1, kRoutingMaster) == 0);
    }

    // Sibling alignment: a latent track and a dry track into master. The dry one
    // is delayed to match the latent one; the latent one passes through.
    {
        std::vector<PdcNode> n = {{1, 100}, {2, 0}};   // 1 latent, 2 dry
        Edges e = {{1, kRoutingMaster}, {2, kRoutingMaster}};
        PdcGraph g;
        CHECK(ComputePdc(n, e, g));
        CHECK(g.masterInLat == 100);
        CHECK(g.EdgeDelay(1, kRoutingMaster) == 0);     // latent: no extra
        CHECK(g.EdgeDelay(2, kRoutingMaster) == 100);   // dry: delayed to match
    }

    // Bus chain: A(latent) + C(dry) -> Bus(dry) -> master, plus D(dry) direct to
    // master. The dry paths (C into the bus, D into master) get delayed.
    {
        const TrackId A = 1, C = 2, BUS = 3, D = 4;
        std::vector<PdcNode> n = {{A, 100}, {C, 0}, {BUS, 0}, {D, 0}};
        Edges e = {{A, BUS}, {C, BUS}, {BUS, kRoutingMaster},
                   {D, kRoutingMaster}};
        PdcGraph g;
        CHECK(ComputePdc(n, e, g));
        CHECK(g.OutLat(A) == 100);
        CHECK(g.InLat(BUS) == 100);
        CHECK(g.OutLat(BUS) == 100);
        CHECK(g.masterInLat == 100);
        CHECK(g.EdgeDelay(A, BUS) == 0);
        CHECK(g.EdgeDelay(C, BUS) == 100);              // dry sibling into bus
        CHECK(g.EdgeDelay(BUS, kRoutingMaster) == 0);
        CHECK(g.EdgeDelay(D, kRoutingMaster) == 100);   // dry sibling into master
    }

    // Latent bus: the latency is on the bus itself, so its own inputs need no
    // compensation, but a track feeding master directly must wait for the bus.
    {
        const TrackId A = 1, BUS = 2, B = 3;
        std::vector<PdcNode> n = {{A, 0}, {BUS, 50}, {B, 0}};
        Edges e = {{A, BUS}, {BUS, kRoutingMaster}, {B, kRoutingMaster}};
        PdcGraph g;
        CHECK(ComputePdc(n, e, g));
        CHECK(g.OutLat(BUS) == 50);
        CHECK(g.masterInLat == 50);
        CHECK(g.EdgeDelay(A, BUS) == 0);
        CHECK(g.EdgeDelay(BUS, kRoutingMaster) == 0);
        CHECK(g.EdgeDelay(B, kRoutingMaster) == 50);
    }

    // Aux send to a latent reverb bus: a source feeds a dry main bus (output)
    // AND a latent reverb bus (send). The dry main-bus path to master gets
    // delayed to align with the reverb return.
    {
        const TrackId A = 1, BUS = 2, REV = 3;
        std::vector<PdcNode> n = {{A, 0}, {BUS, 0}, {REV, 200}};
        Edges e = {{A, BUS}, {A, REV},                 // output + send
                   {BUS, kRoutingMaster}, {REV, kRoutingMaster}};
        PdcGraph g;
        CHECK(ComputePdc(n, e, g));
        CHECK(g.OutLat(REV) == 200);
        CHECK(g.masterInLat == 200);
        CHECK(g.EdgeDelay(A, BUS) == 0);
        CHECK(g.EdgeDelay(A, REV) == 0);
        CHECK(g.EdgeDelay(BUS, kRoutingMaster) == 200); // dry main path delayed
        CHECK(g.EdgeDelay(REV, kRoutingMaster) == 0);   // reverb return passes
    }

    // Two latent branches into one bus: each is delayed to the slower branch.
    {
        const TrackId A = 1, B = 2, BUS = 3;
        std::vector<PdcNode> n = {{A, 100}, {B, 40}, {BUS, 0}};
        Edges e = {{A, BUS}, {B, BUS}, {BUS, kRoutingMaster}};
        PdcGraph g;
        CHECK(ComputePdc(n, e, g));
        CHECK(g.InLat(BUS) == 100);
        CHECK(g.EdgeDelay(A, BUS) == 0);
        CHECK(g.EdgeDelay(B, BUS) == 60);               // 100 - 40
        CHECK(g.masterInLat == 100);
    }

    // Cascading latency sums along a chain: source fx + bus fx add up.
    {
        const TrackId A = 1, BUS = 2;
        std::vector<PdcNode> n = {{A, 30}, {BUS, 20}};
        Edges e = {{A, BUS}, {BUS, kRoutingMaster}};
        PdcGraph g;
        CHECK(ComputePdc(n, e, g));
        CHECK(g.OutLat(A) == 30);
        CHECK(g.InLat(BUS) == 30);
        CHECK(g.OutLat(BUS) == 50);                     // 30 + 20
        CHECK(g.totalLatency == 50);
    }

    // Bad graphs are rejected (caller falls back to no compensation).
    {
        PdcGraph g;
        // Cycle.
        CHECK(!ComputePdc({{1, 0}, {2, 0}}, {{1, 2}, {2, 1}}, g));
        // Duplicate id.
        CHECK(!ComputePdc({{1, 0}, {1, 0}}, {{1, kRoutingMaster}}, g));
        // Self-edge on a real node.
        CHECK(!ComputePdc({{1, 0}}, {{1, 1}}, g));
    }

    // Negative reported latency is clamped to zero (defensive).
    {
        std::vector<PdcNode> n = {{1, -5}};
        Edges e = {{1, kRoutingMaster}};
        PdcGraph g;
        CHECK(ComputePdc(n, e, g));
        CHECK(g.OutLat(1) == 0);
        CHECK(g.masterInLat == 0);
    }

    std::printf("pdc_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
