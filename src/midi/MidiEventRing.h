// Single-producer / single-consumer lock-free ring of MidiEvents.
//
// Producer = the Midi Kit delivery thread (the BMidiLocalConsumer hooks run
// there). Consumer = the app / engine thread draining events into a recorder
// or synth. No locks, no allocation after construction — the same real-time
// contract as the audio RingBuffer, so the delivery hook never blocks.
//
// MidiEvent is trivially copyable, so this is a plain value ring. Capacity is
// rounded up to a power of two; overflow drops the newest event (Push returns
// false) rather than blocking. Portable STL only — host-testable.
#pragma once

#include "MidiEvent.h"

#include <atomic>
#include <cstddef>
#include <vector>

namespace daw {

class MidiEventRing {
public:
    explicit MidiEventRing(std::size_t minCapacity = 256) {
        std::size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        fCapacity = cap;
        fMask = cap - 1;
        fData.resize(cap);
    }

    std::size_t ReadAvailable() const {
        return fWrite.load(std::memory_order_acquire)
             - fRead.load(std::memory_order_relaxed);
    }

    // Producer side. Returns false (dropping the event) if the ring is full.
    bool Push(const MidiEvent& e) {
        const std::size_t w = fWrite.load(std::memory_order_relaxed);
        const std::size_t r = fRead.load(std::memory_order_acquire);
        if (w - r >= fCapacity) return false;   // full
        fData[w & fMask] = e;
        fWrite.store(w + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Drains up to `max` events into `dst`; returns the count.
    std::size_t Read(MidiEvent* dst, std::size_t max) {
        const std::size_t r = fRead.load(std::memory_order_relaxed);
        const std::size_t w = fWrite.load(std::memory_order_acquire);
        std::size_t avail = w - r;
        std::size_t n = max < avail ? max : avail;
        for (std::size_t i = 0; i < n; i++)
            dst[i] = fData[(r + i) & fMask];
        fRead.store(r + n, std::memory_order_release);
        return n;
    }

    std::size_t Capacity() const { return fCapacity; }

private:
    std::vector<MidiEvent> fData;
    std::size_t            fCapacity = 0;
    std::size_t            fMask = 0;
    std::atomic<std::size_t> fRead{0};
    std::atomic<std::size_t> fWrite{0};
};

} // namespace daw
