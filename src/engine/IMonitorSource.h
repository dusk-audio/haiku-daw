// IMonitorSource — a live input the engine can mix into its output for
// input monitoring (hear yourself while recording). The Recorder implements it;
// the Engine consumes it. Kept as a tiny interface so the engine doesn't depend
// on the Media Kit Recorder type.
#pragma once

#include <cstddef>

namespace daw {

class IMonitorSource {
public:
    virtual ~IMonitorSource() = default;

    // Drain up to `maxFloats` interleaved-stereo float samples into `dst`.
    // Returns the count actually written (0 on underrun). Must be lock-free /
    // allocation-free: the engine calls this from the RT audio callback.
    virtual std::size_t ReadMonitor(float* dst, std::size_t maxFloats) = 0;

    // Sample rate of the monitor samples. The engine mixes them at any rate,
    // resampling to its output rate on the RT thread when they differ.
    virtual float MonitorRate() const = 0;
};

} // namespace daw
