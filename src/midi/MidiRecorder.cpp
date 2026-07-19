#include "MidiRecorder.h"

#include <algorithm>   // sort (loop-take event split)
#include <map>

namespace daw {

void MidiRecorder::Begin(Frame startFrame) {
    fStart = startFrame;
    fOpen.clear();
    fNotes.clear();
    fEvents.clear();
    // Drop the dedupe baseline too: a new take must record the controller's
    // opening value, even when it is the same one the previous take ended on.
    fLastCc.clear();
    fLastBend = -1;
}

void MidiRecorder::CloseNote(int key, Frame endFrame) {
    auto it = fOpen.find(key);
    if (it == fOpen.end()) return;
    const Open& o = it->second;
    Frame len = endFrame - (fStart + o.start);
    if (len < 1) len = 1;   // a zero-length note is inaudible; keep it minimal
    fNotes.push_back(MidiNote{ (int)o.pitch, (int)o.velocity, o.start, len });
    fOpen.erase(it);
}

void MidiRecorder::OnEvent(const MidiEvent& e, Frame frameNow) {
    Frame rel0 = frameNow - fStart;
    if (rel0 < 0) rel0 = 0;   // event before the take start clamps to 0

    // Continuous controllers become the clip's controller events, so a volume
    // or expression move played live lands in the CC lane alongside the notes.
    if (e.type == MidiEvent::kControlChange) {
        // Keyed by controller number only: MidiClipEvent has no channel, so
        // two channels moving the same controller are one lane downstream.
        const int k = (int)e.data1;
        const auto it = fLastCc.find(k);
        if (it != fLastCc.end() && it->second == (int)e.data2)
            return;                     // same value already in force
        fLastCc[k] = (int)e.data2;
        MidiClipEvent ce;
        ce.type       = MidiClipEvent::CC;
        ce.startFrame = rel0;
        ce.data       = (int)e.data1;
        ce.value      = (int)e.data2;
        fEvents.push_back(ce);
        return;
    }
    if (e.type == MidiEvent::kPitchBend) {
        // MidiEvent carries bend signed (-8192..8191); the model stores the raw
        // 14-bit value with 8192 as centre.
        const int v = (int)e.bend + 8192;
        if (fLastBend == v) return;     // same bend already in force
        fLastBend = v;
        MidiClipEvent ce;
        ce.type       = MidiClipEvent::PitchBend;
        ce.startFrame = rel0;
        ce.value      = v;
        fEvents.push_back(ce);
        return;
    }
    if (e.type != MidiEvent::kNoteOn && e.type != MidiEvent::kNoteOff)
        return;   // program change / pressure / sysex not captured

    const int key = Key(e.channel, e.data1);

    if (e.IsNoteOff()) {
        CloseNote(key, frameNow);
        return;
    }

    // A note-on for a key already held is a retrigger: end the old note here,
    // then open the new one so the two don't merge.
    if (fOpen.count(key)) CloseNote(key, frameNow);

    fOpen[key] = Open{ e.data1, e.data2, rel0 };
}

std::vector<std::vector<MidiNote>> SplitMidiLoopTakes(
        const std::vector<MidiNote>& notes, Frame loopLen) {
    std::vector<std::vector<MidiNote>> takes;
    if (loopLen <= 0) { takes.push_back(notes); return takes; }
    for (const MidiNote& n : notes) {
        Frame s = n.startFrame < 0 ? 0 : n.startFrame;
        const int pass = (int)(s / loopLen);
        if ((int)takes.size() <= pass) takes.resize((size_t)pass + 1);
        MidiNote rn = n;
        rn.startFrame = s - (Frame)pass * loopLen;   // re-base to the loop start
        takes[(size_t)pass].push_back(rn);
    }
    return takes;
}

std::vector<std::vector<MidiClipEvent>> SplitMidiLoopEvents(
        const std::vector<MidiClipEvent>& events, Frame loopLen, int passes) {
    std::vector<std::vector<MidiClipEvent>> out;
    if (loopLen <= 0 || passes <= 1) { out.push_back(events); return out; }
    out.resize((size_t)passes);

    // Deal each event into the pass it was played in, re-based to the loop
    // start. Sorted first so "latest earlier event" below is a simple carry.
    std::vector<MidiClipEvent> sorted = events;
    std::sort(sorted.begin(), sorted.end(),
              [](const MidiClipEvent& a, const MidiClipEvent& b) {
                  return a.startFrame < b.startFrame;
              });

    // Value in force per controller as we cross each pass boundary. Keyed the
    // same way the recorder keys them, with pitch bend in its own space.
    std::map<int, MidiClipEvent> inForce;
    int cur = 0;
    auto seedPass = [&](int pass) {
        for (const auto& kv : inForce) {
            MidiClipEvent e = kv.second;
            e.startFrame = 0;      // it was already in force when this pass began
            out[(size_t)pass].push_back(e);
        }
    };
    for (const MidiClipEvent& e : sorted) {
        Frame s = e.startFrame < 0 ? 0 : e.startFrame;
        int pass = (int)(s / loopLen);
        if (pass >= passes) pass = passes - 1;
        while (cur < pass) { cur++; seedPass(cur); }
        MidiClipEvent re = e;
        re.startFrame = s - (Frame)pass * loopLen;
        out[(size_t)pass].push_back(re);
        // CC keys by controller number (0..127); every other type has no `data`
        // and gets its own slot in a disjoint negative space (bend -2, program
        // -3, pressure -4). Testing only for PitchBend left program change and
        // channel pressure keyed on their unused `data`, i.e. colliding with
        // each other and with CC 0.
        const int key = (e.type == MidiClipEvent::CC) ? e.data : -1 - e.type;
        inForce[key] = e;
    }
    // Passes AFTER the last event still inherit the value in force: the loop
    // above only advances `cur` when an event lands in a later pass, so a take
    // whose final passes played no controllers would otherwise come back empty
    // and those passes would sound at the wrong level.
    while (cur < passes - 1) { cur++; seedPass(cur); }
    return out;
}

std::vector<MidiNote> MidiRecorder::SnapshotNotes(Frame nowFrame) const {
    std::vector<MidiNote> out = fNotes;   // closed notes (clip-relative)
    for (const auto& kv : fOpen) {
        const Open& o = kv.second;
        Frame len = nowFrame - (fStart + o.start);
        if (len < 1) len = 1;
        out.push_back(MidiNote{ (int)o.pitch, (int)o.velocity, o.start, len });
    }
    return out;
}

MidiClip MidiRecorder::End(Frame endFrame) {
    // End all still-held notes at the take boundary. Collect keys first so we
    // don't mutate the map while iterating.
    std::vector<int> held;
    held.reserve(fOpen.size());
    for (const auto& kv : fOpen) held.push_back(kv.first);
    for (int k : held) CloseNote(k, endFrame);

    MidiClip clip;
    clip.startFrame   = fStart;
    clip.lengthFrames = endFrame - fStart;
    if (clip.lengthFrames < 0) clip.lengthFrames = 0;
    clip.notes        = std::move(fNotes);
    clip.events       = std::move(fEvents);
    fNotes.clear();
    fEvents.clear();
    fLastCc.clear();
    fLastBend = -1;
    return clip;
}

} // namespace daw
