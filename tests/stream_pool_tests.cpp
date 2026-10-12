// Host tests for the kit-free half of the stream pool (M4.3):
//
//   - src/engine/StreamPool.h — the fixed set of fill threads. What a bug looks
//     like here is a worker that never exits, a stream filled by two workers at
//     once, or work that simply never happens, so the fake stream below counts
//     its fills and flags a concurrent one. Run under ASan for teeth.
//   - src/engine/StreamOrder.h — which clip the transport needs next, which is
//     what decides who fills first, and where a loop makes "behind the playhead"
//     mean "needed again in a moment".
#include "../src/engine/StreamOrder.h"
#include "../src/engine/StreamPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// --- what the transport needs next -----------------------------------------

static void TestStreamOrder() {
    std::printf("StreamDueKey ...\n");
    const Frame NEVER = kStreamNeverKey;
    // No loop: playing now is 0, ahead is the distance, behind is never again.
    CHECK(StreamDueKey(0, 1000, 500, false, 0, 0) == 0);          // playing
    CHECK(StreamDueKey(0, 500, 500, false, 0, 0) == NEVER);       // just ended
    CHECK(StreamDueKey(2000, 3000, 500, false, 0, 0) == 1500);    // ahead
    CHECK(StreamDueKey(600, 700, 500, false, 0, 0) == 100);       // 100 ahead
    // Behind sorts after everything that is still to come.
    CHECK(StreamDueKey(0, 100, 500, false, 0, 0) > NEVER - 1);
    CHECK(StreamDueKey(0, 100, 500, false, 0, 0)
          < StreamDueKey(300, 400, 500, false, 0, 0));

    // A loop: a clip inside the loop that the playhead has passed is needed
    // again one wrap away, which beats anything outside the loop.
    const Frame ls = 1000, le = 5000;
    CHECK(StreamDueKey(2000, 3000, 2500, true, ls, le) == 0);       // playing
    CHECK(StreamDueKey(3000, 4000, 2500, true, ls, le) == 500);     // ahead
    // Behind the playhead but inside the loop: distance = to the wrap, then to
    // the clip's start.
    CHECK(StreamDueKey(1000, 2000, 2500, true, ls, le) == (le - 2500) + 0);
    CHECK(StreamDueKey(1500, 2000, 2500, true, ls, le) == (le - 2500) + 500);
    // Outside the loop entirely: never again, however the loop is placed.
    CHECK(StreamDueKey(6000, 7000, 2500, true, ls, le) >= NEVER);
    CHECK(StreamDueKey(0, 500, 2500, true, ls, le) >= NEVER);
    // An inverted or empty loop is not a loop.
    CHECK(StreamDueKey(0, 100, 500, true, 200, 100) >= NEVER);

    // The key is a sort key: applied to a project it must put the clips the
    // transport will reach soonest first.
    struct C { Frame s, e; };
    const C clips[] = {{6000, 7000}, {0, 500}, {1200, 1800}, {2000, 2100}};
    std::vector<int> idx = {0, 1, 2, 3};
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        return StreamDueKey(clips[a].s, clips[a].e, 1500, true, ls, le)
             < StreamDueKey(clips[b].s, clips[b].e, 1500, true, ls, le);
    });
    CHECK(idx[0] == 2);   // [1200,1800): playing right now
    CHECK(idx[1] == 3);   // [2000,2100): 500 ahead, inside the loop
    CHECK(idx[2] == 1);   // [0,500): inside the loop, again after the wrap
    CHECK(idx[3] == 0);   // [6000,7000): outside the loop, never again
}

// --- the fill pool ----------------------------------------------------------

// A fake stream: "fills" one unit per call, up to `want`, and remembers whether
// it was ever inside FillOnce twice at once (which must never happen: static
// assignment, StreamPool.h).
struct FakeStream {
    std::atomic<int>  filled{0};
    int               want = 50;
    std::atomic<int>  overlaps{0};
    std::atomic<bool> inFill{false};
    std::atomic<int>  calls{0};        // every FillOnce entry
    bool              stuck = false;   // NeedsFilling() true, but no progress

    bool NeedsFilling() const { return stuck || filled.load() < want; }
    bool FillOnce() {
        calls.fetch_add(1);
        // A stream that cannot progress does NO work — no sleep either, or the
        // fake would hide exactly the spin this is here to catch.
        if (stuck) return false;       // like a source that has ended
        if (inFill.exchange(true)) overlaps.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        filled.fetch_add(1);
        inFill.store(false);
        return true;
    }
};

template <class Fn>
static bool WaitFor(const Fn& pred, int maxMs = 5000) {
    for (int i = 0; i < maxMs; i++) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

static void TestStreamPool() {
    std::printf("StreamPool ...\n");

    // Every stream gets filled — including when there are many more streams
    // than workers (the case the per-clip threads existed for).
    {
        const size_t N = 40;
        std::vector<FakeStream> streams(N);
        for (FakeStream& s : streams) s.want = 8;
        std::vector<FakeStream*> ptrs;
        for (FakeStream& s : streams) ptrs.push_back(&s);

        StreamPool<FakeStream> pool(ptrs);
        CHECK(WaitFor([&] {
            for (FakeStream& s : streams)
                if (s.filled.load() < s.want) return false;
            return true;
        }));
        for (FakeStream& s : streams) CHECK(s.filled.load() == s.want);
        int overlaps = 0;
        for (FakeStream& s : streams) overlaps += s.overlaps.load();
        CHECK(overlaps == 0);      // one worker per stream, always
    }

    // Stop() joins: no worker may still be filling once it returns, and a
    // stopped pool must not touch its streams again (the graph's streams are
    // destroyed right after — that is the ordering this protects).
    {
        std::vector<FakeStream> streams(8);
        for (FakeStream& s : streams) s.want = 1000000;   // never satisfied
        std::vector<FakeStream*> ptrs;
        for (FakeStream& s : streams) ptrs.push_back(&s);

        StreamPool<FakeStream>* pool = new StreamPool<FakeStream>(ptrs);
        CHECK(WaitFor([&] { return streams[0].filled.load() > 0; }));
        pool->Stop();
        std::vector<int> at;
        for (FakeStream& s : streams) at.push_back(s.filled.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        for (size_t i = 0; i < streams.size(); i++)
            CHECK(streams[i].filled.load() == at[i]);   // nobody is running
        delete pool;                                    // dtor after Stop: fine
    }

    // A pool over NO streams must not start a thread at all, and stopping it is
    // a no-op (a monitor-only graph has no streams).
    {
        std::vector<FakeStream*> none;
        StreamPool<FakeStream> pool(none);
        CHECK(pool.WorkerCount() == 0);
        pool.Stop();
        CHECK(true);   // reaching here is the check: no hang, no crash
    }

    // The assignment is round-robin across workers, not everyone's work on the
    // first thread: 8 streams and 4 workers is 4 workers, and a pool of 2
    // streams is 2.
    {
        std::vector<FakeStream> streams(8);
        std::vector<FakeStream*> ptrs;
        for (FakeStream& s : streams) ptrs.push_back(&s);
        StreamPool<FakeStream> pool(ptrs, 4);
        CHECK(pool.WorkerCount() == 4);
        pool.Stop();
        std::vector<FakeStream> two(2);
        std::vector<FakeStream*> twoP;
        for (FakeStream& s : two) twoP.push_back(&s);
        StreamPool<FakeStream> small(twoP, 4);
        CHECK(small.WorkerCount() == 2);   // one thread per non-empty set
        small.Stop();
    }

    // A stream that cannot make progress must not make a worker SPIN. This is
    // the bug the VM found first: a stream whose source had ended kept
    // NeedsFilling() true, and the worker hammered it 683922 times in 200 ms
    // instead of sleeping. The calls are counted, not the progress.
    {
        std::vector<FakeStream> streams(4);
        for (FakeStream& s : streams) s.stuck = true;
        std::vector<FakeStream*> ptrs;
        for (FakeStream& s : streams) ptrs.push_back(&s);
        StreamPool<FakeStream> pool(ptrs);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        int calls = 0;
        for (FakeStream& s : streams) calls += s.calls.load();
        // Two polls per idle millisecond per stream is ~500 calls for 60 ms;
        // a spin is millions. The bound is deliberately loose.
        CHECK(calls < 5000);
        pool.Stop();
    }

    // The order given is the order the workers get: with one worker, the first
    // stream is filled before the last one is touched (the property the build's
    // due-order relies on when the playhead is near the end of a project).
    {
        std::vector<FakeStream> streams(6);
        for (FakeStream& s : streams) s.want = 1000000;
        std::vector<FakeStream*> ptrs;
        for (FakeStream& s : streams) ptrs.push_back(&s);
        StreamPool<FakeStream> pool(ptrs, /*workers=*/1);
        CHECK(WaitFor([&] { return streams[0].filled.load() > 0; }));
        // The first stream in the list is the one being worked on.
        CHECK(streams[0].filled.load() >= streams[5].filled.load());
        pool.Stop();
    }
}

int main() {
    TestStreamOrder();
    TestStreamPool();
    std::printf("stream_pool_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
