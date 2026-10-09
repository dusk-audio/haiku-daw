// Host tests for the rules that drive a plugin's own GUI (src/plugin/Lv2UiMap.h).
//
// These exist because the window that uses them is Haiku-only: the numbering and
// the apply policy were the two places where a live editor could be wrong
// without looking wrong -- a slot numbered differently from the insert's
// parameter order moves a different knob than the one under the mouse, and an
// engine frame applied mid-drag snaps the user's knob back.

#include "../src/plugin/Lv2UiMap.h"

#include <cstdio>
#include <map>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

namespace {

// A plausible port list: audio in/out first, then control inputs interleaved
// with control OUTPUTS (which are not parameters) and an atom port -- exactly
// the shape that makes an off-by-one in the numbering invisible.
//   index: 0=audio in, 1=audio out, 2=ctrl in, 3=ctrl out, 4=ctrl in,
//          5=atom in, 6=ctrl in
std::vector<bool> MixedPorts() {
    return { false, false, true, false, true, false, true };
}

} // namespace

int main() {
    // --- the numbering ------------------------------------------------------
    {
        const std::vector<int> slots = Lv2UiSlotsForPorts(MixedPorts());
        CHECK(slots.size() == 7);
        CHECK(slots[2] == 0);      // first control input is slot 0
        CHECK(slots[4] == 1);
        CHECK(slots[6] == 2);
        CHECK(slots[0] == -1);     // audio in is not a parameter
        CHECK(slots[1] == -1);     // nor is audio out
        CHECK(slots[3] == -1);     // nor a control OUTPUT
        CHECK(slots[5] == -1);     // nor an atom port

        // Control OUTPUTS are simply not control inputs, so they must not
        // consume a slot number: a plugin whose first control port is an output
        // (index 1 here) would otherwise shift every parameter by one --
        // silently, since both are just ints.
        const std::vector<int> leadsWithOut =
            Lv2UiSlotsForPorts({ false /*audio*/, false /*control out*/,
                                 true /*control in*/ });
        CHECK(leadsWithOut[1] == -1);
        CHECK(leadsWithOut[2] == 0);

        // No control inputs at all (a plain audio effect): no slots, no crash.
        const std::vector<int> none = Lv2UiSlotsForPorts({ false, false });
        CHECK(none[0] == -1 && none[1] == -1);

        // And the inverse is a true inverse, ports with no slot included.
        const std::vector<int> back = Lv2UiPortsForSlots(slots);
        CHECK(back.size() == slots.size());
        CHECK(back[0] == 2);
        CHECK(back[1] == 4);
        CHECK(back[2] == 6);
        for (size_t i = 3; i < back.size(); i++) CHECK(back[i] == -1);
    }

    // --- what an engine frame does to the GUI -------------------------------
    const std::vector<int> slotOfPort = Lv2UiSlotsForPorts(MixedPorts());
    const std::vector<int> portOfSlot = Lv2UiPortsForSlots(slotOfPort);

    {
        // A value the GUI is not already showing becomes a port_event, and is
        // written into the port buffer and the poll's last-seen -- otherwise the
        // direct-access poll would read the engine's own value back as a user
        // edit and send it straight back.
        std::vector<float> applied(7, 0.0f), ctl(7, 0.0f), lastSeen(7, 0.0f);
        applied[0] = 1.0f;                       // GUI is showing 1.0 at slot 0
        const auto ev = Lv2UiPlanApply({ { 0, 2.5f } }, portOfSlot, {}, applied,
                                       ctl, lastSeen);
        CHECK(ev.size() == 1);
        if (!ev.empty()) {
            CHECK(ev[0].port == 2);              // slot 0 is port 2
            CHECK(ev[0].value == 2.5f);
        }
        CHECK(applied[0] == 2.5f);
        CHECK(ctl[2] == 2.5f);
        CHECK(lastSeen[2] == 2.5f);
        CHECK(ctl[0] == 0.0f);                   // nothing else touched
    }

    {
        // The same value again is not worth a port_event: a republished frame
        // must not make the plugin's GUI redraw for nothing.
        std::vector<float> applied(7, 2.5f), ctl(7, 0.0f), lastSeen(7, 0.0f);
        const auto ev = Lv2UiPlanApply({ { 0, 2.5f } }, portOfSlot, {}, applied,
                                       ctl, lastSeen);
        CHECK(ev.empty());
        CHECK(ctl[2] == 0.0f);
    }

    {
        // A gesture in flight owns its parameter: the engine's frame is at best
        // the value the user has already moved past, and applying it would snap
        // their knob back. Other parameters in the same frame still apply.
        std::vector<float> applied(7, 0.0f), ctl(7, 0.0f), lastSeen(7, 0.0f);
        std::map<int, float> pending{ { 0, 9.0f } };     // dragging slot 0
        const auto ev = Lv2UiPlanApply({ { 0, 0.5f }, { 1, 0.25f } }, portOfSlot,
                                       pending, applied, ctl, lastSeen);
        CHECK(ev.size() == 1);
        if (!ev.empty()) CHECK(ev[0].port == 4);         // slot 1 only
        CHECK(ctl[2] == 0.0f);                           // slot 0 left alone
        CHECK(lastSeen[2] == 0.0f);                      // ... in every buffer
        CHECK(ctl[4] == 0.25f);
        CHECK(lastSeen[4] == 0.25f);
        CHECK(applied[0] == 0.0f);                       // and not marked applied
    }

    {
        // Slots the plugin does not have, and ports that are not parameters,
        // are dropped rather than written somewhere arbitrary.
        std::vector<float> applied(7, 0.0f), ctl(7, 0.0f), lastSeen(7, 0.0f);
        const auto ev = Lv2UiPlanApply({ { 9, 1.0f }, { -1, 1.0f } }, portOfSlot,
                                       {}, applied, ctl, lastSeen);
        CHECK(ev.empty());
        for (float v : ctl) CHECK(v == 0.0f);

        // A slot whose port is unknown (a truncated inverse map) likewise.
        const std::vector<int> partial = { 0, -1 };
        const auto ev2 = Lv2UiPlanApply({ { 1, 1.0f } }, partial, {}, applied,
                                        ctl, lastSeen);
        CHECK(ev2.empty());
    }

    {
        // A whole frame at once: every parameter moves, in one pass.
        std::vector<float> applied(7, 0.0f), ctl(7, 0.0f), lastSeen(7, 0.0f);
        const auto ev = Lv2UiPlanApply({ { 0, 1.0f }, { 1, 2.0f }, { 2, 3.0f } },
                                       portOfSlot, {}, applied, ctl, lastSeen);
        CHECK(ev.size() == 3);
        CHECK(ctl[2] == 1.0f);
        CHECK(ctl[4] == 2.0f);
        CHECK(ctl[6] == 3.0f);
    }

    std::printf("lv2_ui_map_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
