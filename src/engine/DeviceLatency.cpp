#include "DeviceLatency.h"

#include "../model/RecordPlan.h"   // LatencyUsToFrames

#include <MediaRoster.h>
#include <MediaNode.h>

namespace daw {

Frame DeviceRoundTripFrames(double framesPerSecond) {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster) return 0;

    // GetAudioOutput/GetAudioInput hand the caller a node REFERENCE; the
    // roster's own reference is separate, so every return path here has to
    // release what it acquired (this runs once per take).
    media_node out, in;
    if (roster->GetAudioOutput(&out) != B_OK) return 0;
    if (roster->GetAudioInput(&in) != B_OK) {
        roster->ReleaseNode(out);
        return 0;
    }

    bigtime_t outLat = 0, inLat = 0;
    const bool got = roster->GetLatencyFor(out, &outLat) == B_OK
                  && roster->GetLatencyFor(in, &inLat) == B_OK;
    roster->ReleaseNode(in);
    roster->ReleaseNode(out);
    if (!got) return 0;

    return LatencyUsToFrames((int64_t)(outLat + inLat), framesPerSecond);
}

} // namespace daw
