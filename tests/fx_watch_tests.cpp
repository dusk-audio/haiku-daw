// Host tests for the native-editor watch table (src/plugin/FxWatchTable.h).
//
// These rules decide whether an open plugin editor keeps showing the insert it
// was opened on. Three of them were wrong in the first cut of package 07 --
// one editor's close dropping another's watch, an editor left driving whatever
// insert moved into its index after a reorder, and an editor whose insert was
// removed staying open. The table is kit-free precisely so all of that can be
// pinned here instead of on the machine, by looking at it.

#include "../src/plugin/FxWatchTable.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

namespace {

constexpr TrackId kT1 = 7;
constexpr int     kSlots = 4;

// Records what the table asked of an editor window.
struct FakeEditor : public FxWatchEditor {
    bool alive = true;
    int  closes = 0;
    int  moves = 0;
    int  lastFx = -1;
    TrackId lastTrack = kInvalidTrackId;
    int  frames = 0;

    bool Alive() const override { return alive; }
    void AskToClose() override { ++closes; }
    void InsertMoved(TrackId t, int fx) override {
        ++moves; lastTrack = t; lastFx = fx;
    }
    void SendFrame(const float*, int) override { ++frames; }
};

FxWatch Entry(FakeEditor* e, const std::string& uri, TrackId track, int fx,
              int slot) {
    FxWatch w;
    w.editor = e;
    w.uri = uri;
    w.track = track;
    w.fx = fx;
    w.slot = slot;
    return w;
}

FxChainView Chain(std::vector<std::string> uris) {
    FxChainView v;
    v.exists = true;
    v.uris = std::move(uris);
    return v;
}

} // namespace

int main() {
    // --- registration identity ---------------------------------------------
    {
        FakeEditor a, b;
        std::vector<FxWatch> e{ Entry(&a, "urn:eq", kT1, 0, 0),
                                Entry(&b, "urn:comp", kT1, 1, 1) };
        // An editor is found by its OWN address, so closing one can never drop
        // the other's watch -- the bug the first cut had.
        CHECK(FxWatchFind(e, "urn:eq", kT1, 0) == 0);
        CHECK(FxWatchFind(e, "urn:comp", kT1, 1) == 1);
        CHECK(FxWatchFind(e, "urn:eq", kT1, 1) == -1);     // same uri, other slot
        CHECK(FxWatchFind(e, "urn:comp", kT1, 0) == -1);
        CHECK(FxWatchFind(e, "urn:eq", (TrackId)99, 0) == -1);
        CHECK(FxWatchFind(e, "urn:none", kT1, 0) == -1);

        // The same plugin on two tracks is two different editors.
        std::vector<FxWatch> two{ Entry(&a, "urn:eq", kT1, 0, 0),
                                  Entry(&b, "urn:eq", (TrackId)8, 0, 1) };
        CHECK(FxWatchFind(two, "urn:eq", kT1, 0) == 0);
        CHECK(FxWatchFind(two, "urn:eq", (TrackId)8, 0) == 1);
    }

    // --- engine watch slots -------------------------------------------------
    {
        FakeEditor a, b, c;
        std::vector<FxWatch> e{ Entry(&a, "u1", kT1, 0, 0),
                                Entry(&b, "u2", kT1, 1, 2) };
        CHECK(FxWatchFreeSlot(e, kSlots) == 1);           // lowest free
        CHECK(FxWatchFreeSlot({}, kSlots) == 0);
        std::vector<FxWatch> full{ Entry(&a, "u1", kT1, 0, 0),
                                   Entry(&b, "u2", kT1, 0, 1),
                                   Entry(&c, "u3", kT1, 0, 2) };
        full.push_back(Entry(&a, "u4", kT1, 0, 3));
        CHECK(FxWatchFreeSlot(full, kSlots) == -1);       // all four taken
        // A fifth editor still works -- it just gets no slot and stops
        // following automation -- so the table must not crash or hand out a
        // slot that is already in use.
    }

    // --- what a chain edit means -------------------------------------------
    {
        FakeEditor a;
        std::vector<FxWatch> e{ Entry(&a, "urn:eq", kT1, 1, 0) };

        // Nothing moved: keep.
        auto u = FxWatchValidate(e, { Chain({ "urn:comp", "urn:eq" }) });
        CHECK(u.size() == 1 && u[0].action == FxWatchAction::Keep);

        // Moved within the chain: follow it, and say where it is now.
        u = FxWatchValidate(e, { Chain({ "urn:eq", "urn:comp" }) });
        CHECK(u.size() == 1 && u[0].action == FxWatchAction::Rebind);
        if (!u.empty()) CHECK(u[0].newFx == 0);

        // Removed: close -- its every control would otherwise edit an insert
        // that no longer exists.
        u = FxWatchValidate(e, { Chain({ "urn:comp" }) });
        CHECK(u.size() == 1 && u[0].action == FxWatchAction::Close);

        // The whole track is gone: close.
        u = FxWatchValidate(e, { FxChainView{} });
        CHECK(u.size() == 1 && u[0].action == FxWatchAction::Close);

        // An index past the end is NOT on its own a reason to close: inserts
        // above the watched one may have been removed, shifting it down. The
        // insert is still there and unique, so the editor follows it.
        std::vector<FxWatch> far{ Entry(&a, "urn:eq", kT1, 5, 0) };
        u = FxWatchValidate(far, { Chain({ "urn:eq" }) });
        CHECK(u.size() == 1 && u[0].action == FxWatchAction::Rebind);
        if (!u.empty()) CHECK(u[0].newFx == 0);

        // The window died: close (the caller then drops the entry).
        a.alive = false;
        u = FxWatchValidate(e, { Chain({ "urn:comp", "urn:eq" }) });
        CHECK(u.size() == 1 && u[0].action == FxWatchAction::Close);
        a.alive = true;
    }

    // --- two copies of one plugin ------------------------------------------
    {
        FakeEditor a;
        // Same URI at two indices: the editor was opened on one of them and a
        // reorder has made them indistinguishable, so following the index would
        // as likely drive the other insert. Close rather than guess.
        std::vector<FxWatch> e{ Entry(&a, "urn:eq", kT1, 1, 0) };
        auto u = FxWatchValidate(e, { Chain({ "urn:eq", "urn:eq" }) });
        CHECK(u.size() == 1 && u[0].action == FxWatchAction::Close);

        // Still ambiguous when the OTHER copy is elsewhere in the chain.
        u = FxWatchValidate(e, { Chain({ "urn:comp", "urn:eq", "urn:eq" }) });
        CHECK(u.size() == 1 && u[0].action == FxWatchAction::Close);
    }

    // --- several editors at once -------------------------------------------
    {
        FakeEditor a, b, c;
        // Two editors on ONE track (which swap places in the chain) and one on
        // another track that does not move.
        std::vector<FxWatch> e{ Entry(&a, "urn:eq", kT1, 0, 0),
                                Entry(&b, "urn:comp", kT1, 1, 1),
                                Entry(&c, "urn:verb", (TrackId)8, 0, 2) };
        const std::vector<FxChainView> chains{
            Chain({ "urn:comp", "urn:eq" }),   // the same chain: eq 0 -> 1
            Chain({ "urn:comp", "urn:eq" }),   // ... and comp 1 -> 0
            Chain({ "urn:verb" }),             // untouched track
        };
        const auto u = FxWatchValidate(e, chains);
        CHECK(u.size() == 3);
        CHECK(u[0].action == FxWatchAction::Rebind && u[0].newFx == 1);
        CHECK(u[1].action == FxWatchAction::Rebind && u[1].newFx == 0);
        CHECK(u[2].action == FxWatchAction::Keep);

        // And a chain that lost one insert while another stayed: the updates are
        // per entry, so the caller can act on each without re-deriving anything.
        const std::vector<FxChainView> after{ Chain({ "urn:comp" }),
                                              Chain({ "urn:comp" }),
                                              FxChainView{} };
        const auto u2 = FxWatchValidate(e, after);
        CHECK(u2[0].action == FxWatchAction::Close);   // eq removed: nothing to show
        CHECK(u2[1].action == FxWatchAction::Rebind);  // comp is still there, now at 0
        if (u2.size() > 1) CHECK(u2[1].newFx == 0);
        CHECK(u2[2].action == FxWatchAction::Close);   // its track is gone
    }

    {
        // The same chain, one insert removed from ABOVE the watched one: the
        // editor follows the shift rather than sitting on its old index.
        FakeEditor a;
        std::vector<FxWatch> e{ Entry(&a, "urn:eq", kT1, 2, 0) };
        const auto u = FxWatchValidate(e, { Chain({ "urn:comp", "urn:eq" }) });
        CHECK(u.size() == 1 && u[0].action == FxWatchAction::Rebind);
        if (!u.empty()) CHECK(u[0].newFx == 1);
    }

    // --- the push decision --------------------------------------------------
    {
        // What decides whether an engine frame reaches an editor. Both halves
        // matter and both are invisible when wrong: skip too eagerly and an
        // editor silently stops following automation; push regardless and one
        // redraws 60 times a second for nothing.
        CHECK(FxWatchFrameIsNew(4, 10, 8) == true);    // changed
        CHECK(FxWatchFrameIsNew(4, 8, 8) == false);    // same generation
        CHECK(FxWatchFrameIsNew(0, 10, 8) == false);   // nothing published
        CHECK(FxWatchFrameIsNew(-1, 10, 8) == false);  // never published
        // A re-registered or rebound editor has never seen a frame, so its
        // cached generation is made unreachable -- the engine's counter only
        // ever takes even values, and this one is odd.
        CHECK(FxWatch::kForcePush == 0xFFFFFFFFu);
        CHECK((FxWatch::kForcePush & 1u) == 1u);
        CHECK(FxWatchFrameIsNew(4, 2, FxWatch::kForcePush) == true);
    }

    std::printf("fx_watch_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
