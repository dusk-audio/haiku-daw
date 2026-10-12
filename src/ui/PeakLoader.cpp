#include "PeakLoader.h"

#include "ProjectDocument.h"

#include "../engine/AudioFormats.h"

#include <Entry.h>
#include <File.h>
#include <Path.h>
#include <kernel/OS.h>

#include <cstdio>
#include <cstring>

namespace daw {

// The bucket size every envelope in the cache uses. One value for the whole app:
// a cache file is only valid for the size it was built at (PeakCacheId), so two
// sizes would mean two files per clip and no benefit.
static constexpr int kPeakBucketFrames = 256;

PeakLoader::~PeakLoader() { Stop(); }

void PeakLoader::Start(std::vector<std::string> paths) {
    // The new pass owns the counters; the old one stops publishing (its
    // generation is no longer the newest) and its thread is joined here, which
    // is why a pass must not be started from the RT or builder threads.
    const uint64_t gen = fGen.fetch_add(1, std::memory_order_acq_rel) + 1;
    Stop();
    {
        std::lock_guard<std::mutex> lk(fMutex);
        fDone.clear();
        fOutcome.clear();
    }
    fHits.store(0, std::memory_order_relaxed);
    fMisses.store(0, std::memory_order_relaxed);
    fReady.store(0, std::memory_order_relaxed);
    if (paths.empty()) return;
    fBusy.store(true, std::memory_order_relaxed);
    fThread = std::thread(&PeakLoader::Worker, this, std::move(paths), gen);
}

void PeakLoader::Stop() {
    // No cancellation flag to set: the worker checks its generation after every
    // clip, so bumping fGen (in Start) makes a running pass drop the rest. This
    // just waits for it.
    if (fThread.joinable()) fThread.join();
    fBusy.store(false, std::memory_order_relaxed);
}

size_t PeakLoader::Drain(std::map<std::string, PeakCache>& into) {
    std::vector<std::pair<std::string, PeakCache>> done;
    {
        std::lock_guard<std::mutex> lk(fMutex);
        done.swap(fDone);
    }
    for (auto& kv : done)
        into[kv.first] = std::move(kv.second);
    return done.size();
}

void PeakLoader::Worker(std::vector<std::string> paths, uint64_t gen) {
    BPath cacheDir;
    const bool haveDir = ProjectDocument::PeakCacheDir(cacheDir);
    int ready = 0;
    for (const std::string& path : paths) {
        if (fGen.load(std::memory_order_acquire) != gen) return;   // superseded
        // A stat is all a cache hit needs: the envelope is valid for this file's
        // size and modification time, which is what says "this is still the audio
        // it was built from".
        BEntry entry(path.c_str());
        off_t size = 0;
        time_t mtime = 0;
        if (entry.InitCheck() != B_OK || entry.GetSize(&size) != B_OK
            || entry.GetModificationTime(&mtime) != B_OK) {
            std::lock_guard<std::mutex> lk(fMutex);
            fOutcome[path] = kSkipped;      // gone or unreadable: no waveform
            continue;
        }
        const PeakCacheId id{(int64_t)mtime, (int64_t)size, kPeakBucketFrames};

        PeakCache cache;
        bool hit = false;
        if (haveDir) {
            BPath file(cacheDir);
            file.Append(PeakCacheFileName(path).c_str());
            BFile f(file.Path(), B_READ_ONLY);
            off_t n = 0;
            if (f.InitCheck() == B_OK && f.GetSize(&n) == B_OK
                && n > 0 && n < 64 * 1024 * 1024) {
                std::vector<uint8_t> bytes((size_t)n);
                if (f.Read(bytes.data(), (size_t)n) == (ssize_t)n)
                    hit = DeserializePeakCache(bytes.data(), bytes.size(), id,
                                               cache);
            }
        }
        int outcome = kHit;
        if (hit) {
            fHits.fetch_add(1, std::memory_order_relaxed);
        } else {
            outcome = kBuilt;
            std::unique_ptr<IAudioSource> src = OpenAudioSource(path);
            if (!src) continue;
            // Cancellation is checked per decoded chunk, so a superseded pass
            // gives up in well under a millisecond — the window waits for it
            // before starting the next one, and must never wait on a whole file.
            class GenCancel : public PeakCache::Cancel {
            public:
                GenCancel(const std::atomic<uint64_t>& g, uint64_t want)
                    : fGen(g), fWant(want) {}
                bool Cancelled() override {
                    return fGen.load(std::memory_order_acquire) != fWant;
                }
            private:
                const std::atomic<uint64_t>& fGen;
                uint64_t                     fWant;
            } cancel(fGen, gen);
            if (!cache.Build(*src, kPeakBucketFrames, &cancel)) continue;
            fMisses.fetch_add(1, std::memory_order_relaxed);
            if (haveDir) {
                // A cache that cannot be written is not an error: this session
                // has the envelope, and the next one builds it again.
                BPath file(cacheDir);
                file.Append(PeakCacheFileName(path).c_str());
                const std::vector<uint8_t> bytes = SerializePeakCache(cache, id);
                BFile out(file.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
                if (out.InitCheck() == B_OK)
                    out.Write(bytes.data(), bytes.size());
            }
        }
        {
            std::lock_guard<std::mutex> lk(fMutex);
            if (fGen.load(std::memory_order_acquire) != gen) return;
            fOutcome[path] = outcome;
            fDone.emplace_back(path, std::move(cache));
        }
        fReady.store(++ready, std::memory_order_relaxed);
    }
    fBusy.store(false, std::memory_order_relaxed);
}

} // namespace daw
