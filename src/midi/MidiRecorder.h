// MidiRecorder — pairs a live stream of MidiEvents into the model's MidiNotes.
//
// A keyboard sends independent note-on / note-off events, each stamped with the
// timeline frame at which it arrived. This turns that stream into a MidiClip:
// a note-on opens a pending note; the matching note-off (or a note-on with
// velocity 0, or a retrigger of the same key) closes it into a MidiNote. Notes
// are stored clip-relative to the record start, so the finished region drops
// onto a track as an ordinary MidiClip.
//
// Kit-free and host-testable: it consumes plain MidiEvents and explicit frame
// positions, so the pairing logic is exercised without the Midi Kit or the VM.
// The Media-Kit-free analogue of the audio Recorder.
#pragma once

#include "MidiEvent.h"
#include "../model/Project.h"
#include "../model/types.h"

#include <unordered_map>

namespace daw {

class MidiRecorder {
public:
    // Begin a take whose clip starts at `startFrame` on the project timeline.
    // Drops any state from a previous take.
    void Begin(Frame startFrame);

    // Feed one event that arrived at timeline position `frameNow`. Only note
    // on/off affect the take; other messages (CC, bend) are ignored for v1.
    void OnEvent(const MidiEvent& e, Frame frameNow);

    // Close the take at `endFrame`: any notes still held are ended there. The
    // returned MidiClip has its notes clip-relative and lengthFrames set to the
    // take span. `id`/`colorIndex` are left default for the caller to assign.
    MidiClip End(Frame endFrame);

    // Notes closed so far (for a live count / meter). Does not include held
    // notes still waiting for their note-off.
    size_t ClosedNoteCount() const { return fNotes.size(); }
    bool   HasOpenNotes()    const { return !fOpen.empty(); }

private:
    // Key a held note by channel+pitch so overlapping keys are independent.
    static int Key(uint8_t channel, uint8_t pitch) {
        return (int)channel * 128 + (int)pitch;
    }
    void CloseNote(int key, Frame endFrame);

    struct Open {
        uint8_t pitch    = 0;
        uint8_t velocity = 0;
        Frame   start    = 0;   // clip-relative
    };

    Frame                        fStart = 0;
    std::unordered_map<int, Open> fOpen;    // key -> held note
    std::vector<MidiNote>        fNotes;    // closed notes, clip-relative
};

} // namespace daw
