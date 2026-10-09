#include "DeviceLatency.h"

#include "../model/RecordPlan.h"   // LatencyUsToFrames

#include <MediaRoster.h>
#include <MediaNode.h>

namespace daw {

Frame DeviceRoundTripFrames(double framesPerSecond) {
    BMediaRoster* roster = BMediaRoster::Roster();
    if (!roster) return 0;

    media_node out, in;
    if (roster->GetAudioOutput(&out) != B_OK) return 0;
    if (roster->GetAudioInput(&in) != B_OK) return 0;

    bigtime_t outLat = 0, inLat = 0;
    if (roster->GetLatencyFor(out, &outLat) != B_OK) return 0;
    if (roster->GetLatencyFor(in, &inLat) != B_OK) return 0;

    return LatencyUsToFrames((int64_t)(outLat + inLat), framesPerSecond);
}

} // namespace daw
