// AudioInputs — the live Media Kit nodes an audio track can record FROM (M5).
//
// The pre-M5 recorder asked the roster for "the" audio input (GetAudioInput)
// and connected to it; a machine with more than one input, or an interface with
// more than two channels, had no say in it. This lists the candidates, so a
// track can name one and the Recorder can open THAT node.
//
// An input device PRODUCES the capture stream, so the nodes wanted here are
// live nodes with a raw-audio OUTPUT, and the system mixer and the physical
// output are excluded: the speakers are never a source.
//
// Haiku-only (Media Kit). Device enumeration cannot be exercised on the host or
// on the dev VM — neither has an audio device — so it is on the hardware click
// list (`docs/PLAN_1.0.md` M5.4) rather than on a claim of verification.
#pragma once

#include <MediaDefs.h>
#include <MediaNode.h>
#include <MediaRoster.h>

#include <cstring>
#include <string>
#include <vector>

namespace daw {

// One capture device. `name` is the node's name — the durable identifier a
// track's InputSource stores, exactly like a MIDI endpoint name — and
// `channels` is how many channels it hands over (0 = the node did not say, and
// the input picker then offers only the default pair).
struct AudioInputNode {
    std::string name;
    media_node  node{};
    int         channels = 0;
};

// Every live input device, in the roster's order. Cheap: one GetLiveNodes call
// plus one format query per node, and it is called from the UI (building a
// menu) and from the Recorder (resolving a stored name), never from the RT
// thread.
inline std::vector<AudioInputNode> EnumerateAudioInputs(BMediaRoster* roster) {
    std::vector<AudioInputNode> out;
    if (roster == nullptr) return out;

    media_format raw;
    memset(&raw, 0, sizeof(raw));
    raw.type = B_MEDIA_RAW_AUDIO;
    raw.u.raw_audio = media_raw_audio_format::wildcard;

    live_node_info info[32];
    int32 count = 32;
    if (roster->GetLiveNodes(info, &count, NULL, &raw, NULL, 0) != B_OK)
        return out;
    if (count > 32) count = 32;

    for (int32 i = 0; i < count; i++) {
        const uint32 kind = info[i].node.kind;
        if ((kind & B_PHYSICAL_OUTPUT) != 0) continue;   // an output, not a source
        if ((kind & B_SYSTEM_MIXER) != 0)    continue;   // the system's own mixer
        if (info[i].name[0] == '\0')         continue;   // no name to store

        AudioInputNode n;
        n.name = info[i].name;
        n.node = info[i].node;
        // How many channels it has, when the node will say. A node that answers
        // nothing keeps 0 = unknown; nothing here depends on the answer, it only
        // decides how many channel entries the picker can offer.
        media_output o;
        int32 found = 0;
        if (roster->GetFreeOutputsFor(info[i].node, &o, 1, &found,
                                      B_MEDIA_RAW_AUDIO) == B_OK && found > 0)
            n.channels = (int)o.format.u.raw_audio.channel_count;
        out.push_back(n);
    }
    return out;
}

// The live input device called `name` (a track's stored address), or false when
// it is not there right now. The media_node it fills in is a descriptor from
// the roster's live list — the same shape BMediaRecorder::Connect takes — so
// nothing here has to be released (unlike GetAudioInput's node reference).
inline bool FindAudioInputNode(BMediaRoster* roster, const std::string& name,
                               media_node* out) {
    if (roster == nullptr || out == nullptr || name.empty()) return false;
    for (const AudioInputNode& n : EnumerateAudioInputs(roster))
        if (n.name == name) {
            *out = n.node;
            return true;
        }
    return false;
}

} // namespace daw
