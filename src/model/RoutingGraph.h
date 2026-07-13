// RoutingGraph — dependency ordering for the routing graph.
//
// Each track/bus routes its output into another bus, or into the master sink.
// To render, a node must be processed AFTER every node that feeds it and
// BEFORE the node it feeds. This computes such an order (a topological sort of
// the edges node -> output) and rejects anything that isn't a valid DAG:
// cycles, self-loops, dangling outputs, and duplicate ids.
//
// Kit-free, header-only, so it builds and unit-tests anywhere.
#pragma once

#include "types.h"

#include <unordered_map>
#include <vector>

namespace daw {

// The implicit final sink. A node whose `output == kRoutingMaster` feeds the
// master directly; the master is not itself a node and never appears in the
// resolved order.
constexpr TrackId kRoutingMaster = kInvalidTrackId; // 0 = master sink

// A node with `id` routes its output into `output`. `output == kRoutingMaster`
// means it feeds the master sink.
struct RouteNode {
    TrackId id;
    TrackId output;
};

// Fills `outOrder` with every node id in an order where each node appears
// BEFORE the node it routes to, so processing in this order means a node's
// inputs are all finished before it runs. The master sink is NOT included.
//
// Returns false (and leaves `outOrder` empty) on any of:
//   - a cycle,
//   - a self-loop (output == id),
//   - a dangling output (names a non-existent, non-master node),
//   - a duplicate id.
inline bool ResolveRoutingOrder(const std::vector<RouteNode>& nodes,
                                std::vector<TrackId>& outOrder) {
    outOrder.clear();

    // Index every node by id and count incoming edges. Reject duplicates and
    // self-loops up front.
    std::unordered_map<TrackId, size_t> indexOf;
    indexOf.reserve(nodes.size() * 2);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const RouteNode& n = nodes[i];
        if (n.output == n.id) return false;              // self-loop
        if (!indexOf.emplace(n.id, i).second) return false; // duplicate id
    }

    // Validate outputs and build indegrees. An edge node -> output means the
    // output depends on the node, so the output's indegree increases.
    std::vector<size_t> indegree(nodes.size(), 0);
    for (const RouteNode& n : nodes) {
        if (n.output == kRoutingMaster) continue;        // feeds master sink
        auto it = indexOf.find(n.output);
        if (it == indexOf.end()) return false;           // dangling output
        ++indegree[it->second];
    }

    // Kahn's algorithm. Seed with nodes that nothing feeds into.
    std::vector<TrackId> ready;
    ready.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        if (indegree[i] == 0) ready.push_back(nodes[i].id);

    outOrder.reserve(nodes.size());
    while (!ready.empty()) {
        const TrackId id = ready.back();
        ready.pop_back();
        outOrder.push_back(id);

        const RouteNode& n = nodes[indexOf[id]];
        if (n.output == kRoutingMaster) continue;
        const size_t oi = indexOf[n.output];
        if (--indegree[oi] == 0) ready.push_back(nodes[oi].id);
    }

    // If not every node was emitted, the leftovers form a cycle.
    if (outOrder.size() != nodes.size()) {
        outOrder.clear();
        return false;
    }
    return true;
}

} // namespace daw
