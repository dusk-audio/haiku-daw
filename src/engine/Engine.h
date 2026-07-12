// Engine — milestone 2 playback engine.
//
// Pulls decoded audio from disk (WavSource) through a lock-free RingBuffer
// filled by a per-track disk thread, and mixes it in the BSoundPlayer
// real-time callback. One TrackStream per playing clip; the callback sums
// them, so extending to true multitrack (milestone 3) is just more streams.
//
// Threading contract (the rule that governs the whole DAW):
//   - The audio callback (RT thread) only reads rings + atomics. No alloc,
//     no locks, no file I/O.
//   - Disk threads do all file reading and fill the rings.
//   - The main thread starts/stops and polls IsFinished().
//
// Haiku-only: depends on the Media Kit (BSoundPlayer + WavSource).
#pragma once

#include "RingBuffer.h"
#include "WavSource.h"
#include "../model/Project.h"

#include <SoundPlayer.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace daw {

// One clip's playback: a decoder, a ring, and the disk thread feeding it,
// plus where the clip sits on the timeline and the track's gain.
class TrackStream {
public:
    TrackStream(const std::string& path, Frame startFrame, Frame lengthFrames,
                float gain, float pan);
    ~TrackStream();

    status_t Prepare();          // open file, start disk thread, prime ring
    void     StopThread();

    // Called from the RT callback. Adds this stream's contribution for the
    // block [blockStart, blockStart+frames) into the interleaved stereo
    // `out`. Never blocks; on underrun it contributes silence.
    void Mix(float* out, size_t frames, Frame blockStart);

    Frame EndFrame() const { return fStart + fLength; }
    bool  Valid() const { return fSource.IsValid(); }
    float SourceRate() const { return fSource.FrameRate(); }

private:
    void DiskLoop();             // producer thread body

    std::string fPath;
    Frame       fStart;
    Frame       fLength;
    float       fGainL;         // per-channel gain after equal-power pan
    float       fGainR;

    WavSource   fSource;
    RingBuffer  fRing;
    std::thread fDiskThread;
    std::atomic<bool> fRunning{false};
};

class Engine {
public:
    Engine() = default;
    ~Engine();

    // Build streams from the project's audio clips and open the output.
    status_t Load(const Project& project);

    void Start();
    void Stop();

    // True once the playhead has passed the end of all clips.
    bool  IsFinished() const { return fFinished.load(); }
    Frame Playhead() const { return fPlayhead.load(); }

    float OutputRate() const { return fOutputRate; }

private:
    static void PlayTrampoline(void* cookie, void* buffer, size_t size,
                               const media_raw_audio_format& format);
    void FillBuffer(float* out, size_t frames);

    std::unique_ptr<BSoundPlayer>             fPlayer;
    std::vector<std::unique_ptr<TrackStream>> fStreams;

    std::atomic<Frame> fPlayhead{0};
    std::atomic<bool>  fPlaying{false};
    std::atomic<bool>  fFinished{false};
    Frame  fEndFrame   = 0;
    float  fOutputRate = 48000.0f;
};

} // namespace daw
