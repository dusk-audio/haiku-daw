#include "MidiRecorder.h"

namespace daw {

void MidiRecorder::Begin(Frame startFrame) {
    fStart = startFrame;
    fOpen.clear();
    fNotes.clear();
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
    if (e.type != MidiEvent::kNoteOn && e.type != MidiEvent::kNoteOff)
        return;   // v1 records notes only

    const int key = Key(e.channel, e.data1);

    if (e.IsNoteOff()) {
        CloseNote(key, frameNow);
        return;
    }

    // A note-on for a key already held is a retrigger: end the old note here,
    // then open the new one so the two don't merge.
    if (fOpen.count(key)) CloseNote(key, frameNow);

    Frame rel = frameNow - fStart;
    if (rel < 0) rel = 0;   // event before the take start clamps to 0
    fOpen[key] = Open{ e.data1, e.data2, rel };
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
    fNotes.clear();
    return clip;
}

} // namespace daw
