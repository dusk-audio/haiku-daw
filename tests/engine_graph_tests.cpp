// Host tests for the two kit-free pieces of the engine-graph swap (M4.1):
//
//   - src/engine/GraphSwap.h — the atomic graph slot + its reclaim thread,
//     where a bug is a use-after-free or a leaked graph. A publisher/consumer
//     stress loop runs under ASan (build with -DDAW_SANITIZE=ON for teeth: a
//     graph freed while the "RT" thread still holds it is a real UAF).
//   - src/engine/StreamRebase.h — the arithmetic that re-aligns a clip stream
//     when an in-place rebuild is swapped in while the transport keeps rolling.
#include "../src/engine/GraphSwap.h"
#include "../src/engine/RingBuffer.h"
#include "../src/engine/StreamRebase.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// --- a fake graph that counts its own births and deaths ---------------------

static std::atomic<int> g_alive{0};
static std::atomic<int> g_born{0};
static std::atomic<int> g_died{0};

struct FakeGraph {
    int payload = 0;
    FakeGraph() { g_alive.fetch_add(1); g_born.fetch_add(1); }
    ~FakeGraph() { g_alive.fetch_sub(1); g_died.fetch_add(1); }
};

static std::unique_ptr<FakeGraph> MakeFake(int payload) {
    auto g = std::make_unique<FakeGraph>();
    g->payload = payload;
    return g;
}

// Poll until `pred` holds (a few ms is all any of these need).
template <class Fn>
static bool WaitFor(const Fn& pred, int maxMs = 5000) {
    for (int i = 0; i < maxMs; i++) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// --- the swap slot + reclaimer ---------------------------------------------

static void TestSwapBasics() {
    std::printf("GraphSwap: publish/detach/reclaim ...\n");
    g_alive = 0; g_born = 0; g_died = 0;

    {
        std::atomic<bool> quiesced{true};
        GraphSwap<FakeGraph> slot([&] { return quiesced.load(); });

        CHECK(slot.Active() == nullptr);
        slot.Publish(MakeFake(1));
        CHECK(slot.Active() != nullptr);
        CHECK(slot.Active()->payload == 1);

        slot.Publish(MakeFake(2));                 // replaces 1
        CHECK(slot.Active()->payload == 2);
        CHECK(WaitFor([&] { return g_died.load() == 1; }));
        CHECK(g_alive.load() == 1);                // only the active one

        slot.Detach();                             // retires 2, nothing active
        CHECK(slot.Active() == nullptr);
        CHECK(WaitFor([&] { return g_died.load() == 2 && g_alive.load() == 0; }));
    }
    // The destructor frees whatever was left; nothing may leak.
    CHECK(g_alive.load() == 0);
    CHECK(g_born.load() == g_died.load());
    std::printf("  born %d, died %d\n", g_born.load(), g_died.load());
}

// The point of the reclaimer: a retired graph is NOT freed while the RT side
// may still hold it, and IS freed once the owner confirms quiescence.
static void TestNotFreedUntilQuiesced() {
    std::printf("GraphSwap: a retired graph waits for quiescence ...\n");
    g_alive = 0; g_born = 0; g_died = 0;

    std::atomic<bool> quiesced{false};
    GraphSwap<FakeGraph> slot([&] { return quiesced.load(); });

    slot.Publish(MakeFake(1));
    slot.Publish(MakeFake(2));      // 1 is now retired, but NOT free-able
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(g_died.load() == 0);      // still held: the RT could be in it
    CHECK(g_alive.load() == 2);

    quiesced.store(true);           // the callback boundary has passed
    CHECK(WaitFor([&] { return g_died.load() == 1; }));
    CHECK(g_alive.load() == 1);     // the active graph is untouched

    quiesced.store(false);
    slot.Publish(MakeFake(3));      // retire 2 while unquiesced again
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(g_died.load() == 1);
    quiesced.store(true);
    CHECK(WaitFor([&] { return g_died.load() == 2; }));
    CHECK(g_alive.load() == 1);
    // The active graph is freed by the destructor, quiesced by then.
    CHECK(g_born.load() == 3);
}

// A "real" RT-style consumer loop over the slot while a publisher keeps
// replacing the graph. Every graph is touched through the pointer the consumer
// loaded; under ASan a graph freed too early is a use-after-free here.
static void TestSwapUnderLoad() {
    std::printf("GraphSwap: publisher/consumer under load (ASan) ...\n");
    g_alive = 0; g_born = 0; g_died = 0;

    // The consumer bumps a generation each time around its loop; the owner's
    // predicate mirrors the engine's rule: two boundaries after the retire.
    std::atomic<uint64_t> gen{0};
    std::atomic<uint64_t> retireGen{0};
    std::atomic<bool>     running{true};
    std::atomic<bool>     stopConsumer{false};

    GraphSwap<FakeGraph> slot([&] {
        return !running.load() || gen.load() >= retireGen.load() + 2;
    });

    std::thread consumer([&] {
        for (;;) {
            FakeGraph* g = slot.Active();     // ONE load per "block"
            const bool have = (g != nullptr);
            if (have) {
                // HOLD it the way a block does, then touch it: if the reclaimer
                // frees a retired graph too early, this read lands on freed
                // memory and ASan reports it. (Without the hold the window is
                // too small for the race to be seen at all, and only the
                // deterministic checks above would notice.)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                volatile int v = g->payload;
                (void)v;
            }
            gen.fetch_add(1, std::memory_order_release);
            if (stopConsumer.load()) break;
            if (!have)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    const int kGraphs = 200;
    for (int i = 0; i < kGraphs; i++) {
        retireGen.store(gen.load(std::memory_order_acquire));
        slot.Publish(MakeFake(i));
    }
    running.store(false);
    stopConsumer.store(true);
    consumer.join();

    CHECK(WaitFor([&] { return g_alive.load() == 1; }, 5000));
    CHECK(g_alive.load() == 1);            // only the last published graph
    CHECK(g_born.load() == kGraphs);
    CHECK(g_died.load() == kGraphs - 1);   // the destructor frees the last
}

// --- the rebase arithmetic --------------------------------------------------

static void TestRebaseMath() {
    std::printf("RebaseSkipFrames: the edges ...\n");
    CHECK(RebaseSkipFrames(0, 0, 100, 0) == 0);        // nothing consumed yet
    CHECK(RebaseSkipFrames(0, 0, 100, 40) == 40);      // 40 frames rolled on
    CHECK(RebaseSkipFrames(0, 0, 100, 100) == 100);    // target at the end
    CHECK(RebaseSkipFrames(0, 0, 100, 500) == 100);    // target past the clip
    CHECK(RebaseSkipFrames(60, 0, 100, 50) == 0);      // target before origin
    // Origin inside the window, target past the end: skip to the end only.
    CHECK(RebaseSkipFrames(30, 0, 100, 500) == 70);
    // A build that started BEFORE the clip (origin clamps to the clip start):
    // only the frames inside the window count.
    CHECK(RebaseSkipFrames(0, 50, 150, 90) == 40);
    CHECK(RebaseSkipFrames(0, 50, 150, 120) == 70);
    CHECK(RebaseSkipFrames(0, 50, 150, 40) == 0);      // target before clip start
    // Origins past the clip, and the frames OUTSIDE the window that the stream
    // never consumed (the gap between two clips is not skipped).
    CHECK(RebaseSkipFrames(200, 0, 100, 300) == 0);
    CHECK(RebaseSkipFrames(20, 50, 150, 70) == 20);
    CHECK(RebaseSkipFrames(20, 50, 150, 40) == 0);
    CHECK(RebaseSkipFrames(0, 100, 100, 150) == 0);    // empty window
}

// RingBuffer::Skip drops exactly what it says and leaves the rest readable.
static void TestRingSkip() {
    std::printf("RingBuffer::Skip ...\n");
    RingBuffer ring(64);
    std::vector<float> in(40);
    for (size_t i = 0; i < in.size(); i++) in[i] = (float)i;
    CHECK(ring.Write(in.data(), in.size()) == 40);

    CHECK(ring.Skip(0) == 0);
    CHECK(ring.Skip(10) == 10);
    float out[4] = {};
    CHECK(ring.Read(out, 2) == 2);
    CHECK(out[0] == 10.0f && out[1] == 11.0f);   // what followed the skip

    CHECK(ring.Skip(1000) == 28);                // clamps to what is there
    CHECK(ring.ReadAvailable() == 0);
    CHECK(ring.Skip(5) == 0);                    // empty: nothing to skip

    // Skipping interleaved stereo frames is "2 floats per frame" at the call
    // site; the ring only knows floats.
    CHECK(ring.Write(in.data(), 8) == 8);
    CHECK(ring.Skip(4) == 4);
    CHECK(ring.Read(out, 1) == 1);
    CHECK(out[0] == 4.0f);
}

int main() {
    TestSwapBasics();
    TestNotFreedUntilQuiesced();
    TestSwapUnderLoad();
    TestRebaseMath();
    TestRingSkip();
    std::printf("engine_graph_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
