// Recorder — audio capture to one WAV per armed track (M5: multichannel input).
//
// Mirrors the playback engine, inverted: BMediaRecorder delivers input buffers
// on a Media Kit service thread (the record hook); the hook converts to float
// and pushes the WHOLE interleaved capture into a lock-free RingBuffer; a
// low-priority disk thread drains it and writes one WavWriter per take,
// de-interleaving that take's channels out of the same capture
// (`model/ChannelSplit.h`). The hook does no file I/O and no allocation, same
// real-time contract as the audio callback.
//
// The input node is CHOSEN, not implied (M5): Start() takes the node NAME a
// track's InputSource stores and finds it in the live node list
// (`engine/AudioInputs.h`). The pre-M5 recipe — BMediaRoster::GetAudioInput
// with a wildcard format — stays as the fallback for an empty or vanished
// name, so a session keeps recording after a device is unplugged instead of
// not recording at all.
//
// Haiku-only: depends on the Media Kit (BMediaRecorder + BMediaRoster).
#pragma once

#include "RingBuffer.h"
#include "WavWriter.h"
#include "IMonitorSource.h"
#include "../dsp/MeterMath.h"        // kMeterFullScale + the clip flag bits (M3.3)
#include "../model/ChannelSplit.h"   // TakeSpec / TakeChannels / Deinterleave

#include <MediaDefs.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace daw {

class Recorder : public IMonitorSource {
public:
    Recorder();
    ~Recorder();

    // Open the input named `device` ("" or a name that is not live right now =
    // the system default input), start the disk thread and begin capturing to
    // one file per take. Every take reads its channels out of the SAME capture,
    // so an 8-channel interface fills eight mono takes in one pass. B_BAD_VALUE
    // when `takes` is empty.
    status_t Start(const std::vector<TakeSpec>& takes, const std::string& device);

    // The pre-M5 call: one 16-bit stereo take of the default input's first two
    // channels (prototypes/record_clip, and any caller that wants one file).
    status_t Start(const std::string& path);

    // The disk thread's last failure: 0 = none, 1 = a take file could not be
    // created, 2 = a write failed (disk full, device error). The UI polls this
    // (the disk thread must not touch the UI), reports once and stops the take,
    // which keeps whatever was written. One failed take stops the capture:
    // every take comes out of the same pass, and a truncated tail on one file
    // would silently desynchronise the tracks from each other.
    int ErrorCode() const { return fError.load(std::memory_order_relaxed); }
    void     Stop();   // stop capture, flush, finalize the WAVs

    // Input monitoring: when on, the capture hook also pushes samples into a
    // monitor ring the engine drains. RT-safe (lock-free, drops on overflow).
    // The ring is STEREO-interleaved — that is what the engine's monitor path
    // reads — and carries the FIRST take's channel pair (a mono take's channel
    // goes to both sides), so what is monitored is what is being recorded.
    void SetMonitor(bool on) { fMonitor.store(on, std::memory_order_relaxed); }
    std::size_t ReadMonitor(float* dst, std::size_t maxFloats) override {
        return fMonitorRing.Read(dst, maxFloats);
    }
    float MonitorRate() const override { return fRate.load(); }

    bool    IsRecording() const { return fRunning.load(); }
    // Frames written by the take writers (all takes hold the same pass, so this
    // is one number) — what the UI turns into the clip's length.
    int64_t FramesWritten() const {
        return fWriters.empty() ? 0 : fWriters[0]->FramesWritten();
    }
    float   SampleRate() const { return fRate.load(); }
    int     Channels() const { return fChannels.load(); }   // the CAPTURE's channels

    // The takes this capture writes: path (as passed to Start) and how many
    // there are, so the drop path can put each one on its own track.
    size_t TakeCount() const { return fTakes.size(); }
    const std::string& TakePath(size_t i) const {
        static const std::string none;
        return i < fTakes.size() ? fTakes[i].path : none;
    }

    // Input peak (abs) of the last captured buffer, over the monitored take's
    // channel pair, for a meter.
    float PeakL() const { return fPeakL.load(); }
    float PeakR() const { return fPeakR.load(); }

    // Did the capture ring overflow during this take (the disk fell behind)?
    // Latched for the take: the transport's xrun readout adds it to the
    // engine's own count, because a dropped sample in a take is a dropout too.
    bool Xrun() const { return fXrun.load(std::memory_order_relaxed); }

    // Clip flags, one bit per channel (kMeterClipL/R in dsp/MeterMath.h): the
    // capture hook latches a bit at digital full scale — two compares on the
    // block peak it already computes, no extra pass over the samples — and the
    // UI reads the word and clears it (a clip between the two is not lost).
    uint32_t ClipFlags() const { return fClipFlags.load(std::memory_order_relaxed); }
    void ClearClipFlags(uint32_t mask) {
        fClipFlags.fetch_and(~mask, std::memory_order_relaxed);
    }

    // Live capture waveform envelope, PER TAKE: one min/max pair per
    // kEnvBucketFrames source frames of that take's own de-interleaved signal,
    // appended by the disk thread (single producer) as it writes and read by
    // the UI (single consumer) to draw the take as it grows. Append-only with
    // an atomic count, so already-published buckets are stable. Only the first
    // kMaxLiveEnvTakes takes get one — a live waveform is a convenience and
    // each costs ~2.4 MB — and EnvCount() is 0 for the rest.
    static constexpr int kEnvBucketFrames = 256;
    static constexpr size_t kMaxLiveEnvTakes = 4;
    size_t EnvCount(size_t take) const;
    float  EnvMin(size_t take, size_t i) const;
    float  EnvMax(size_t take, size_t i) const;

private:
    static void RecordHook(void* cookie, bigtime_t time, void* data,
                           size_t size, const media_format& format);
    void HandleBuffer(void* data, size_t size, const media_format& format);
    void DiskLoop();

    BMediaRoster*                   fRoster = nullptr;
    std::unique_ptr<BMediaRecorder> fRec;
    // What each take writes. Fixed at Start (the UI reads FramesWritten()
    // through them); the disk thread opens the files once the format is known.
    std::vector<TakeSpec>                   fTakes;
    std::vector<std::unique_ptr<WavWriter>> fWriters;

    RingBuffer  fRing;              // float samples, hook -> disk thread
    RingBuffer  fMonitorRing;       // stereo float, hook -> engine (monitoring)
    std::thread fDiskThread;

    std::atomic<bool>  fRunning{false};
    std::atomic<bool>  fMonitor{false};
    std::atomic<bool>  fFormatReady{false};
    std::atomic<float> fRate{0.0f};
    std::atomic<int>   fChannels{0};
    std::atomic<int>   fFormat{0};   // media_raw_audio_format::format tag
    // What the meter and the monitor ring follow: the first take's channel
    // pair on the capture (clamped to the negotiated channel count in the hook).
    std::atomic<int>   fMonL{0};
    std::atomic<int>   fMonR{1};
    std::atomic<float> fPeakL{0.0f};
    std::atomic<float> fPeakR{0.0f};
    std::atomic<uint32_t> fClipFlags{0};  // latched input clips (M3.3)
    std::atomic<bool>  fXrun{false}; // ring overflowed (disk not keeping up)
    std::atomic<int>   fError{0};    // 0 none, 1 open, 2 write (see ErrorCode)
    std::atomic<int64_t> fDroppedFloats{0}; // xrun-dropped samples to pad w/ silence

    // Live waveform envelope per take (disk thread writes by index, UI reads
    // below the published count). Empty for takes past kMaxLiveEnvTakes.
    std::vector<std::vector<float>>  fEnvMin;
    std::vector<std::vector<float>>  fEnvMax;
    std::unique_ptr<std::vector<std::atomic<size_t>>> fEnvCount;
};

} // namespace daw
