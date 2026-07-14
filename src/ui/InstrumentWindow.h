// InstrumentWindow — editor for a MIDI track's synth voice (waveform + ADSR).
//
// Same threading contract as EffectsWindow: a separate BWindow on its own
// looper thread edits a local snapshot and posts the whole Instrument back to
// the main window (kMsgApplyInstrument), which owns model mutation. Opened from
// a MIDI track's "Inst" header box.
#pragma once

#include "../model/Instrument.h"
#include "../model/types.h"

#include <Messenger.h>
#include <Window.h>

namespace daw {

// Fields: int64 "track"; int32 "wave"; float "a","d","s","r".
constexpr uint32 kMsgApplyInstrument = 'inst';

class InstrumentWindow : public BWindow {
public:
    InstrumentWindow(BRect frame, Instrument inst, TrackId track,
                     BMessenger apply);

    void MessageReceived(BMessage* msg) override;

private:
    void Build();
    void Apply();

    Instrument fInst;
    TrackId    fTrack;
    BMessenger fApply;
    BView*     fRoot;
};

} // namespace daw
