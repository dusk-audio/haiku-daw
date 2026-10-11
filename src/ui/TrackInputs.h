// TrackInputs — the ONE definition of a track's record-input choices, shared
// by the inspector's Input button and the arrangement header's "In" field.
//
// Both pickers list the same things and commit the same `InputSource` through
// `SetTrackInputCommand`; before this header existed the list lived inline in
// InspectorView, so a second copy in the header would have been free to drift
// (a MIDI endpoint the inspector offers and the header does not is exactly the
// bug this prevents).
//
// Haiku-only: it enumerates the Midi Kit's published endpoints.
#pragma once

#include "../model/Project.h"   // InputSource / Track / TrackType
#include "../midi/MidiPort.h"   // EnumerateMidiEndpoints (Midi Kit 2)

#include <string>
#include <vector>

namespace daw {

// The choices a track's input picker offers, in menu order. None is always
// first; a MIDI track then gets every published PRODUCER (a consumer is not a
// source) except our own input port, and an audio track the system default.
// The track's CURRENT source is kept even when it is not published right now
// (a keyboard that is switched off): a project must not silently look
// unassigned, and re-picking the same name costs nothing.
inline std::vector<InputSource> TrackInputChoices(const Track& t) {
    std::vector<InputSource> out;
    out.push_back(InputSource{});                       // None
    if (t.type == TrackType::Midi) {
        bool haveCurrent = false;
        for (const MidiEndpointInfo& e : EnumerateMidiEndpoints()) {
            if (!e.isProducer || e.name.find("HaikuDAW") != std::string::npos)
                continue;
            if (t.input.kind == InputSource::kMidi && t.input.name == e.name)
                haveCurrent = true;
            out.push_back(InputSource{ InputSource::kMidi, e.name, 0 });
        }
        if (t.input.kind == InputSource::kMidi && !haveCurrent
            && !t.input.name.empty())
            out.push_back(t.input);
    } else if (t.type == TrackType::Audio) {
        out.push_back(InputSource{ InputSource::kAudioDefault, "", 0 });
    }
    return out;
}

// The menu label for one choice.
inline std::string InputChoiceLabel(const InputSource& s) {
    if (s.kind == InputSource::kNone)         return "None";
    if (s.kind == InputSource::kAudioDefault) return "Default Input";
    return s.name;
}

// The short form the header's narrow "In" field shows: the last path segment
// of an endpoint name ("/dev/midi/usb/0-0" -> "0-0"), because the field is
// ~90 design px and the full name would never fit.
inline std::string InputShortLabel(const InputSource& s) {
    if (s.kind == InputSource::kNone)         return "\xE2\x80\x94";  // em dash
    if (s.kind == InputSource::kAudioDefault) return "Default";
    const size_t slash = s.name.find_last_of('/');
    return slash == std::string::npos ? s.name : s.name.substr(slash + 1);
}

} // namespace daw
