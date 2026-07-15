#include "MidiPort.h"

#include <MidiRoster.h>
#include <MidiEndpoint.h>
#include <OS.h>

namespace daw {

// ---------------------------------------------------------------------------
// Endpoint enumeration
// ---------------------------------------------------------------------------
std::vector<MidiEndpointInfo> EnumerateMidiEndpoints() {
    std::vector<MidiEndpointInfo> out;
    int32 id = 0;
    BMidiEndpoint* e;
    while ((e = BMidiRoster::NextEndpoint(&id)) != nullptr) {
        MidiEndpointInfo info;
        info.id         = e->ID();
        info.name       = e->Name() ? e->Name() : "";
        info.isProducer = e->IsProducer();
        info.isConsumer = e->IsConsumer();
        out.push_back(std::move(info));
        e->Release();
    }
    return out;
}

// ---------------------------------------------------------------------------
// MidiInputPort — a local consumer whose delivery hooks queue into a ring.
// ---------------------------------------------------------------------------
class MidiInputPort::Consumer : public BMidiLocalConsumer {
public:
    Consumer(const char* name, MidiEventRing* ring)
        : BMidiLocalConsumer(name), fRing(ring) {}

    // These hooks run on a Midi Kit delivery thread. Pushing to the SPSC ring
    // is the lock-free handoff to the app thread; a full ring drops the event.
    void NoteOn(uchar ch, uchar note, uchar vel, bigtime_t t) override {
        fRing->Push(MidiEvent::NoteOn(ch, note, vel, (int64_t)t));
    }
    void NoteOff(uchar ch, uchar note, uchar vel, bigtime_t t) override {
        fRing->Push(MidiEvent::NoteOff(ch, note, vel, (int64_t)t));
    }
    void ControlChange(uchar ch, uchar cc, uchar val, bigtime_t t) override {
        fRing->Push(MidiEvent::ControlChange(ch, cc, val, (int64_t)t));
    }
    void PitchBend(uchar ch, uchar lsb, uchar msb, bigtime_t t) override {
        MidiEvent e;
        e.type    = MidiEvent::kPitchBend;
        e.channel = ch;
        e.bend    = (int16_t)((((int)msb << 7) | (int)lsb) - 8192);
        e.timeUs  = (int64_t)t;
        fRing->Push(e);
    }

private:
    MidiEventRing* fRing;
};

MidiInputPort::MidiInputPort(const char* name) {
    fConsumer = new Consumer(name, &fRing);
}

MidiInputPort::~MidiInputPort() {
    if (fConsumer) {
        fConsumer->Unregister();
        fConsumer->Release();   // drop our construction reference
    }
}

status_t MidiInputPort::Register()   { return fConsumer->Register(); }
void     MidiInputPort::Unregister() { fConsumer->Unregister(); }
int32    MidiInputPort::Id() const   { return fConsumer->ID(); }

BMidiLocalConsumer* MidiInputPort::KitConsumer() const { return fConsumer; }

status_t MidiInputPort::ConnectFrom(int32 producerId) {
    BMidiProducer* p = BMidiRoster::FindProducer(producerId);
    if (!p) return B_ENTRY_NOT_FOUND;
    status_t r = p->Connect(fConsumer);
    p->Release();
    return r;
}

// ---------------------------------------------------------------------------
// MidiOutputPort — a local producer wrapped as an IMidiOutput.
// ---------------------------------------------------------------------------
MidiOutputPort::MidiOutputPort(const char* name) {
    fProducer = new BMidiLocalProducer(name);
}

MidiOutputPort::~MidiOutputPort() {
    if (fProducer) {
        fProducer->Unregister();
        fProducer->Release();
    }
}

status_t MidiOutputPort::Register()   { return fProducer->Register(); }
void     MidiOutputPort::Unregister() { fProducer->Unregister(); }
int32    MidiOutputPort::Id() const   { return fProducer->ID(); }

status_t MidiOutputPort::ConnectTo(int32 consumerId) {
    BMidiConsumer* c = BMidiRoster::FindConsumer(consumerId);
    if (!c) return B_ENTRY_NOT_FOUND;
    status_t r = fProducer->Connect(c);
    c->Release();
    return r;
}

status_t MidiOutputPort::ConnectTo(BMidiLocalConsumer* c) {
    if (!c) return B_BAD_VALUE;
    return fProducer->Connect(c);
}

void MidiOutputPort::Send(const MidiEvent& e) {
    const bigtime_t t = e.timeUs ? (bigtime_t)e.timeUs : system_time();
    switch (e.type) {
        case MidiEvent::kNoteOn:
            if (e.data2) fProducer->SprayNoteOn(e.channel, e.data1, e.data2, t);
            else         fProducer->SprayNoteOff(e.channel, e.data1, 0, t);
            break;
        case MidiEvent::kNoteOff:
            fProducer->SprayNoteOff(e.channel, e.data1, e.data2, t);
            break;
        case MidiEvent::kControlChange:
            fProducer->SprayControlChange(e.channel, e.data1, e.data2, t);
            break;
        case MidiEvent::kPitchBend: {
            const int v = e.bend + 8192;                 // 0..16383
            fProducer->SprayPitchBend(e.channel, (uchar)(v & 0x7f),
                                      (uchar)((v >> 7) & 0x7f), t);
            break;
        }
        default:
            break;
    }
}

} // namespace daw
