#!/bin/sh
# Probe Midi Kit 2 on this Haiku image before building external MIDI support.
# Same discipline as record_probe.sh: compile tiny probes with the real kit and
# run them; never assume an API works.
#
# Run inside the desktop session (needs the midi_server):
#   cd ~/haiku-daw && git pull && sh scripts/midi_probe.sh 2>&1 | tail -80

TMP=$(mktemp -d /tmp/midiprobe.XXXXXX)

echo "== 0. Haiku version =="
uname -a
echo
echo "== 1. midi_server running? =="
ps 2>/dev/null | grep -i midi_server | grep -v grep || echo "  midi_server NOT in ps"

echo
echo "== 2. Midi Kit 2 headers present? =="
for h in MidiRoster.h MidiEndpoint.h MidiProducer.h MidiConsumer.h; do
    f="/boot/system/develop/headers/os/midi2/$h"
    if [ -f "$f" ]; then echo "  yes  midi2/$h"; else echo "  NO   midi2/$h"; fi
done
# The old Midi Kit 1 soft-synth (BMidiSynth / be_synth) lives in midi/.
for h in MidiSynth.h Synth.h; do
    f="/boot/system/develop/headers/os/midi/$h"
    if [ -f "$f" ]; then echo "  yes  midi/$h (soft-synth)"; else echo "  NO   midi/$h"; fi
done

# --- Probe A: enumerate endpoints on the system MIDI graph ------------------
cat > "$TMP/probe_list.cpp" <<'EOF'
#include <MidiRoster.h>
#include <MidiEndpoint.h>
#include <cstdio>

int main() {
    int32 id = 0;
    BMidiEndpoint* e;
    int n = 0;
    while ((e = BMidiRoster::NextEndpoint(&id)) != NULL) {
        printf("  [%d] id=%d name='%s' producer=%d consumer=%d\n",
               n++, (int)e->ID(), e->Name(),
               e->IsProducer() ? 1 : 0, e->IsConsumer() ? 1 : 0);
        e->Release();
    }
    if (n == 0) printf("  (no endpoints registered)\n");
    return 0;
}
EOF

echo
echo "== 3. compile + run endpoint enumeration =="
if g++ "$TMP/probe_list.cpp" -o "$TMP/probe_list" -lbe -lmidi2 2>"$TMP/errA"; then
    "$TMP/probe_list"
else
    echo "  COMPILE FAILED:"; cat "$TMP/errA"
fi

# --- Probe B: local producer -> local consumer loopback ---------------------
cat > "$TMP/probe_loop.cpp" <<'EOF'
#include <MidiProducer.h>
#include <MidiConsumer.h>
#include <OS.h>
#include <cstdio>
#include <atomic>

static std::atomic<int> g_notes{0};

class ProbeConsumer : public BMidiLocalConsumer {
public:
    ProbeConsumer() : BMidiLocalConsumer("probe_in") {}
    void NoteOn(uchar, uchar note, uchar vel, bigtime_t) {
        (void)note; (void)vel;
        g_notes.fetch_add(1);
    }
};

int main() {
    BMidiLocalProducer* prod = new BMidiLocalProducer("probe_out");
    ProbeConsumer*      cons = new ProbeConsumer();
    printf("producer Register: %s\n", strerror(prod->Register()));
    printf("consumer Register: %s\n", strerror(cons->Register()));
    printf("Connect: %s\n", strerror(prod->Connect(cons)));

    prod->SprayNoteOn(0, 60, 100, system_time());
    prod->SprayNoteOn(0, 64, 100, system_time());
    snooze(200000);   // let it deliver

    printf("consumer received %d note-ons (expect 2)\n", g_notes.load());

    prod->Disconnect(cons);
    cons->Unregister();
    prod->Unregister();
    cons->Release();
    prod->Release();
    return 0;
}
EOF

echo
echo "== 4. compile + run producer->consumer loopback =="
if g++ "$TMP/probe_loop.cpp" -o "$TMP/probe_loop" -lbe -lmidi2 2>"$TMP/errB"; then
    "$TMP/probe_loop"
else
    echo "  COMPILE FAILED:"; cat "$TMP/errB"
fi

echo
echo "== 5. verdict hint =="
echo "  - Loopback delivering 2 note-ons -> Midi Kit 2 works for our own"
echo "    producer/consumer nodes (record from a keyboard producer, send to"
echo "    a synth/hardware consumer)."
echo "  - Endpoints list shows any hardware ports / system synth to connect to."
echo "  - If midi/MidiSynth.h exists, an internal General-MIDI soft-synth is"
echo "    available (libmidi) as a consumer to hear MIDI with no external gear."

rm -rf "$TMP"
