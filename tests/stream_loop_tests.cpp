// Host tests for the kit-free half of the seamless loop (M4.2):
// src/engine/StreamLoop.h — the block splitter the RT callback wraps with, the
// pass walker every clip stream's disk thread produces through, and the
// loop-aware in-place join. The arithmetic is the part a bug would live in (the
// Haiku-only wiring around it is an RT split and a producer loop), and the two
// sides have to agree: a stream must produce exactly the frames the transport
// visits, in the order it visits them, or the ring hands the callback the wrong
// audio after a seam. The cross-check at the end pins that agreement directly.
#include "../src/engine/StreamLoop.h"

#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// --- which ranges loop at all ----------------------------------------------

static void TestLoopActive() {
    std::printf("LoopActive ...\n");
    CHECK(!LoopActive(false, 0, 48000, 512));      // off
    CHECK(!LoopActive(true, 100, 100, 512));       // empty
    CHECK(!LoopActive(true, 200, 100, 512));       // inverted
    CHECK(LoopActive(true, 0, 48000, 512));        // a second
    // Shorter than the block the callback renders in: not walked (the walker
    // and the RT wrap must agree, so the rule is one function).
    CHECK(!LoopActive(true, 0, 511, 512));
    CHECK(LoopActive(true, 0, 512, 512));
    // A zero block size (nothing opened yet) must not disable every loop.
    CHECK(LoopActive(true, 0, 1, 0));
}

// --- the RT's block splitter -----------------------------------------------

static void TestSegmentFrames() {
    std::printf("SegmentFrames ...\n");
    // A block that does not reach the loop end renders whole.
    CHECK(SegmentFrames(0, 512, 1000, 2000) == 512);
    // One that lands exactly on it does too (the wrap is then at its end).
    CHECK(SegmentFrames(1488, 512, 1000, 2000) == 512);
    // One that straddles it is cut at the loop end.
    CHECK(SegmentFrames(1900, 512, 1000, 2000) == 100);
    CHECK(SegmentFrames(1999, 512, 1000, 2000) == 1);
    // At or past the loop end: the callback wraps before rendering anything.
    CHECK(SegmentFrames(2000, 512, 1000, 2000) == 0);
    CHECK(SegmentFrames(2500, 512, 1000, 2000) == 0);
}

// --- the producer's pass walker --------------------------------------------

// Collect up to `count` passes a cursor walks, as (lo, hi) pairs. Fewer come
// back when the walk ends first (`done`); a loop pass repeats forever, so the
// count is what bounds an endless walk.
static std::vector<std::pair<Frame, Frame>> Walk(ProduceCursor& c, Frame origin,
                                                 int count) {
    std::vector<std::pair<Frame, Frame>> out;
    c.Start(origin);
    while ((int)out.size() < count && !c.done) {
        out.push_back({c.lo, c.hi});
        if (!c.Next()) break;
    }
    return out;
}

static void TestProduceCursor() {
    std::printf("ProduceCursor ...\n");
    // No loop: ONE pass, the clip's own window from where playback starts. The
    // producer stops at the clip's end (the transport never reads past it).
    {
        ProduceCursor c;
        c.clipStart = 1000; c.clipEnd = 5000; c.looping = false;
        auto p = Walk(c, 0, 4);
        CHECK(p.size() == 1);
        CHECK(p[0].first == 1000 && p[0].second == 5000);
        CHECK(c.done);
        // A seek into the middle starts the one pass there.
        auto q = Walk(c, 2400, 4);
        CHECK(q.size() == 1 && q[0].first == 2400 && q[0].second == 5000);
        // And a seek past the clip produces nothing at all.
        auto r = Walk(c, 9000, 4);
        CHECK(r.empty() && c.done);
    }

    // Looping: the clip covers the loop, so the first pass runs to the loop end
    // and every pass after it is the loop's own range, for as long as the
    // stream runs.
    {
        ProduceCursor c;
        c.clipStart = 0; c.clipEnd = 100000; c.looping = true;
        c.loopStart = 48000; c.loopEnd = 96000;
        auto p = Walk(c, 0, 3);
        CHECK(p.size() == 3);
        CHECK(p[0].first == 0 && p[0].second == 96000);
        CHECK(p[1].first == 48000 && p[1].second == 96000);
        CHECK(p[2].first == 48000 && p[2].second == 96000);
        CHECK(!c.done);   // it goes round again, not off the end
    }

    // A clip that ENDS before the loop does: it plays out its first pass (up to
    // its own end) and is then spent — its frames are never visited again.
    {
        ProduceCursor c;
        c.clipStart = 1000; c.clipEnd = 20000; c.looping = true;
        c.loopStart = 48000; c.loopEnd = 96000;
        auto p = Walk(c, 0, 3);
        CHECK(p.size() == 1);
        CHECK(p[0].first == 1000 && p[0].second == 20000);
        CHECK(c.done);
    }

    // A clip that STRADDLES the loop end: its tail past loopEnd is never played
    // (the transport jumps over it), so the producer cuts there — and the pass
    // after it starts at the loop start, in the same clip.
    {
        ProduceCursor c;
        c.clipStart = 0; c.clipEnd = 70000; c.looping = true;
        c.loopStart = 48000; c.loopEnd = 96000;
        auto p = Walk(c, 0, 2);
        CHECK(p.size() == 2);
        CHECK(p[0].first == 0 && p[0].second == 70000);      // pass 1: the clip
        CHECK(p[1].first == 48000 && p[1].second == 70000);   // then the loop's
    }

    // A clip that begins INSIDE the loop: its first pass starts at its own
    // start (before the loop start), and the loop pass starts at the clip.
    {
        ProduceCursor c;
        c.clipStart = 10000; c.clipEnd = 200000; c.looping = true;
        c.loopStart = 48000; c.loopEnd = 96000;
        auto p = Walk(c, 0, 2);
        CHECK(p.size() == 2);
        CHECK(p[0].first == 10000 && p[0].second == 96000);
        CHECK(p[1].first == 48000 && p[1].second == 96000);
    }

    // A clip entirely inside the loop, played from before it.
    {
        ProduceCursor c;
        c.clipStart = 50000; c.clipEnd = 60000; c.looping = true;
        c.loopStart = 48000; c.loopEnd = 96000;
        auto p = Walk(c, 0, 2);
        CHECK(p.size() == 2);
        CHECK(p[0].first == 50000 && p[0].second == 60000);
        CHECK(p[1].first == 50000 && p[1].second == 60000);   // every pass
    }

    // A clip entirely AFTER the loop end never plays while the loop is on: the
    // walker walks straight to the loop pass (empty) and reports done.
    {
        ProduceCursor c;
        c.clipStart = 100000; c.clipEnd = 120000; c.looping = true;
        c.loopStart = 48000; c.loopEnd = 96000;
        auto p = Walk(c, 0, 3);
        CHECK(p.empty() && c.done);
    }

    // The transport already past the loop end when the graph is built (a loop
    // enabled with the playhead beyond it): the first pass is empty, and the
    // producer starts with the loop's own — which is where the RT wraps to.
    {
        ProduceCursor c;
        c.clipStart = 0; c.clipEnd = 200000; c.looping = true;
        c.loopStart = 48000; c.loopEnd = 96000;
        auto p = Walk(c, 120000, 2);
        CHECK(p.size() == 2);
        CHECK(p[0].first == 48000 && p[0].second == 96000);
    }

    // Frames(): the pass's length, and 0 once spent.
    {
        ProduceCursor c;
        c.clipStart = 0; c.clipEnd = 100000; c.looping = true;
        c.loopStart = 48000; c.loopEnd = 96000;
        c.Start(0);
        CHECK(c.Frames() == 96000);
        CHECK(c.Next() && c.Frames() == 48000);
    }
}

// --- the source frame a pass starts at --------------------------------------

static void TestPassSourceFrame() {
    std::printf("PassSourceFrame ...\n");
    // Same rate: the offset plus the frames into the clip.
    CHECK(PassSourceFrame(0, 1000, 3000, 48000.0, 48000.0) == 2000);
    CHECK(PassSourceFrame(500, 1000, 1000, 48000.0, 48000.0) == 500);
    // Half the rate: two timeline frames per source frame.
    CHECK(PassSourceFrame(0, 0, 100, 24000.0, 48000.0) == 50);
    // Double: one source frame per two timeline frames.
    CHECK(PassSourceFrame(0, 0, 100, 96000.0, 48000.0) == 200);
    // The rounding is +0.5, the same rule TrackStream::Prepare used for the
    // graph's opening seek — a pass start and that first seek must not land a
    // frame apart.
    CHECK(PassSourceFrame(0, 0, 3, 44100.0, 48000.0)
          == (int64_t)(3.0 * 44100.0 / 48000.0 + 0.5));
}

// --- the loop-aware in-place join -------------------------------------------

static void TestLoopRebase() {
    std::printf("LoopRebaseSkipFrames ...\n");
    // No loop: exactly the M4.1 arithmetic.
    CHECK(LoopRebaseSkipFrames(0, 0, 100, 40, false, 0, 0, 0) == 40);
    CHECK(LoopRebaseSkipFrames(0, 50, 150, 90, false, 0, 0, 0) == 40);
    // A loop that nothing has wrapped yet: the direct path again.
    CHECK(LoopRebaseSkipFrames(0, 0, 100, 40, true, 0, 50, 0) == 40);
    // Seven wraps behind it: pass 1 (50) + six complete passes (300) + the head
    // of the one it is in (40).
    CHECK(LoopRebaseSkipFrames(0, 0, 100, 40, true, 0, 50, 7) == 50 + 6 * 50 + 40);

    // One wrap during the build. The transport ran from the origin to the loop
    // end (frames IN THE WINDOW count: 0..50 = 50) and then to `target` in the
    // loop's own pass (target - loopStart).
    CHECK(LoopRebaseSkipFrames(0, 0, 100, 20, true, 10, 50, 1) == 60);
    // The clip only covers part of that path: only in-window frames are
    // consumed, so only those are skipped.
    CHECK(LoopRebaseSkipFrames(20, 25, 40, 20, true, 10, 50, 1) == 15);
    // Two wraps: one complete loop pass (loopStart..loopEnd ∩ window) more.
    CHECK(LoopRebaseSkipFrames(0, 0, 100, 20, true, 10, 50, 2) == 60 + 40);
    CHECK(LoopRebaseSkipFrames(0, 0, 100, 20, true, 10, 50, 3) == 60 + 80);
    // The origin already past the loop end (the transport wrapped at once):
    // the first pass contributed nothing.
    CHECK(LoopRebaseSkipFrames(80, 0, 100, 20, true, 10, 50, 1) == 10);
    // A clip whose window is entirely before the loop: its whole first pass is
    // consumed (the transport plays up to the loop end), and after the wrap the
    // transport never comes back to it — so that pass is all that is ever
    // skipped, however many wraps follow.
    CHECK(LoopRebaseSkipFrames(0, 0, 100, 20, true, 200, 300, 3) == 100);
    // An inverted loop is not a loop.
    CHECK(LoopRebaseSkipFrames(0, 0, 100, 40, true, 50, 10, 3) == 40);
}

// The property the design rests on: the frames the pass walker produces before
// the transport reaches `target` — after `wraps` wraps — are EXACTLY the frames
// the callback consumes getting there. If these two ever disagree, a stream
// hands the ring the wrong audio after a seam, and the loop plays out of phase
// with itself.
static void TestWalkMatchesRebase() {
    std::printf("the walker and the join agree ...\n");
    const Frame clipStart = 1000, clipEnd = 90000;
    const Frame ls = 20000, le = 60000;
    // Frames the transport visits walking [from, to) INSIDE the transport's own
    // path: the cursor's definition, spelled out independently here.
    auto visited = [&](Frame from, Frame to) {
        Frame lo = from > clipStart ? from : clipStart;
        Frame hi = to < clipEnd ? to : clipEnd;
        return hi > lo ? hi - lo : 0;
    };

    for (Frame origin : {Frame(0), Frame(3000), Frame(55000), Frame(70000)}) {
        for (uint64_t wraps = 0; wraps <= 3; wraps++) {
            // The transport: [origin, le), then `wraps` passes of [ls, le),
            // stopping at `target` in the last one.
            const Frame target = ls + 5000;
            Frame expect = 0;
            if (wraps == 0) {
                expect = visited(origin, target);
            } else {
                expect = visited(origin, le);
                if (wraps > 1) expect += (Frame)(wraps - 1) * visited(ls, le);
                expect += visited(ls, target);
            }
            CHECK(LoopRebaseSkipFrames(origin, clipStart, clipEnd, target, true,
                                       ls, le, wraps) == expect);

            // The producer's own walk over the same path. The frames the
            // callback consumed are a PREFIX of what the walker produces, in the
            // order it produces them: the passes the transport completed, then
            // part of the pass it is in now. If the walker's passes did not
            // partition the path exactly like this, the ring would hand the
            // callback the wrong audio after a seam.
            const Frame firstPass = visited(origin, le);
            // A first pass with no frames in it is skipped rather than counted
            // (the transport wrapped before it ever visited the clip), so the
            // completed passes are one fewer.
            const uint64_t complete = (wraps == 0) ? 0
                                     : (firstPass == 0 ? wraps - 1 : wraps);
            ProduceCursor c;
            c.clipStart = clipStart; c.clipEnd = clipEnd; c.looping = true;
            c.loopStart = ls; c.loopEnd = le;
            c.Start(origin);
            Frame produced = 0;
            for (uint64_t i = 0; i < complete && !c.done; i++) {
                produced += c.Frames();
                if (!c.Next()) break;
            }
            CHECK(produced <= expect);
            if (!c.done) {
                // The pass the transport is in holds the rest of what it ate.
                CHECK(expect - produced <= c.Frames());
            } else {
                CHECK(produced == expect);
            }
        }
    }
}

int main() {
    TestLoopActive();
    TestSegmentFrames();
    TestProduceCursor();
    TestPassSourceFrame();
    TestLoopRebase();
    TestWalkMatchesRebase();
    std::printf("stream_loop_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
