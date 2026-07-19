// InstrumentWindow — editor for a MIDI track's voice.
//
// Same threading contract as EffectsWindow: a separate BWindow on its own
// looper thread edits a local snapshot and posts the whole InstrumentDesc back
// to the main window (kMsgApplyInstrument), which owns model mutation. Opened
// from a MIDI track's "Inst" header box.
//
// The voice is either the built-in synth (waveform + ADSR sliders) or a
// soundfont (.sfz / .sf2). Picking a soundfont LOADS it here, on this window's
// looper, into the process-wide SoundfontCache — never on the audio thread, and
// never during an engine rebuild. By the time MainWindow applies the descriptor
// the samples are already resident, so the next Play (and every loop-record
// restart after it) only has to look the instrument up.
#pragma once

#include "../model/Instrument.h"
#include "../model/types.h"
#include "../synth/Sf2ToRegions.h"

#include <FilePanel.h>
#include <Messenger.h>
#include <Window.h>

#include <string>
#include <vector>

namespace daw {

// Fields: int64 "track"; int32 "type"; int32 "wave"; float "a","d","s","r";
//         string "path"; int32 "preset".
constexpr uint32 kMsgApplyInstrument = 'inst';

class InstrumentWindow : public BWindow {
public:
    InstrumentWindow(BRect frame, InstrumentDesc inst, TrackId track,
                     BMessenger apply);
    ~InstrumentWindow() override;

    void MessageReceived(BMessage* msg) override;
    void DispatchMessage(BMessage* msg, BHandler* h) override;  // spacebar -> transport

private:
    void Build();
    void Apply();

    // Post a deferred Build(). Menu handlers MUST use this rather than calling
    // Build() inline — see the comment on the definition.
    void RebuildLater();

    // Refresh fStatus from the SoundfontCache. With allowDecode, a cache miss
    // is decoded here — BLOCKING this window's looper for as long as it takes,
    // behind a busy cursor. Without it, a miss just reports "not loaded"; the
    // constructor uses that so opening the editor is always instant.
    void LoadSoundfontNow(bool allowDecode = true);

    // Re-read the SF2's preset list (metadata only, cheap).
    void RefreshPresets();

    InstrumentDesc             fDesc;
    TrackId                    fTrack;
    BMessenger                 fApply;
    BView*                     fRoot;
    BFilePanel*                fPanel = nullptr;
    // BFilePanel does NOT take ownership of its ref filter, so the window has
    // to outlive the panel with it and delete it alongside.
    BRefFilter*                fFilter = nullptr;
    std::vector<Sf2PresetInfo> fPresets;
    std::string                fStatus;   // loaded summary, or the load error
    bool                       fLoadOk = false;
    bool                       fPartial = false;  // loaded, but samples dropped
};

} // namespace daw
