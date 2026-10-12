// StreamPool — a fixed set of threads that keep a graph's clip streams filled.
//
// One thread per clip does not scale: MEASURED 2026-10-12 on the 2-vCPU VM with
// 329 clips, starting them cost 166 ms of the build in thread creation alone,
// 329 concurrent fillers then starved the builder (680 ms of contention), and
// every one of those threads kept polling its ring every 2 ms for as long as the
// graph existed. A pool of K workers serves the same streams with K threads:
// the build's thread cost drops to K starts, the fills cannot outnumber the
// CPUs, and an idle graph costs K polls instead of one per clip.
//
// Assignment is STATIC and round-robin, which is what makes this lock-free: each
// stream belongs to exactly one worker for its whole life, so nothing shared is
// ever touched by two threads. `T` must provide:
//
//   bool NeedsFilling() const;   // would it take more audio right now?
//   bool FillOnce();             // move a bounded amount of audio into it (may
//                                // block on file I/O — this is not the RT path);
//                                // false when it could not, which is what keeps
//                                // a finished stream from spinning a worker
//
// A graph builds a pool over the streams it just opened, ordered so the clips
// the transport needs first land in the earliest slots of each worker's list
// (StreamOrder.h says which those are).
//
// Kit-free (std only) and host-tested with a fake stream type, the same way the
// graph-swap machinery is: what a bug would look like here is a thread that
// never exits, a stream filled by two workers at once, or work that never
// happens.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <utility>
#include <vector>

namespace daw {

// How long a worker sleeps when every stream it owns is full. Short enough that
// a ring that empties is refilled promptly, long enough that an idle pool is not
// a busy loop — this is the poll the per-clip threads used to do, done K times
// instead of once per clip.
inline constexpr int kStreamPoolIdleMs = 2;

// How many threads fill a graph's streams. Four keeps the fills inside a 2-vCPU
// machine's budget while overlapping a wave's disk reads; the pool's whole point
// is that this number does not grow with the clip count.
inline constexpr size_t kStreamPoolWorkers = 4;

template <class T>
class StreamPool {
public:
    // `streams` in the order they should be filled (nearest the transport
    // first); each worker gets every kStreamPoolWorkers-th of them.
    explicit StreamPool(const std::vector<T*>& streams,
                        size_t workers = kStreamPoolWorkers) {
        if (workers == 0) workers = 1;
        std::vector<std::vector<T*>> sets(workers);
        for (size_t i = 0; i < streams.size(); i++)
            sets[i % workers].push_back(streams[i]);
        for (size_t w = 0; w < workers; w++) {
            if (sets[w].empty()) continue;   // no thread for an empty set
            fThreads.emplace_back(&StreamPool::Worker, this, std::move(sets[w]));
        }
    }

    ~StreamPool() { Stop(); }

    // How many workers actually got a set (a pool with fewer streams than
    // workers starts fewer threads, and a pool with none starts none). What a
    // test can assert about the assignment being round-robin rather than
    // "everyone's work on one thread".
    size_t WorkerCount() const { return fThreads.size(); }

    StreamPool(const StreamPool&) = delete;
    StreamPool& operator=(const StreamPool&) = delete;

    // Stop and join every worker. Idempotent, and safe to call from a graph's
    // teardown: the streams must outlive this call (the owner declares the pool
    // after them, so it is destroyed first).
    void Stop() {
        if (fRun.exchange(false, std::memory_order_acq_rel) == false) return;
        for (std::thread& t : fThreads)
            if (t.joinable()) t.join();
        fThreads.clear();
    }

private:
    void Worker(std::vector<T*> mine) {
        while (fRun.load(std::memory_order_acquire)) {
            bool did = false;
            for (T* s : mine) {
                if (!fRun.load(std::memory_order_acquire)) return;
                if (!s->NeedsFilling()) continue;
                if (s->FillOnce()) did = true;
            }
            if (!did)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kStreamPoolIdleMs));
        }
    }

    std::atomic<bool>       fRun{true};
    std::vector<std::thread> fThreads;
};

} // namespace daw
