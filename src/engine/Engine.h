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
#include "Resampler.h"
#include "../model/Project.h"
#include "../dsp/IEffect.h"
#include "../synth/Synth.h"

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
    // startFrame/lengthFrames/seekProjectDelta are in output (timeline) frames;
    // sourceOffset is in source frames. The stream resamples the source to
    // outputRate on its disk thread, so the ring is at the output rate.
    TrackStream(TrackId track, const std::string& path, Frame startFrame,
                Frame lengthFrames, Frame sourceOffset, float gain, float pan,
                bool audible, Frame seekProjectDelta, float outputRate);
    ~TrackStream();

    status_t Prepare();          // open file, seek, start disk thread, prime
    void     StopThread();

    // Called from the RT callback. Adds this stream's contribution for the
    // block [blockStart, blockStart+frames) into the interleaved stereo
    // `out`. Never blocks; on underrun it contributes silence. Always drains
    // the ring inside the clip window (even when inaudible) so an unmute
    // mid-playback stays sample-aligned with the playhead.
    void Mix(float* out, size_t frames, Frame blockStart);

    // Live mix update from the UI thread: recompute per-channel gains and the
    // audible flag. Lock-free (atomic stores); the RT callback picks them up
    // at the next block. No thread restart, no reallocation.
    void SetMix(float gain, float pan, bool audible);

    TrackId Track() const { return fTrackId; }
    Frame EndFrame() const { return fStart + fLength; }
    bool  Valid() const { return fSource.IsValid(); }
    float SourceRate() const { return fSource.FrameRate(); }

private:
    void DiskLoop();             // producer thread body

    TrackId     fTrackId;
    std::string fPath;
    Frame       fStart;
    Frame       fLength;
    Frame       fSourceOffset;  // source-frame offset into the file
    Frame       fSeekDelta;     // output frames into the clip to skip on start
    float       fOutputRate;    // engine output rate to resample to
    std::atomic<float> fGainL{0.0f};   // per-channel gain after equal-power pan
    std::atomic<float> fGainR{0.0f};
    std::atomic<bool>  fAudible{true};

    WavSource   fSource;
    std::unique_ptr<Resampler> fResampler;   // source rate -> output rate
    std::vector<float>         fResampled;    // disk-thread scratch buffer
    RingBuffer  fRing;
    std::thread fDiskThread;
    std::atomic<bool> fRunning{false};
};

class Engine {
public:
    Engine() = default;
    ~Engine();

    // Build streams from the project's audio clips and open the output.
    // Playback (and each clip's source) is aligned to start at `startFrame`,
    // so seeking is just a reload at a new start. `minEndFrame` extends the
    // playback end past the last clip/note (used for looping past content, so
    // the playhead keeps advancing through silence up to the loop point).
    status_t Load(const Project& project, Frame startFrame = 0,
                  Frame minEndFrame = 0);

    void Start();
    void Stop();

    // Recompute every stream's gain/pan/audibility from the model, live,
    // without rebuilding the graph. Safe to call from the UI thread while
    // playing (writes atomics the RT callback reads).
    void UpdateMix(const Project& project);

    // True once the playhead has passed the end of all clips.
    bool  IsFinished() const { return fFinished.load(); }
    Frame Playhead() const { return fPlayhead.load(); }

    // Master output peak (abs) of the last mixed block, per channel, in
    // [0, 1+]. Written by the RT callback, read by the UI meter poll.
    float PeakL() const { return fPeakL.load(); }
    float PeakR() const { return fPeakR.load(); }

    float OutputRate() const { return fOutputRate; }

private:
    static void PlayTrampoline(void* cookie, void* buffer, size_t size,
                               const media_raw_audio_format& format);
    void FillBuffer(float* out, size_t frames);

    // One mix bus per audio track: its clips' streams summed into a scratch
    // buffer, then the track's effect chain applied, then added to master.
    struct Bus {
        TrackId                               id;
        std::vector<TrackStream*>             streams;   // audio, owned by fStreams
        std::vector<MidiNote>                 notes;     // MIDI (empty for audio)
        float                                 midiGain = 1.0f;
        std::vector<std::unique_ptr<IEffect>> fx;
    };

    std::unique_ptr<BSoundPlayer>             fPlayer;
    std::vector<std::unique_ptr<TrackStream>> fStreams;
    std::vector<Bus>                          fBuses;
    std::vector<float>                        fScratch;   // per-bus mix buffer
    Synth                                     fSynth;     // MIDI voice renderer

    std::atomic<Frame> fPlayhead{0};
    std::atomic<bool>  fPlaying{false};
    std::atomic<bool>  fFinished{false};
    std::atomic<float> fPeakL{0.0f};
    std::atomic<float> fPeakR{0.0f};
    std::atomic<float> fMasterGain{1.0f};
    Frame  fStartFrame = 0;   // playhead position playback begins at
    Frame  fEndFrame   = 0;
    float  fOutputRate = 48000.0f;
};

} // namespace daw
