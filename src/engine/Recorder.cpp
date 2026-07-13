#include "Recorder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace daw {

// ~2 seconds of stereo float at 96k of slack between hook and disk thread.
static constexpr size_t kRecRingFloats = 96000 * 2 * 2;

Recorder::Recorder() : fRing(kRecRingFloats) {}

Recorder::~Recorder() {
    Stop();
}

// --- capture hook (Media Kit service thread) --------------------------

void Recorder::RecordHook(void* cookie, bigtime_t, void* data, size_t size,
                          const media_format& format) {
    static_cast<Recorder*>(cookie)->HandleBuffer(data, size, format);
}

void Recorder::HandleBuffer(void* data, size_t size, const media_format& fmt) {
    const media_raw_audio_format& raw = fmt.u.raw_audio;

    // Publish the negotiated format on the first buffer so the disk thread can
    // open the writer. No file I/O here — this runs on the RT service thread.
    if (!fFormatReady.load(std::memory_order_acquire)) {
        fRate.store(raw.frame_rate);
        fChannels.store(raw.channel_count > 0 ? (int)raw.channel_count : 2);
        fFormat.store((int)raw.format);
        fFormatReady.store(true, std::memory_order_release);
    }

    // Convert the incoming samples to float in a fixed stack buffer and push
    // to the ring in chunks (no allocation, no locks).
    float tmp[2048];
    float pl = 0.0f, pr = 0.0f;
    const int ch = fChannels.load(std::memory_order_relaxed);

    auto flush = [&](size_t n) {
        if (fRing.Write(tmp, n) < n)
            fXrun.store(true, std::memory_order_relaxed);   // disk fell behind
    };

    if (raw.format == media_raw_audio_format::B_AUDIO_SHORT) {
        const int16_t* s = static_cast<const int16_t*>(data);
        const size_t n = size / sizeof(int16_t);
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            const float v = s[i] / 32768.0f;
            tmp[k++] = v;
            if ((i % ch) == 0) { if (v > pl || -v > pl) pl = std::fabs(v); }
            else               { if (v > pr || -v > pr) pr = std::fabs(v); }
            if (k == 2048) { flush(k); k = 0; }
        }
        if (k) flush(k);
    } else if (raw.format == media_raw_audio_format::B_AUDIO_FLOAT) {
        const float* s = static_cast<const float*>(data);
        const size_t n = size / sizeof(float);
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            const float v = s[i];
            tmp[k++] = v;
            if ((i % ch) == 0) { if (std::fabs(v) > pl) pl = std::fabs(v); }
            else               { if (std::fabs(v) > pr) pr = std::fabs(v); }
            if (k == 2048) { flush(k); k = 0; }
        }
        if (k) flush(k);
    }
    // Other formats: ignored (unsupported); writer stays empty.

    fPeakL.store(pl, std::memory_order_relaxed);
    fPeakR.store(pr, std::memory_order_relaxed);
}

// --- disk writer thread -----------------------------------------------

void Recorder::DiskLoop() {
    // Wait for the first buffer to publish the format, then open the file.
    while (fRunning.load() && !fFormatReady.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!fFormatReady.load(std::memory_order_acquire))
        return;   // stopped before any audio arrived

    const int   ch   = fChannels.load();
    const int   rate = (int)(fRate.load() + 0.5f);
    if (!fWriter.Open(fPath, rate, ch)) {
        fprintf(stderr, "Recorder: cannot open '%s'\n", fPath.c_str());
        return;
    }

    float   fbuf[4096];
    int16_t ibuf[4096];
    // Drain until stopped AND the ring is empty (flush the tail).
    while (true) {
        const size_t got = fRing.Read(fbuf, 4096);
        if (got == 0) {
            if (!fRunning.load())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        for (size_t i = 0; i < got; i++) {
            float v = fbuf[i];
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            int s = (int)std::lround(v * 32767.0f);
            ibuf[i] = (int16_t)s;
        }
        fWriter.WriteInt16(ibuf, got);
    }

    fWriter.Close();
    if (fXrun.load())
        fprintf(stderr, "Recorder: WARNING ring overflow (dropped input)\n");
}

// --- control ----------------------------------------------------------

status_t Recorder::Start(const std::string& path) {
    if (fRunning.load())
        return B_BUSY;

    fRoster = BMediaRoster::Roster();
    if (!fRoster) {
        fprintf(stderr, "Recorder: no media roster\n");
        return B_ERROR;
    }

    media_node input;
    status_t err = fRoster->GetAudioInput(&input);
    if (err != B_OK) {
        fprintf(stderr, "Recorder: GetAudioInput failed: %s\n", strerror(err));
        return err;
    }

    fRec.reset(new BMediaRecorder("daw_rec", B_MEDIA_RAW_AUDIO));
    if ((err = fRec->InitCheck()) != B_OK) {
        fprintf(stderr, "Recorder: BMediaRecorder init: %s\n", strerror(err));
        fRec.reset();
        return err;
    }
    fRec->SetHooks(RecordHook, NULL, this);

    // Wildcard format: let the device negotiate its native rate/format
    // (the recipe that worked in record_probe.sh).
    media_format wild;
    memset(&wild, 0, sizeof(wild));
    wild.type = B_MEDIA_RAW_AUDIO;
    wild.u.raw_audio = media_raw_audio_format::wildcard;

    if ((err = fRec->Connect(input, NULL, &wild)) != B_OK) {
        fprintf(stderr, "Recorder: Connect failed: %s\n", strerror(err));
        fRec.reset();
        return err;
    }

    // Reset per-take state and start the drain thread before the recorder,
    // so no buffer is missed.
    fPath = path;
    fFormatReady.store(false);
    fXrun.store(false);
    fRunning.store(true);
    fDiskThread = std::thread(&Recorder::DiskLoop, this);

    if ((err = fRec->Start()) != B_OK) {
        fprintf(stderr, "Recorder: Start failed: %s\n", strerror(err));
        Stop();
        return err;
    }
    return B_OK;
}

void Recorder::Stop() {
    if (fRec)
        fRec->Stop();       // no more record-hook callbacks after this

    if (fRunning.exchange(false)) {
        if (fDiskThread.joinable())
            fDiskThread.join();   // flushes the ring tail + closes the WAV
    }
    fRec.reset();           // disconnects the recorder
}

} // namespace daw
