// GraphSwap — the RT-safe half of an engine graph rebuild.
//
// The audio callback may never wait for anything, so a rebuilt graph is handed
// to it by ONE atomic pointer exchange that the callback performs once per
// block: it loads the pointer at the top and renders that graph for the whole
// block, whatever happens next. The graph it replaced cannot simply be deleted
// — the callback may still be running it — so it is queued on a reclaim thread
// that frees it once the owner says no RT thread can still be holding it.
//
// This is the machinery only; the graph type is the caller's. It is kit-free
// (STL + <thread>) so the part where the bugs would live — a use-after-free or
// a leaked graph — is host-tested (tests/engine_graph_tests.cpp), including
// under ASan with a real publisher/consumer loop.
//
// The owner's contract:
//   - `quiesced()` returns true only when no RT thread can still be running
//     with a pointer it loaded from Active() before the most recent
//     Publish()/Detach(). It is called from the reclaim thread, so it must not
//     take a lock the publisher holds and must not block for long.
//   - By the time the owner destroys the GraphSwap, no RT thread is running and
//     no thread will publish again (the engine stops the player and joins its
//     builder first). The destructor then frees whatever is left.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace daw {

template <class Graph>
class GraphSwap {
public:
    // True when no RT thread can still hold a graph retired earlier (see the
    // header comment). Called on the reclaim thread only.
    using QuiesceFn = std::function<bool()>;

    explicit GraphSwap(QuiesceFn quiesced)
        : fQuiesced(std::move(quiesced)) {
        fReclaimer = std::thread(&GraphSwap::ReclaimLoop, this);
    }

    ~GraphSwap() {
        {
            std::lock_guard<std::mutex> lk(fMutex);
            fStop = true;
        }
        fCv.notify_all();
        if (fReclaimer.joinable()) fReclaimer.join();
        // Quiesced by contract: free the queued graphs, then the live one.
        fGarbage.clear();
        delete fActive.exchange(nullptr, std::memory_order_acq_rel);
    }

    GraphSwap(const GraphSwap&) = delete;
    GraphSwap& operator=(const GraphSwap&) = delete;

    // The RT side. Call it ONCE per block and use what it returned for the
    // whole block — a second call could return a newer graph, and the block
    // would then be rendered from two.
    Graph* Active() const { return fActive.load(std::memory_order_acquire); }

    // Publish a graph, retiring whatever was active. Safe from any non-RT
    // thread; the swap itself is one atomic exchange.
    void Publish(std::unique_ptr<Graph> next) {
        Graph* old = fActive.exchange(next.release(),
                                      std::memory_order_acq_rel);
        if (old) Retire(old);
    }

    // Go silent: retire the active graph and leave none. The RT then renders
    // nothing until the next Publish (that is what a position-changing rebuild
    // wants: the old graph's content is for the wrong place).
    void Detach() { Publish(nullptr); }

    // How many graphs are waiting to be freed (for tests/teardown diagnostics).
    size_t Retired() const {
        std::lock_guard<std::mutex> lk(fMutex);
        return fGarbage.size();
    }

private:
    void Retire(Graph* g) {
        {
            std::lock_guard<std::mutex> lk(fMutex);
            if (fStop) {   // teardown: the owner has already quiesced the RT
                delete g;
                return;
            }
            fGarbage.push_back(g);
        }
        fCv.notify_all();
    }

    void ReclaimLoop() {
        // The item being reclaimed is owned HERE, not left in the queue: the
        // wait below runs without the lock, and a publisher must be free to
        // push onto the deque meanwhile (a deque push_back under a concurrent
        // unlocked front() is a data race, even if the element itself is not
        // moved).
        Graph* current = nullptr;
        for (;;) {
            if (!current) {
                std::unique_lock<std::mutex> lk(fMutex);
                fCv.wait(lk, [this] { return fStop || !fGarbage.empty(); });
                if (fGarbage.empty()) return;      // fStop, nothing left
                current = fGarbage.front();
                fGarbage.pop_front();
            }
            while (!fQuiesced())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            delete current;
            current = nullptr;
        }
    }

    std::atomic<Graph*>                fActive{nullptr};
    QuiesceFn                          fQuiesced;
    mutable std::mutex                 fMutex;
    std::condition_variable            fCv;
    std::deque<Graph*>                 fGarbage;
    bool                               fStop = false;
    std::thread                        fReclaimer;
};

} // namespace daw
