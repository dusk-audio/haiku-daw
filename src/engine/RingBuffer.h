// Single-producer / single-consumer lock-free float ring buffer.
//
// Producer = the disk-reader thread (fills decoded audio).
// Consumer = the real-time audio callback (drains it).
// No locks, no allocation after construction — safe to Read() from the
// audio thread. Portable STL only (builds on host too, so it's testable).
//
// Capacity is rounded up to a power of two so index wrap is a mask.
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace daw {

class RingBuffer {
public:
    explicit RingBuffer(size_t minCapacity) {
        size_t cap = 1;
        while (cap < minCapacity) cap <<= 1;
        fCapacity = cap;
        fMask = cap - 1;
        fData.resize(cap, 0.0f);
    }

    // Number of floats available to read / space available to write.
    size_t ReadAvailable() const {
        return fWrite.load(std::memory_order_acquire)
             - fRead.load(std::memory_order_relaxed);
    }
    size_t WriteAvailable() const {
        return fCapacity - ReadAvailable();
    }

    // Producer side. Copies up to `count` floats in; returns how many were
    // actually written (less than count if the buffer filled).
    size_t Write(const float* src, size_t count) {
        const size_t w = fWrite.load(std::memory_order_relaxed);
        const size_t r = fRead.load(std::memory_order_acquire);
        size_t space = fCapacity - (w - r);
        size_t n = count < space ? count : space;
        for (size_t i = 0; i < n; i++)
            fData[(w + i) & fMask] = src[i];
        fWrite.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer side. Copies up to `count` floats out; returns how many were
    // actually read (less than count on underrun).
    size_t Read(float* dst, size_t count) {
        const size_t r = fRead.load(std::memory_order_relaxed);
        const size_t w = fWrite.load(std::memory_order_acquire);
        size_t avail = w - r;
        size_t n = count < avail ? count : avail;
        for (size_t i = 0; i < n; i++)
            dst[i] = fData[(r + i) & fMask];
        fRead.store(r + n, std::memory_order_release);
        return n;
    }

    // Consumer side only (the RT thread is the single reader): drop up to
    // `count` floats without copying them, and report how many were dropped.
    // Used to re-align a stream after its graph is swapped in mid-playback
    // (TrackStream::RebaseTo) — O(1) where a Read of the same frames would be
    // O(n), and still just arithmetic on the same atomics as Read.
    size_t Skip(size_t count) {
        const size_t r = fRead.load(std::memory_order_relaxed);
        const size_t w = fWrite.load(std::memory_order_acquire);
        const size_t avail = w - r;
        const size_t n = count < avail ? count : avail;
        fRead.store(r + n, std::memory_order_release);
        return n;
    }

    size_t Capacity() const { return fCapacity; }

private:
    std::vector<float>  fData;
    size_t              fCapacity = 0;
    size_t              fMask = 0;
    std::atomic<size_t> fRead{0};    // consumer advances
    std::atomic<size_t> fWrite{0};   // producer advances
};

} // namespace daw
