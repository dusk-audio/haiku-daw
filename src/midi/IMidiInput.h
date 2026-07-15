// IMidiInput — a live source of MIDI events (a keyboard, a hardware port, or a
// loopback producer). The Midi Kit 2 adapter (MidiInputPort) implements it; the
// app / recorder drains it. Kept as a tiny interface so consumers don't depend
// on the Midi Kit types — mirrors IMonitorSource for audio input.
#pragma once

#include "MidiEvent.h"

#include <cstddef>

namespace daw {

class IMidiInput {
public:
    virtual ~IMidiInput() = default;

    // Drain up to `max` events into `dst`, returning the count actually copied
    // (0 when nothing is pending). Must be lock-free / allocation-free: called
    // off a polling timer, but the underlying handoff is a real-time ring.
    virtual std::size_t ReadEvents(MidiEvent* dst, std::size_t max) = 0;
};

} // namespace daw
