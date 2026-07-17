// Pdc — plugin-delay-compensation latency solver for the routing graph.
//
// A latent effect (a look-ahead limiter, a linear-phase FIR, an FFT-block
// processor) delays its track's output by IEffect::LatencySamples() samples. If
// one track in a mix is delayed and a sibling isn't, they smear. Plugin delay
// compensation fixes this by delaying every signal path so all paths arriving
// at a common node (a bus, or the master sink) are time-aligned to the slowest.
//
// This computes, for the routing DAG, each node's input/output latency and the
// master alignment target, from which the caller derives a per-edge compensating
// delay (EdgeDelay). It reuses the same edge model + topo sort as RoutingGraph
// (a node's output edge plus its aux-send edges), so the Engine and the offline
// Exporter feed it the identical graph they already build.
//
// Kit-free, header-only (STL + RoutingGraph only), so it host-unit-tests
// anywhere. Latencies are in samples at the render/output rate.
#pragma once

#include "RoutingGraph.h"
#include "types.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace daw {

// Per-node latency (samples).
//   inLat  — the alignment target for signals feeding this node: the max output
//            latency over every node that routes into it (0 for a pure source).
//            All of the node's inputs are delayed up to this so they line up.
//   outLat — what this node's own output lags the timeline by: inLat plus the
//            latency its own effect chain adds.
struct PdcLatency {
    int inLat  = 0;
    int outLat = 0;
};

// One node in the PDC graph: its id and the latency its OWN fx chain adds (the
// sum of IEffect::LatencySamples() over the chain; 0 if none).
struct PdcNode {
    TrackId id;
    int     fxLatency;
};

// The solved latency graph. `EdgeDelay(from, to)` gives the samples of delay to
// insert on the `from`->`to` edge so `to` receives `from` time-aligned with its
// other inputs. `to == kRoutingMaster` targets the master sink.
struct PdcGraph {
    std::unordered_map<TrackId, PdcLatency> node;   // per node id
    int masterInLat  = 0;   // alignment target for edges feeding the master sink
    int totalLatency = 0;   // == masterInLat: leading samples the render lags by

    int InLat(TrackId id) const {
        auto it = node.find(id);
        return it == node.end() ? 0 : it->second.inLat;
    }
    int OutLat(TrackId id) const {
        auto it = node.find(id);
        return it == node.end() ? 0 : it->second.outLat;
    }

    // Compensating delay (samples, always >= 0) to apply to `from`'s signal when
    // it is summed into `to`. `to == kRoutingMaster` = the master sink. The
    // target latency is the max over all of `to`'s feeders, so this is never
    // negative.
    int EdgeDelay(TrackId from, TrackId to) const {
        const int target = (to == kRoutingMaster) ? masterInLat : InLat(to);
        const int d = target - OutLat(from);
        return d > 0 ? d : 0;
    }
};

// Solve the PDC latency graph. `edges` are the routing edges (from -> to): each
// node's output edge {id, output} plus one edge per aux send {id, sendDest}. An
// edge whose `to` is kRoutingMaster (== kInvalidTrackId) or any id not in
// `nodes` feeds the master sink. Returns false (and clears `out`) on a cycle,
// duplicate id, or self-edge — the same graph rejections as ResolveOrderWithEdges
// — so the caller falls back to no compensation, exactly as it already falls
// back to a flat routing order.
inline bool ComputePdc(const std::vector<PdcNode>& nodes,
                       const std::vector<std::pair<TrackId, TrackId>>& edges,
                       PdcGraph& out) {
    out.node.clear();
    out.masterInLat  = 0;
    out.totalLatency = 0;

    // Index node ids + their own fx latency; seed every node at zero.
    std::vector<TrackId> ids;
    ids.reserve(nodes.size());
    std::unordered_map<TrackId, int> fxLat;
    fxLat.reserve(nodes.size() * 2);
    for (const PdcNode& n : nodes) {
        ids.push_back(n.id);
        fxLat[n.id] = n.fxLatency < 0 ? 0 : n.fxLatency;
        out.node[n.id] = PdcLatency{0, 0};
    }

    // Topo order (a node's feeders all precede it), reusing the routing solver so
    // a bad graph is rejected identically. This also rejects duplicate ids.
    std::vector<TrackId> order;
    if (!ResolveOrderWithEdges(ids, edges, order))
        return false;

    // Outgoing adjacency for real source nodes only (edges from unknown nodes
    // impose nothing). Kept in edge order; duplicates are harmless (max relax).
    std::unordered_map<TrackId, std::vector<TrackId>> adj;
    adj.reserve(ids.size() * 2);
    for (const auto& e : edges) {
        if (!fxLat.count(e.first)) continue;   // edge from a non-node
        if (e.first == e.second)   continue;   // self-edge (already rejected)
        adj[e.first].push_back(e.second);
    }

    // Relax in topo order: a node's inLat is final once every feeder is done, so
    // by Kahn order it is final when we reach the node. Finalize its outLat, then
    // push that into each destination's alignment target (a real dest's inLat, or
    // the master target for an edge to the sink).
    for (TrackId id : order) {
        const int outLat = out.node[id].inLat + fxLat[id];
        out.node[id].outLat = outLat;
        auto it = adj.find(id);
        if (it == adj.end()) continue;
        for (TrackId to : it->second) {
            if (to == kRoutingMaster || !fxLat.count(to)) {
                if (outLat > out.masterInLat) out.masterInLat = outLat;
            } else {
                int& din = out.node[to].inLat;
                if (outLat > din) din = outLat;
            }
        }
    }
    out.totalLatency = out.masterInLat;
    return true;
}

} // namespace daw
