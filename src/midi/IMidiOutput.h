// IMidiOutput — a sink for MIDI events (a hardware synth, an external port, or
// another app's consumer). The Midi Kit 2 adapter (MidiOutputPort) implements
// it by spraying to a BMidiLocalProducer. Kept kit-free so the sequencer /
// playback side can emit events without depending on the Midi Kit types.
#pragma once

#include "MidiEvent.h"

namespace daw {

class IMidiOutput {
public:
    virtual ~IMidiOutput() = default;

    // Send one event to the sink. May be called from a scheduling thread; the
    // implementation is responsible for its own delivery timing (the Midi Kit
    // timestamps the spray).
    virtual void Send(const MidiEvent& e) = 0;
};

} // namespace daw
