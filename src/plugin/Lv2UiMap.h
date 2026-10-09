// Lv2UiMap — the host-side rules for driving a plugin's own GUI, as plain data
// and pure functions.
//
// All of it lives outside src/ui on purpose: the window that uses these rules is
// Haiku-only and cannot be built (let alone tested) on the Linux host, but the
// rules themselves are where the bugs are -- a slot numbered differently from
// the insert's parameter order moves the wrong knob, and one that an engine
// frame overwrites mid-drag makes the knob jump back. So the numbering and the
// apply policy are here, tested on the host, and the window only wires them to
// a real UI.
#pragma once

#include <cstddef>
#include <map>
#include <vector>

namespace daw {

// Port index -> the insert's parameter slot, or -1 for a port that is not one of
// this insert's control inputs.
//
// EffectDesc.params -- and so kMsgFxLive's "slot" -- numbers the control INPUT
// ports in port order. Lv2Host builds the insert's parameter list in exactly
// that order, so both sides must derive the numbering the same way or a knob
// moves a different parameter than the one it is labelled with.
inline std::vector<int> Lv2UiSlotsForPorts(const std::vector<bool>& isCtrlIn) {
    std::vector<int> slotOfPort(isCtrlIn.size(), -1);
    int slot = 0;
    for (std::size_t i = 0; i < isCtrlIn.size(); i++)
        if (isCtrlIn[i]) slotOfPort[i] = slot++;
    return slotOfPort;
}

// The inverse, for values arriving the other way (engine -> GUI). Sized to match
// `slotOfPort` (a slot number is never larger than the port count).
inline std::vector<int> Lv2UiPortsForSlots(const std::vector<int>& slotOfPort) {
    std::vector<int> portOfSlot(slotOfPort.size(), -1);
    for (std::size_t i = 0; i < slotOfPort.size(); i++) {
        const int slot = slotOfPort[i];
        if (slot >= 0 && (std::size_t)slot < portOfSlot.size())
            portOfSlot[(std::size_t)slot] = (int)i;
    }
    return portOfSlot;
}

// One value to hand the plugin's GUI as port_event.
struct Lv2UiPortEvent {
    int   port  = -1;
    float value = 0.0f;
};

// Decide what a frame of engine-published values means for the plugin's GUI, and
// apply the bookkeeping that keeps the two directions from echoing each other.
//
// `applied` (slot -> the value the GUI was last told), `ctl` (port -> the port
// buffer of the instance this window owns) and `lastSeen` (port -> the value the
// direct-access poll last sent) are updated in place. `pending` is what the user
// is STILL dragging: those slots are left alone, because the engine's frame is
// at best the value they have already moved past, and applying it would snap
// their knob back mid-gesture.
inline std::vector<Lv2UiPortEvent> Lv2UiPlanApply(
    const std::map<int, float>& inbound,      // slot -> value, from the engine
    const std::vector<int>&     portOfSlot,
    const std::map<int, float>& pending,      // slot -> value, gesture in flight
    std::vector<float>&         applied,      // slot -> last value applied
    std::vector<float>&         ctl,          // port -> port buffer
    std::vector<float>&         lastSeen)     // port -> poll's last sent value
{
    std::vector<Lv2UiPortEvent> out;
    for (const auto& e : inbound) {
        const int slot = e.first;
        if (slot < 0 || (std::size_t)slot >= portOfSlot.size()) continue;
        const int port = portOfSlot[(std::size_t)slot];
        if (port < 0) continue;                       // not one of our parameters
        if (pending.count(slot)) continue;            // the user owns it right now
        const float v = e.second;
        if ((std::size_t)slot < applied.size()) {
            if (applied[(std::size_t)slot] == v) continue;   // already showing it
            applied[(std::size_t)slot] = v;
        }
        // Keep our own instance in step, and tell the direct-access poll not to
        // read the engine's own value back as a user edit.
        if ((std::size_t)port < ctl.size())      ctl[(std::size_t)port] = v;
        if ((std::size_t)port < lastSeen.size()) lastSeen[(std::size_t)port] = v;
        out.push_back({ port, v });
    }
    return out;
}

} // namespace daw
