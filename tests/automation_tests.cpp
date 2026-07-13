// Host-buildable tests for AutomationLane: empty/default, single point hold,
// two-point linear interpolation + end clamping, out-of-order insertion,
// overwrite at a duplicate frame, and point removal.

#include "../src/model/Automation.h"

#include <cmath>
#include <cstdio>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    // Empty lane returns the supplied default everywhere.
    {
        AutomationLane lane;
        CHECK(lane.Count() == 0);
        CHECK(near(lane.ValueAt(0, 0.25f), 0.25f));
        CHECK(near(lane.ValueAt(100000, -1.0f), -1.0f));
    }

    // Single point: that value everywhere (before, at, after).
    {
        AutomationLane lane;
        lane.AddPoint(1000, 0.5f);
        CHECK(lane.Count() == 1);
        CHECK(near(lane.ValueAt(0, 0.0f), 0.5f));
        CHECK(near(lane.ValueAt(1000, 0.0f), 0.5f));
        CHECK(near(lane.ValueAt(5000, 0.0f), 0.5f));
    }

    // Two points: linear interpolation at the midpoint, clamp before/after.
    {
        AutomationLane lane;
        lane.AddPoint(0, 0.0f);
        lane.AddPoint(1000, 1.0f);
        CHECK(near(lane.ValueAt(-500, 9.0f), 0.0f)); // before first -> hold
        CHECK(near(lane.ValueAt(0, 9.0f), 0.0f));
        CHECK(near(lane.ValueAt(250, 9.0f), 0.25f));
        CHECK(near(lane.ValueAt(500, 9.0f), 0.5f));  // midpoint
        CHECK(near(lane.ValueAt(750, 9.0f), 0.75f));
        CHECK(near(lane.ValueAt(1000, 9.0f), 1.0f));
        CHECK(near(lane.ValueAt(9999, 9.0f), 1.0f)); // after last -> hold
    }

    // Out-of-order insertion still yields sorted access and correct interp.
    {
        AutomationLane lane;
        lane.AddPoint(2000, 2.0f);
        lane.AddPoint(0,    0.0f);
        lane.AddPoint(1000, 1.0f);
        CHECK(lane.Count() == 3);
        CHECK(lane.At(0).frame == 0    && near(lane.At(0).value, 0.0f));
        CHECK(lane.At(1).frame == 1000 && near(lane.At(1).value, 1.0f));
        CHECK(lane.At(2).frame == 2000 && near(lane.At(2).value, 2.0f));
        CHECK(near(lane.ValueAt(500, 9.0f), 0.5f));
        CHECK(near(lane.ValueAt(1500, 9.0f), 1.5f));
    }

    // Overwriting a point at an existing frame replaces its value, no dup.
    {
        AutomationLane lane;
        lane.AddPoint(1000, 0.2f);
        lane.AddPoint(1000, 0.8f);
        CHECK(lane.Count() == 1);
        CHECK(near(lane.At(0).value, 0.8f));
        CHECK(near(lane.ValueAt(1000, 0.0f), 0.8f));
    }

    // RemovePoint: out-of-range false, in-range removes and keeps order.
    {
        AutomationLane lane;
        lane.AddPoint(0,    0.0f);
        lane.AddPoint(1000, 1.0f);
        lane.AddPoint(2000, 2.0f);
        CHECK(lane.RemovePoint(5) == false);
        CHECK(lane.RemovePoint(1) == true);   // remove the middle point
        CHECK(lane.Count() == 2);
        CHECK(lane.At(0).frame == 0);
        CHECK(lane.At(1).frame == 2000);
        // With the middle gone, 1000 now interpolates 0..2 over 0..2000 -> 1.0.
        CHECK(near(lane.ValueAt(1000, 9.0f), 1.0f));

        CHECK(lane.RemovePoint(0) == true);
        CHECK(lane.RemovePoint(0) == true);
        CHECK(lane.Count() == 0);
        CHECK(lane.RemovePoint(0) == false);
    }

    // Clear empties the lane.
    {
        AutomationLane lane;
        lane.AddPoint(0, 1.0f);
        lane.AddPoint(100, 2.0f);
        lane.Clear();
        CHECK(lane.Count() == 0);
        CHECK(near(lane.ValueAt(50, 7.0f), 7.0f));
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
