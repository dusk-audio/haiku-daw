// MidiPort — the Midi Kit 2 adapter: the thin Haiku-only edge that bridges the
// system MIDI graph to the kit-free MidiEvent world below it.
//
// MidiInputPort is a BMidiLocalConsumer that queues every incoming event into a
// lock-free ring (the delivery hooks run on a Midi Kit thread; draining happens
// on the app thread) and exposes them through the kit-free IMidiInput. Connect
// a hardware keyboard's producer endpoint (e.g. /dev/midi/usb/0-0, published by
// midi_server) to it and its notes flow in.
//
// MidiOutputPort is a BMidiLocalProducer wrapped as an IMidiOutput: Send() maps
// a MidiEvent to the matching Spray*, so the sequencer can drive a hardware
// synth or the OS soft-synth without touching the Midi Kit.
//
// Haiku-only: depends on the Midi Kit 2 (libmidi2). The types above the
// IMidiInput/IMidiOutput line stay kit-free and host-testable.
#pragma once

#include "IMidiInput.h"
#include "IMidiOutput.h"
#include "MidiEventRing.h"

#include <MidiConsumer.h>
#include <MidiProducer.h>

#include <string>
#include <vector>

namespace daw {

// A discoverable endpoint on the system MIDI graph (hardware port, soft-synth,
// or another app). Producers emit MIDI; consumers receive it.
struct MidiEndpointInfo {
    int32       id         = 0;
    std::string name;
    bool        isProducer = false;
    bool        isConsumer = false;
};

// Snapshot every registered endpoint (requires midi_server running). Hardware
// keyboards appear as producers; drive their id into MidiInputPort::ConnectFrom.
std::vector<MidiEndpointInfo> EnumerateMidiEndpoints();

class MidiInputPort : public IMidiInput {
public:
    explicit MidiInputPort(const char* name = "HaikuDAW In");
    ~MidiInputPort() override;

    status_t Register();      // publish this consumer to the system graph
    void     Unregister();
    int32    Id() const;

    // Connect a producer endpoint (a keyboard/hardware port) to this consumer.
    status_t ConnectFrom(int32 producerId);

    // IMidiInput — drain queued events (app thread).
    std::size_t ReadEvents(MidiEvent* dst, std::size_t max) override {
        return fRing.Read(dst, max);
    }

    // Underlying kit consumer, so a local producer can Connect() directly
    // (loopback / tests) without going through the roster.
    BMidiLocalConsumer* KitConsumer() const;

private:
    class Consumer;                 // BMidiLocalConsumer subclass (in the .cpp)
    Consumer*     fConsumer = nullptr;
    MidiEventRing fRing{1024};
};

class MidiOutputPort : public IMidiOutput {
public:
    explicit MidiOutputPort(const char* name = "HaikuDAW Out");
    ~MidiOutputPort() override;

    status_t Register();
    void     Unregister();
    int32    Id() const;

    status_t ConnectTo(int32 consumerId);           // via the roster
    status_t ConnectTo(BMidiLocalConsumer* c);       // direct (loopback/tests)

    // IMidiOutput — spray one event. Unstamped events (timeUs == 0) go out now.
    void Send(const MidiEvent& e) override;

    BMidiLocalProducer* KitProducer() const { return fProducer; }

private:
    BMidiLocalProducer* fProducer = nullptr;
};

} // namespace daw
