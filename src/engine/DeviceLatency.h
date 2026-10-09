// DeviceLatency — what the audio device costs, in frames.
//
// The Media Kit knows; this asks it. The number is the round trip through the
// output and the input (both nodes' GetLatencyFor, summed), converted to
// timeline frames at the given rate, and it is what a recorded take has to be
// slid EARLIER by to land where the player heard it.
//
// Media Kit only (Haiku-only, like Engine.cpp and Recorder.cpp). It returns 0
// wherever the roster, the nodes or the numbers are unavailable — a machine
// with no usable device — so a caller can always use the result and a wrong
// answer degrades to "no compensation" rather than to a wild slide.
#pragma once

#include "../model/types.h"

namespace daw {

// Audio output + input latency, in frames at `framesPerSecond`.
Frame DeviceRoundTripFrames(double framesPerSecond);

} // namespace daw
