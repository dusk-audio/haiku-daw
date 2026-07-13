// Host-buildable tests for the routing-graph resolver: topological ordering of
// node -> output edges, with rejection of cycles, self-loops, dangling
// outputs, and duplicate ids.

#include "../src/model/RoutingGraph.h"

#include <algorithm>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// Index of `id` in `order`, or -1 if absent.
static int PosOf(const std::vector<TrackId>& order, TrackId id) {
    auto it = std::find(order.begin(), order.end(), id);
    return it == order.end() ? -1 : (int)(it - order.begin());
}

static bool Contains(const std::vector<TrackId>& order, TrackId id) {
    return PosOf(order, id) >= 0;
}

int main() {
    // Empty -> ok, empty order.
    {
        std::vector<RouteNode> nodes;
        std::vector<TrackId> order;
        CHECK(ResolveRoutingOrder(nodes, order));
        CHECK(order.empty());
    }

    // All nodes to master -> order contains all; sibling order is free, but
    // each node must precede its output when the output is a real node (here
    // there are none, so just membership + size).
    {
        std::vector<RouteNode> nodes = {
            {1, kRoutingMaster}, {2, kRoutingMaster}, {3, kRoutingMaster}};
        std::vector<TrackId> order;
        CHECK(ResolveRoutingOrder(nodes, order));
        CHECK(order.size() == 3);
        CHECK(Contains(order, 1) && Contains(order, 2) && Contains(order, 3));
    }

    // Linear chain A->B->C->master: A before B before C.
    {
        std::vector<RouteNode> nodes = {
            {10, 20}, {20, 30}, {30, kRoutingMaster}};
        std::vector<TrackId> order;
        CHECK(ResolveRoutingOrder(nodes, order));
        CHECK(order.size() == 3);
        CHECK(PosOf(order, 10) < PosOf(order, 20));
        CHECK(PosOf(order, 20) < PosOf(order, 30));
    }

    // Diamond: A->bus, B->bus, bus->master. A and B before bus.
    {
        const TrackId A = 1, B = 2, bus = 9;
        std::vector<RouteNode> nodes = {
            {A, bus}, {B, bus}, {bus, kRoutingMaster}};
        std::vector<TrackId> order;
        CHECK(ResolveRoutingOrder(nodes, order));
        CHECK(order.size() == 3);
        CHECK(PosOf(order, A) < PosOf(order, bus));
        CHECK(PosOf(order, B) < PosOf(order, bus));
    }

    // Cycle A->B->A -> false, order left empty.
    {
        std::vector<RouteNode> nodes = {{1, 2}, {2, 1}};
        std::vector<TrackId> order;
        CHECK(!ResolveRoutingOrder(nodes, order));
        CHECK(order.empty());
    }

    // Self-loop -> false.
    {
        std::vector<RouteNode> nodes = {{5, 5}};
        std::vector<TrackId> order;
        CHECK(!ResolveRoutingOrder(nodes, order));
        CHECK(order.empty());
    }

    // Dangling output (points to a missing id) -> false.
    {
        std::vector<RouteNode> nodes = {{1, 2}, {2, 99}};
        std::vector<TrackId> order;
        CHECK(!ResolveRoutingOrder(nodes, order));
        CHECK(order.empty());
    }

    // Duplicate id -> false.
    {
        std::vector<RouteNode> nodes = {
            {7, kRoutingMaster}, {7, kRoutingMaster}};
        std::vector<TrackId> order;
        CHECK(!ResolveRoutingOrder(nodes, order));
        CHECK(order.empty());
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
