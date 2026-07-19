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

    // Feed one event that arrived at timeline position `frameNow`. Note on/off
    // pair into notes; control change and pitch bend are captured as the clip's
    // controller events (repeats of the value already in force are dropped).
    void OnEvent(const MidiEvent& e, Frame frameNow);

    // Close the take at `endFrame`: any notes still held are ended there. The
    // returned MidiClip has its notes clip-relative and lengthFrames set to the
    // take span. `id`/`colorIndex` are left default for the caller to assign.
    MidiClip End(Frame endFrame);

    // Notes closed so far (for a live count / meter). Does not include held
    // notes still waiting for their note-off.
    size_t ClosedNoteCount() const { return fNotes.size(); }
    bool   HasOpenNotes()    const { return !fOpen.empty(); }

    // A live snapshot of the take in progress (for drawing it while recording):
    // every closed note plus each still-held note extended to `nowFrame`. Notes
    // are clip-relative (same frame base as End()). Const — does not finalize.
    std::vector<MidiNote> SnapshotNotes(Frame nowFrame) const;

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
    std::vector<MidiClipEvent>   fEvents;   // CC / bend, clip-relative
    // Last value recorded per controller (and for bend), so a knob held still
    // -- or a controller that resends the same value -- doesn't fill the take
    // with duplicate points. A controller is a step function, so a repeat of
    // the value already in force is a no-op that only costs scan time later.
    //
    // Keyed WITHOUT the channel, to match the model these events feed:
    // MidiClipEvent carries no channel (the track supplies it), and playback
    // resolves a controller by its number alone. Keying the cache by channel
    // let the same controller arriving on two channels write interleaved,
    // contradictory points that playback then read as one lane.
    std::unordered_map<int, int> fLastCc;   // cc number -> value
    int                          fLastBend = -1;   // 14-bit value, -1 = none yet
};

// Split loop-recorded notes (clip-relative to the loop start, spanning several
// passes of length `loopLen`) into one note list per pass, each re-based to the
// loop start. takes[k] holds pass k's notes (empty if that pass was silent).
// Kit-free, host-testable.
std::vector<std::vector<MidiNote>> SplitMidiLoopTakes(
    const std::vector<MidiNote>& notes, Frame loopLen);

// The controller-event counterpart: same re-basing, so a CC move played during
// pass 2 lands in pass 2's region instead of collapsing onto the first one.
// Each pass also inherits the value in force when it began (the latest earlier
// event for that controller, re-stamped at frame 0), so a stacked take sounds
// the same alone as it did in the pass it was played in.
std::vector<std::vector<MidiClipEvent>> SplitMidiLoopEvents(
    const std::vector<MidiClipEvent>& events, Frame loopLen, int passes);

} // namespace daw
