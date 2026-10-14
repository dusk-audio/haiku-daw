// Host tests for the kit-free rules the UI's refresh and identity bookkeeping
// rests on (package 52):
//
//   * src/app/UiSync.h      — the stale-window identity rule and the
//                             strip-payload refresh rule;
//   * src/model/ParamDisplay.h — a parameter's value as text (the scale point's
//                             name, the unit a port name declares).
//
// Each of these exists because the bug beside it was invisible: a panel left on
// a track id from the previous project writes the wrong session's data, and a
// rack left on a stale layout either drops every edit in silence or draws a
// track that is gone. The commands are real ones (AddTrackCommand and friends),
// so the layout rules are tested against what the app actually runs.

#include "../src/app/UiSync.h"
#include "../src/model/Commands.h"
#include "../src/model/ParamDisplay.h"
#include "../src/model/Project.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static void TestProjectScope() {
    std::printf("project scope\n");
    ProjectScope scope;
    const ProjectScope::Generation opened = scope.Current();
    // A window that opened against this project is current ...
    CHECK(!scope.Stale(opened));
    // ... until the project is replaced ...
    const ProjectScope::Generation next = scope.Bump();
    CHECK(next != opened);
    CHECK(scope.Stale(opened));
    // ... and a window opened AFTER the replacement is not stale: that is the
    // whole reason to ask rather than to close by window kind.
    CHECK(!scope.Stale(next));
    // Bumping twice keeps a window from the first project stale -- two Opens
    // without a window being reopened must not make it look current again.
    scope.Bump();
    CHECK(scope.Stale(opened));
    CHECK(scope.Stale(next));
    // The default is not "current": a record nobody wrote must read as stale
    // rather than as belonging to whatever project happens to be open.
    CHECK(scope.Stale(0));
    ProjectScope fresh;
    CHECK(!fresh.Stale(fresh.Current()));
    CHECK(fresh.Current() != 0);
}

// One plain audio track on a project, the way every other host test builds
// one (the UI suite's own helper needs a window and a lock).
static uint64_t AddTrackNamed(Project& p, const char* name) {
    Track t;
    t.id = p.NextTrackId();
    t.type = TrackType::Audio;
    t.name = name;
    p.AddTrack(t);
    return (uint64_t)t.id;
}

static void TestTrackLayout() {
    std::printf("track layout\n");
    Project p;
    AddTrackNamed(p, "one");
    AddTrackNamed(p, "two");
    AddTrackNamed(p, "three");

    const std::vector<uint64_t> layout = TrackLayoutOf(p);
    CHECK(layout.size() == 3);
    // The console shows the model's layout: no drift, in any order-insensitive
    // sense either.
    CHECK(TrackLayoutMatches(layout, layout));
    CHECK(TrackLayoutDrift(layout, TrackLayoutOf(p)) == LayoutDrift::None);

    // --- a track added: a strip that does not exist yet ---
    const uint64_t added = AddTrackNamed(p, "four");
    CHECK(TrackLayoutDrift(layout, TrackLayoutOf(p)) == LayoutDrift::TrackAdded);
    CHECK(!TrackLayoutMatches(layout, TrackLayoutOf(p)));
    const std::vector<uint64_t> afterAdd = TrackLayoutOf(p);
    CHECK(afterAdd.back() == added);

    // --- a track removed: a live-looking strip whose edits go nowhere ---
    CHECK(p.RemoveTrack((TrackId)afterAdd[1]));
    CHECK(TrackLayoutDrift(afterAdd, TrackLayoutOf(p))
          == LayoutDrift::TrackRemoved);

    // --- reordered: same ids, different order (a reorder is NOT "no drift") ---
    {
        Project q;
        const uint64_t a = AddTrackNamed(q, "a");
        const uint64_t b = AddTrackNamed(q, "b");
        std::vector<uint64_t> shown = TrackLayoutOf(q);
        CommandStack stack;
        stack.Execute(std::make_unique<MoveTrackCommand>((TrackId)b, -1), q);
        const std::vector<uint64_t> model = TrackLayoutOf(q);
        CHECK(shown.size() == model.size());
        CHECK(TrackLayoutDrift(shown, model) == LayoutDrift::Reordered);
        CHECK(!TrackLayoutMatches(shown, model));
        // Both ids are still there -- which is what makes it a reorder and not
        // an add/remove: the ids alone cannot tell a rack in the wrong order.
        CHECK(shown[0] == a && shown[1] == b);
        CHECK(model[0] == b && model[1] == a);
    }

    // --- a duplicate adds a track: the rack has to grow a strip ---
    {
        Project q;
        AddTrackNamed(q, "src");
        const std::vector<uint64_t> shown = TrackLayoutOf(q);
        Track dup;
        dup.id = q.NextTrackId();
        dup.type = TrackType::Audio;
        dup.name = "src copy";
        q.AddTrack(dup);        CHECK(TrackLayoutDrift(shown, TrackLayoutOf(q)) == LayoutDrift::TrackAdded);
    }

    // --- both at once reads as the removal: it is the one the user cannot see ---
    {
        const std::vector<uint64_t> shown = { 10, 11 };
        const std::vector<uint64_t> model = { 11, 12 };
        CHECK(TrackLayoutDrift(shown, model) == LayoutDrift::TrackRemoved);
    }
    // An empty model (a New project) against a full rack is a removal of all.
    CHECK(TrackLayoutDrift({ 1, 2 }, {}) == LayoutDrift::TrackRemoved);
    CHECK(TrackLayoutDrift({}, { 1 }) == LayoutDrift::TrackAdded);
    CHECK(TrackLayoutDrift({}, {}) == LayoutDrift::None);
}

static void TestParamText() {
    std::printf("parameter text\n");
    // The unit a port name declares: the LAST bracketed group, and only when it
    // plausibly IS a unit.
    CHECK(ParamUnitFromName("Frequency (Hz)") == "Hz");
    CHECK(ParamUnitFromName("Threshold (dB)") == "dB");
    CHECK(ParamUnitFromName("Attack (ms)") == "ms");
    CHECK(ParamUnitFromName("Release (s)") == "s");
    CHECK(ParamUnitFromName("Wet/Dry (%)") == "%");
    CHECK(ParamUnitFromName("Gain (L/R) (dB)") == "dB");   // the last group wins
    CHECK(ParamUnitFromName("Enabled") == "");
    // A short bracketed word IS taken as the unit: the convention says the
    // bracket holds one, and nothing can tell "left" from "Hz" from the text
    // alone. What is refused is a bracket that cannot be a unit at all.
    CHECK(ParamUnitFromName("Input (left)") == "left");
    CHECK(ParamUnitFromName("Notes (a long parenthetical)") == "");
    CHECK(ParamUnitFromName("(Hz)") == "Hz");
    CHECK(ParamUnitFromName("Bad ()") == "");
    CHECK(ParamUnitFromName("Unclosed (Hz") == "");
    CHECK(ParamUnitFromName("") == "");

    // Which scale point a value lands on, with the tolerance a float round trip
    // needs -- and nothing for a value that is not one.
    const std::vector<float> pts = { 0.0f, 2.0f, 7.0f };
    CHECK(ScalePointIndex(pts, 0.0f) == 0);
    CHECK(ScalePointIndex(pts, 7.0f) == 2);
    CHECK(ScalePointIndex(pts, 2.0001f) == 1);
    CHECK(ScalePointIndex(pts, 3.0f) == -1);
    CHECK(ScalePointIndex(pts, -1.0f) == -1);
    CHECK(ScalePointIndex(pts, std::nanf("")) == -1);
    CHECK(ScalePointIndex({}, 0.0f) == -1);

    // The number itself: whole for an integral domain or a wide range, two
    // decimals otherwise -- the rule the panel already had.
    CHECK(FormatParamNumber(2.0f, true, 0.0f, 10.0f) == "2");
    CHECK(FormatParamNumber(440.0f, false, 20.0f, 20000.0f) == "440");
    CHECK(FormatParamNumber(0.5f, false, 0.0f, 1.0f) == "0.50");
    CHECK(FormatParamNumber(-6.25f, false, -60.0f, 0.0f) == "-6.25");
    CHECK(FormatParamNumber(std::nanf(""), false, 0.0f, 1.0f) == "--");

    // The whole readout: the scale point's own name when there is one, the
    // number + unit otherwise -- and never both.
    const std::vector<std::string> names = { "Lowpass", "Highpass", "Bandpass" };
    CHECK(ParamValueText(7.0f, true, 0.0f, 10.0f, "", &pts, &names) == "Bandpass");
    CHECK(ParamValueText(2.0f, true, 0.0f, 10.0f, "", &pts, &names) == "Highpass");
    // A value between the points is not a name: it is a number.
    CHECK(ParamValueText(3.0f, true, 0.0f, 10.0f, "", &pts, &names) == "3");
    // A name list that does not line up must not be indexed past its end.
    const std::vector<std::string> shortNames = { "Lowpass" };
    CHECK(ParamValueText(7.0f, true, 0.0f, 10.0f, "", &pts, &shortNames) == "7");
    // No names at all (an add-on, or an enumeration natively unnamed).
    CHECK(ParamValueText(2.0f, true, 0.0f, 10.0f, "", &pts, nullptr) == "2");
    // The unit: appended to a number, never to a scale-point name.
    CHECK(ParamValueText(440.0f, false, 20.0f, 20000.0f, "Hz", nullptr, nullptr)
          == "440 Hz");
    CHECK(ParamValueText(2.0f, true, 0.0f, 10.0f, "dB", &pts, &names)
          == "Highpass");
    CHECK(ParamValueText(0.5f, false, 0.0f, 1.0f, "", nullptr, nullptr) == "0.50");
}

int main() {
    TestProjectScope();
    TestTrackLayout();
    TestParamText();
    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
