// Recorder — audio capture to a WAV file (milestone 5).
//
// Mirrors the playback engine, inverted: BMediaRecorder delivers input buffers
// on a Media Kit service thread (the record hook); the hook converts to float
// and pushes into a lock-free RingBuffer; a low-priority disk thread drains it
// and writes a WAV via WavWriter. The hook does no file I/O and no allocation,
// same real-time contract as the audio callback.
//
// Capture recipe confirmed on the target image (record_probe.sh): the physical
// input is found via BMediaRoster::GetAudioInput and connected with
// Connect(node, NULL, wildcard); the device negotiates its native format
// (here 96 kHz / 2ch / int16). We don't assume that format — the hook reports
// it on its first buffer and the disk thread opens the writer from it.
//
// Haiku-only: depends on the Media Kit (BMediaRecorder + BMediaRoster).
#pragma once

#include "RingBuffer.h"
#include "WavWriter.h"
#include "IMonitorSource.h"

#include <MediaDefs.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace daw {

class Recorder : public IMonitorSource {
public:
    Recorder();
    ~Recorder();

    // Open the input, start the disk thread, begin capturing to `path`.
    status_t Start(const std::string& path);
    void     Stop();   // stop capture, flush, finalize the WAV

    // Input monitoring: when on, the capture hook also pushes samples into a
    // monitor ring the engine drains. RT-safe (lock-free, drops on overflow).
    void SetMonitor(bool on) { fMonitor.store(on, std::memory_order_relaxed); }
    std::size_t ReadMonitor(float* dst, std::size_t maxFloats) override {
        return fMonitorRing.Read(dst, maxFloats);
    }
    float MonitorRate() const override { return fRate.load(); }

    bool    IsRecording() const { return fRunning.load(); }
    int64_t FramesWritten() const { return fWriter.FramesWritten(); }
    float   SampleRate() const { return fRate.load(); }
    int     Channels() const { return fChannels.load(); }

    // Input peak (abs) of the last captured buffer, per channel, for a meter.
    float PeakL() const { return fPeakL.load(); }
    float PeakR() const { return fPeakR.load(); }

private:
    static void RecordHook(void* cookie, bigtime_t time, void* data,
                           size_t size, const media_format& format);
    void HandleBuffer(void* data, size_t size, const media_format& format);
    void DiskLoop();

    BMediaRoster*                   fRoster = nullptr;
    std::unique_ptr<BMediaRecorder> fRec;
    std::string                     fPath;

    RingBuffer  fRing;              // float samples, hook -> disk thread
    RingBuffer  fMonitorRing;       // float samples, hook -> engine (monitoring)
    WavWriter   fWriter;           // opened by the disk thread once format known
    std::thread fDiskThread;

    std::atomic<bool>  fRunning{false};
    std::atomic<bool>  fMonitor{false};
    std::atomic<bool>  fFormatReady{false};
    std::atomic<float> fRate{0.0f};
    std::atomic<int>   fChannels{0};
    std::atomic<int>   fFormat{0};   // media_raw_audio_format::format tag
    std::atomic<float> fPeakL{0.0f};
    std::atomic<float> fPeakR{0.0f};
    std::atomic<bool>  fXrun{false}; // ring overflowed (disk not keeping up)
};

} // namespace daw
