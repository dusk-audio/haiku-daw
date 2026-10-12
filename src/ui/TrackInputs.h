// TrackInputs — the ONE definition of a track's record-input choices, shared
// by the inspector's Input button and the arrangement header's "In" field.
//
// Both pickers list the same things and commit the same `InputSource` through
// `SetTrackInputCommand`; before this header existed the list lived inline in
// InspectorView, so a second copy in the header would have been free to drift
// (a MIDI endpoint the inspector offers and the header does not is exactly the
// bug this prevents).
//
// M5: an audio track's choices are no longer just "the system default" — they
// are every live input DEVICE (engine/AudioInputs.h), each contributing its
// default channel pair and one entry per channel it reports. Picking a single
// channel also means a MONO track (see WidthOfChoice); picking a pair means
// stereo. The device is stored by NAME, like a MIDI endpoint, so the
// assignment survives the node-id reshuffling of a reboot.
//
// Haiku-only: it enumerates the Media Kit's input nodes and the Midi Kit's
// published endpoints.
#pragma once

#include "../model/Project.h"    // InputSource / Track / TrackType
#include "../engine/AudioInputs.h"   // EnumerateAudioInputs (Media Kit)
#include "../midi/MidiPort.h"    // EnumerateMidiEndpoints (Midi Kit 2)

#include <cstdio>
#include <string>
#include <vector>

namespace daw {

// A name's tail: "/dev/midi/usb/0-0" -> "0-0", "/dev/audio/hmulti/0-0" -> "0-0".
// Haiku's node names lead with the driver path, and the full name never fits a
// menu entry or the header's narrow In field.
inline std::string InputNameTail(const std::string& n) {
    const size_t slash = n.find_last_of('/');
    return slash == std::string::npos ? n : n.substr(slash + 1);
}

// The choices a track's input picker offers, in menu order. None is always
// first; a MIDI track then gets every published PRODUCER (a consumer is not a
// source) except our own input port, and an audio track the system default
// followed by every live input device. The track's CURRENT source is kept even
// when it is not published right now (a keyboard that is switched off, an
// interface that is unplugged): a project must not silently look unassigned,
// and re-picking the same name costs nothing.
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
        bool haveCurrent = t.input.device.empty();   // the default IS listed above
        for (const AudioInputNode& d : EnumerateAudioInputs(BMediaRoster::Roster())) {
            // The device's default pair: what the pre-M5 recorder captured.
            out.push_back(InputSource{ InputSource::kAudioDefault, "", 0,
                                       d.name, 0, 1 });
            // One entry per channel it reports. A single channel is a MONO
            // track (WidthOfChoice), which is how an 8-in interface feeds
            // eight tracks: one channel each.
            for (int c = 0; c < d.channels; c++)
                out.push_back(InputSource{ InputSource::kAudioDefault, "", 0,
                                           d.name, c, c });
            if (t.input.kind == InputSource::kAudioDefault
                && t.input.device == d.name)
                haveCurrent = true;
        }
        // A device that is not live right now keeps its entry, so the track
        // still reads as assigned and picking it again changes nothing.
        if (t.input.kind == InputSource::kAudioDefault && !haveCurrent
            && !t.input.device.empty())
            out.push_back(t.input);
    }
    return out;
}

// The channel width a choice means: naming ONE channel is a mono take, naming
// two (or the device's default pair) is stereo. The picker commits this with
// the address, so "record channel 3" is one gesture, not two.
inline TrackChannels WidthOfChoice(const InputSource& s) {
    return (s.kind == InputSource::kAudioDefault && s.channelR == s.channelL)
           ? TrackChannels::Mono : TrackChannels::Stereo;
}

// Do two addresses name the same source? Every field that decides WHERE the
// audio comes from -- comparing kind+name alone would light up all of one
// device's channel entries at once.
inline bool SameInput(const InputSource& a, const InputSource& b) {
    return a.kind == b.kind && a.name == b.name && a.device == b.device
        && a.channelL == b.channelL && a.channelR == b.channelR;
}

// The menu label for one choice: a device contributes several entries, so the
// channels it names are spelled out.
inline std::string InputChoiceLabel(const InputSource& s) {
    if (s.kind == InputSource::kNone)         return "None";
    if (s.kind == InputSource::kMidi)         return s.name;
    if (s.device.empty())                     return "Default Input";
    char ch[24];
    if (s.channelR == s.channelL)
        std::snprintf(ch, sizeof(ch), " Ch %d", s.channelL + 1);
    else
        std::snprintf(ch, sizeof(ch), " %d-%d", s.channelL + 1, s.channelR + 1);
    return InputNameTail(s.device) + ch;
}

// The short form the header's narrow "In" field shows ("Default", a MIDI
// endpoint's tail, or a device and its channels), because the field is ~90
// design px and a full node name would never fit.
inline std::string InputShortLabel(const InputSource& s) {
    if (s.kind == InputSource::kNone)         return "\xE2\x80\x94";  // em dash
    if (s.kind == InputSource::kMidi)         return InputNameTail(s.name);
    if (s.device.empty())                     return "Default";
    char ch[24];
    if (s.channelR == s.channelL)
        std::snprintf(ch, sizeof(ch), " %d", s.channelL + 1);
    else
        std::snprintf(ch, sizeof(ch), " %d-%d", s.channelL + 1, s.channelR + 1);
    return InputNameTail(s.device) + ch;
}

} // namespace daw
