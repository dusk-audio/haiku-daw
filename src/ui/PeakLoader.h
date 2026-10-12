// PeakLoader — waveform envelopes off the window thread, cached on disk (M4.4).
//
// `MainWindow::RebuildPeaks` used to do this inline: for every clip, open the
// file and scan it top to bottom on the window thread, once per load, with
// nothing kept between runs. A project with a few hundred clips froze the window
// for as long as it takes to read all of that audio, twice over on the second
// open.
//
// The work moves here. A pass walks the project's clip paths and, for each one:
//
//   - stat the file, and try the cache — `<settings>/HaikuDAW/peaks/<hash>.bin`,
//     valid only if it was written for this file's modification time, size and
//     bucket size (PeakCacheId). A hit costs one stat and one small read;
//   - on a miss, open the audio and build the envelope, then write the cache.
//
// The worker never touches the window's map: finished envelopes go into its own
// list, and the window drains them while it holds its own lock — which is the
// same lock the timeline draws under, so no new locking appears in the drawing
// code. `Start` bumps a generation, so a pass for a project that has since been
// replaced finishes into nothing instead of filling the new project with the old
// project's clips.
//
// Haiku-only (BEntry/BFile), like the rest of src/ui/. The parts a bug would be
// silent in — the id, the bytes, the cache name — are kit-free and host-tested
// in src/model/PeakCache.cpp.
#pragma once

#include "../model/PeakCache.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace daw {

class PeakLoader {
public:
    ~PeakLoader();

    // Start a pass over these clip paths (duplicates are fine: the caller hands
    // over the project's clip list). Any pass still running stops publishing.
    void Start(std::vector<std::string> paths);

    // Stop and join. The window calls this before it goes away.
    void Stop();

    // Move what the worker finished into `into`. Call with the window locked.
    // Returns how many envelopes arrived.
    size_t Drain(std::map<std::string, PeakCache>& into);

    // The last pass's outcome, for the tests and the record: how many envelopes
    // came from the cache, how many were built, and how many are ready in all.
    int CacheHits()   const { return fHits.load(std::memory_order_relaxed); }
    int CacheMisses() const { return fMisses.load(std::memory_order_relaxed); }
    int ReadyCount()  const { return fReady.load(std::memory_order_relaxed); }

    // What happened to one path in the last pass: 0 = not walked, 1 = came from
    // the cache, 2 = built (and cached), 3 = skipped (no such file, or the audio
    // would not open). A project's clips do not all point at media that is still
    // on the machine, so "did MY file hit the cache" is a question about a path,
    // not about the whole pass.
    enum Outcome { kNotWalked = 0, kHit = 1, kBuilt = 2, kSkipped = 3 };
    int OutcomeOf(const std::string& path) const {
        std::lock_guard<std::mutex> lk(fMutex);
        auto it = fOutcome.find(path);
        return it == fOutcome.end() ? (int)kNotWalked : it->second;
    }

    // Is a pass still working?
    bool Busy() const { return fBusy.load(std::memory_order_relaxed); }

    // Is there anything left for the window to do — a pass still running, or
    // envelopes waiting to be drained? The window's pulse asks this to keep
    // itself alive until the last one has been handed over (a worker that
    // finished between two pulses still has its list full).
    bool HasWork() const {
        if (fBusy.load(std::memory_order_relaxed)) return true;
        std::lock_guard<std::mutex> lk(fMutex);
        return !fDone.empty();
    }

private:
    void Worker(std::vector<std::string> paths, uint64_t gen);

    std::thread                        fThread;
    std::atomic<uint64_t>              fGen{0};      // newest pass wins
    std::atomic<bool>                  fBusy{false};
    std::atomic<int>                   fHits{0}, fMisses{0}, fReady{0};
    mutable std::mutex                 fMutex;
    std::vector<std::pair<std::string, PeakCache>> fDone;   // worker -> window
    std::map<std::string, int>         fOutcome;            // path -> Outcome
};

} // namespace daw
