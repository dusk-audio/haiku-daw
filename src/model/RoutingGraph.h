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
#include <utility>
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

// Generalized topo sort over an arbitrary edge list. `nodeIds` lists every real
// node; `edges` are directed dependencies (from -> to) meaning `to` depends on
// `from`, so `from` must appear BEFORE `to` in the result. An edge whose `to`
// is kRoutingMaster (or any id not in `nodeIds`) is treated as feeding the
// implicit sink and imposes no ordering constraint on a real node. This lets a
// node have MANY outgoing edges (its output plus every aux send), which the
// single-output ResolveRoutingOrder cannot express.
//
// Returns false (and clears `outOrder`) on a duplicate id, a self-edge
// (from == to on a real node), or a cycle. Edges into the master sink and
// edges from unknown nodes are ignored.
inline bool ResolveOrderWithEdges(
        const std::vector<TrackId>& nodeIds,
        const std::vector<std::pair<TrackId, TrackId>>& edges,
        std::vector<TrackId>& outOrder) {
    outOrder.clear();

    std::unordered_map<TrackId, size_t> indexOf;
    indexOf.reserve(nodeIds.size() * 2);
    for (size_t i = 0; i < nodeIds.size(); ++i)
        if (!indexOf.emplace(nodeIds[i], i).second) return false; // duplicate

    std::vector<std::vector<size_t>> adj(nodeIds.size());
    std::vector<size_t> indegree(nodeIds.size(), 0);
    for (const auto& e : edges) {
        auto from = indexOf.find(e.first);
        if (from == indexOf.end()) continue;         // edge from unknown node
        auto to = indexOf.find(e.second);
        if (to == indexOf.end()) continue;           // feeds master / unknown
        if (from->second == to->second) return false;// self-edge
        adj[from->second].push_back(to->second);
        ++indegree[to->second];
    }

    std::vector<TrackId> ready;
    ready.reserve(nodeIds.size());
    for (size_t i = 0; i < nodeIds.size(); ++i)
        if (indegree[i] == 0) ready.push_back(nodeIds[i]);

    outOrder.reserve(nodeIds.size());
    while (!ready.empty()) {
        const TrackId id = ready.back();
        ready.pop_back();
        outOrder.push_back(id);
        for (size_t oi : adj[indexOf[id]])
            if (--indegree[oi] == 0) ready.push_back(nodeIds[oi]);
    }

    if (outOrder.size() != nodeIds.size()) {         // leftovers = cycle
        outOrder.clear();
        return false;
    }
    return true;
}

} // namespace daw
