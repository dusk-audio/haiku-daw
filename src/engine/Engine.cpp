#include "Engine.h"

#include <MediaDefs.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace daw {

// Ring holds ~2 seconds of stereo audio at 48k: plenty of slack so the
// disk thread stays ahead of the RT callback without hoarding memory.
static constexpr size_t kRingFramesPerStream = 48000 * 2;   // frames
static constexpr size_t kRingFloats = kRingFramesPerStream * 2;

// --- TrackStream ------------------------------------------------------

TrackStream::TrackStream(const std::string& path, Frame startFrame,
                         Frame lengthFrames, float gain, float pan)
    : fPath(path), fStart(startFrame), fLength(lengthFrames),
      fGain(gain), fPan(pan), fRing(kRingFloats) {}

TrackStream::~TrackStream() {
    StopThread();
}

status_t TrackStream::Prepare() {
    status_t err = fSource.Open(fPath.c_str());
    if (err != B_OK)
        return err;

    fRunning.store(true);
    fDiskThread = std::thread(&TrackStream::DiskLoop, this);

    // Prime: give the disk thread a moment to fill the ring before the RT
    // callback starts draining, so playback doesn't underrun on frame 0.
    for (int i = 0; i < 50 && fRing.ReadAvailable() < kRingFloats / 2; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    return B_OK;
}

void TrackStream::StopThread() {
    fRunning.store(false);
    if (fDiskThread.joinable())
        fDiskThread.join();
}

// Producer thread: decode chunks and push them into the ring. Carries the
// unpushed remainder of a chunk across iterations when the ring is full.
void TrackStream::DiskLoop() {
    const float* chunk = nullptr;
    size_t chunkFloats = 0;   // remaining floats in the current chunk
    size_t chunkOffset = 0;

    while (fRunning.load()) {
        if (chunkFloats == 0) {
            size_t frames = 0;
            if (!fSource.ReadChunk(&chunk, &frames)) {
                // End of file: nothing more to push. Idle until stopped.
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            chunkFloats = frames * 2;   // stereo
            chunkOffset = 0;
        }

        size_t pushed = fRing.Write(chunk + chunkOffset, chunkFloats);
        chunkOffset += pushed;
        chunkFloats -= pushed;

        if (pushed == 0)   // ring full: wait for the RT thread to drain
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void TrackStream::Mix(float* out, size_t frames, Frame blockStart) {
    const Frame clipEnd = fStart + fLength;
    for (size_t i = 0; i < frames; i++) {
        const Frame ph = blockStart + static_cast<Frame>(i);
        if (ph < fStart || ph >= clipEnd)
            continue;   // outside the clip -> contribute nothing

        float lr[2];
        if (fRing.Read(lr, 2) < 2)
            continue;   // underrun -> silence for this frame

        out[i * 2 + 0] += lr[0] * fGain;
        out[i * 2 + 1] += lr[1] * fGain;
    }
}

// --- Engine -----------------------------------------------------------

Engine::~Engine() {
    Stop();
    fStreams.clear();   // joins disk threads via ~TrackStream
    fPlayer.reset();
}

status_t Engine::Load(const Project& project) {
    // Open the output first so we know the real output rate.
    media_raw_audio_format format = media_raw_audio_format::wildcard;
    format.frame_rate    = project.sampleRate;
    format.channel_count = 2;
    format.format        = media_raw_audio_format::B_AUDIO_FLOAT;
    format.byte_order    = B_MEDIA_HOST_ENDIAN;

    fPlayer.reset(new BSoundPlayer(&format, "haiku_daw", PlayTrampoline,
                                   nullptr, this));
    status_t err = fPlayer->InitCheck();
    if (err != B_OK) {
        fprintf(stderr, "Engine: BSoundPlayer init failed: %s\n", strerror(err));
        return err;
    }
    fOutputRate = fPlayer->Format().frame_rate;

    // Build one stream per audio clip.
    fEndFrame = 0;
    for (const Track& t : project.Tracks()) {
        if (t.type != TrackType::Audio || t.muted)
            continue;
        for (const Clip& c : t.clips) {
            if (c.sourcePath.empty())
                continue;
            auto s = std::make_unique<TrackStream>(
                c.sourcePath, c.startFrame, c.lengthFrames, t.gain, t.pan);
            if (s->Prepare() != B_OK || !s->Valid()) {
                fprintf(stderr, "Engine: skipping clip '%s'\n",
                        c.sourcePath.c_str());
                continue;
            }
            // Warn on sample-rate mismatch (no resampling in M2).
            if (s->EndFrame() > fEndFrame)
                fEndFrame = s->EndFrame();
            fStreams.push_back(std::move(s));
        }
    }

    if (fStreams.empty()) {
        fprintf(stderr, "Engine: no playable clips\n");
        return B_ERROR;
    }
    return B_OK;
}

void Engine::Start() {
    fPlayhead.store(0);
    fFinished.store(false);
    fPlaying.store(true);
    fPlayer->SetHasData(true);
    fPlayer->Start();
}

void Engine::Stop() {
    fPlaying.store(false);
    if (fPlayer)
        fPlayer->Stop();
}

void Engine::PlayTrampoline(void* cookie, void* buffer, size_t size,
                            const media_raw_audio_format& format) {
    Engine* self = static_cast<Engine*>(cookie);
    const size_t frames = size / (sizeof(float) * format.channel_count);
    self->FillBuffer(static_cast<float*>(buffer), frames);
}

void Engine::FillBuffer(float* out, size_t frames) {
    std::memset(out, 0, frames * 2 * sizeof(float));   // stereo silence

    if (!fPlaying.load())
        return;

    const Frame blockStart = fPlayhead.load();
    if (blockStart >= fEndFrame) {
        fFinished.store(true);   // main thread will Stop(); RT stays silent
        return;
    }

    for (auto& s : fStreams)
        s->Mix(out, frames, blockStart);

    fPlayhead.store(blockStart + static_cast<Frame>(frames));
}

} // namespace daw
