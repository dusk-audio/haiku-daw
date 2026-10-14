#include "Recorder.h"

#include "AudioInputs.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace daw {

// ~2 seconds of stereo float at 96k of slack between hook and disk thread.
static constexpr size_t kRecRingFloats = 96000 * 2 * 2;
// One interleaved block from the ring, plus the partial frame carried over from
// the previous read (at most channels-1 floats; 128 channels is far past
// anything the Media Kit will hand over).
static constexpr size_t kReadFloats = 4096;
static constexpr size_t kCarryFloats = 128;
// ~13 min at 96 kHz / 256 frames-per-bucket, per take with a live envelope.
static constexpr size_t kEnvBuckets = 300000;

Recorder::Recorder() : fRing(kRecRingFloats), fMonitorRing(kRecRingFloats) {}

Recorder::~Recorder() {
    Stop();
}

size_t Recorder::EnvCount(size_t take) const {
    if (fEnvCount == nullptr || take >= fEnvCount->size()) return 0;
    return (*fEnvCount)[take].load(std::memory_order_acquire);
}

float Recorder::EnvMin(size_t take, size_t i) const {
    if (take >= fEnvMin.size() || i >= fEnvMin[take].size()) return 0.0f;
    return fEnvMin[take][i];
}

float Recorder::EnvMax(size_t take, size_t i) const {
    if (take >= fEnvMax.size() || i >= fEnvMax[take].size()) return 0.0f;
    return fEnvMax[take][i];
}

// --- capture hook (Media Kit service thread) --------------------------

void Recorder::RecordHook(void* cookie, bigtime_t, void* data, size_t size,
                          const media_format& format) {
    static_cast<Recorder*>(cookie)->HandleBuffer(data, size, format);
}

void Recorder::HandleBuffer(void* data, size_t size, const media_format& fmt) {
    const media_raw_audio_format& raw = fmt.u.raw_audio;

    // Publish the negotiated format on the first buffer so the disk thread can
    // open the writers. No file I/O here — this runs on the RT service thread.
    if (!fFormatReady.load(std::memory_order_acquire)) {
        fRate.store(raw.frame_rate);
        fChannels.store(raw.channel_count > 0 ? (int)raw.channel_count : 2);
        fFormat.store((int)raw.format);
        fFormatReady.store(true, std::memory_order_release);
    }

    // Convert the incoming samples to float in a fixed stack buffer and push
    // to the ring in chunks (no allocation, no locks).
    float tmp[2048];
    float mon[4096];            // stereo monitor block (2 floats per frame)
    float pl = 0.0f, pr = 0.0f;
    const int ch = fChannels.load(std::memory_order_relaxed);
    // The channels the meter and the monitor follow: the first take's pair,
    // clamped to what the device actually negotiated (a device can come up with
    // fewer channels than the address asked for).
    int monL = fMonL.load(std::memory_order_relaxed);
    int monR = fMonR.load(std::memory_order_relaxed);
    if (monL < 0 || monL >= ch) monL = 0;
    if (monR < 0 || monR >= ch) monR = monL;

    const bool monitor = fMonitor.load(std::memory_order_relaxed);
    // A monitor-only capture has no disk thread: the take ring would fill and
    // every write would latch an xrun that means nothing. Skip it there.
    const bool take = fTakeMode.load(std::memory_order_relaxed);
    auto flush = [&](size_t n) {
        if (take) {
            const size_t wrote = fRing.Write(tmp, n);
            if (wrote < n) {
                fXrun.store(true, std::memory_order_relaxed);   // disk fell behind
                // Record how many samples we dropped so the disk thread can pad
                // the take with silence and keep its total length == real
                // elapsed time (else everything after the xrun shifts earlier
                // on the timeline).
                fDroppedFloats.fetch_add((int64_t)(n - wrote),
                                         std::memory_order_relaxed);
            }
        }
        if (monitor) {
            // The engine's monitor path is stereo-interleaved, so the capture
            // is folded to the monitored pair here. RT-safe: arithmetic into a
            // stack buffer, then a lock-free ring write (overflow dropped).
            const size_t frames = ch > 0 ? n / (size_t)ch : 0;
            const size_t out = Deinterleave(tmp, frames, ch, monL, monR,
                                            /*stereo*/ true, mon);
            fMonitorRing.Write(mon, out);
        }
    };

    if (raw.format == media_raw_audio_format::B_AUDIO_SHORT) {
        const int16_t* s = static_cast<const int16_t*>(data);
        const size_t n = size / sizeof(int16_t);
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            const float v = s[i] / 32768.0f;
            tmp[k++] = v;
            const int ci = (int)(i % (size_t)ch);
            if (ci == monL) {
                const float a = std::fabs(v);
                if (a > pl) pl = a;
                if (monR == monL && pl > pr) pr = pl;   // a mono take (both sides)
            } else if (ci == monR) {
                if (std::fabs(v) > pr) pr = std::fabs(v);
            }
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
            const int ci = (int)(i % (size_t)ch);
            if (ci == monL) {
                const float a = std::fabs(v);
                if (a > pl) pl = a;
                if (monR == monL && pl > pr) pr = pl;   // a mono take (both sides)
            } else if (ci == monR) {
                if (std::fabs(v) > pr) pr = std::fabs(v);
            }
            if (k == 2048) { flush(k); k = 0; }
        }
        if (k) flush(k);
    }
    // Other formats: ignored (unsupported); writers stay empty.

    fPeakL.store(pl, std::memory_order_relaxed);
    fPeakR.store(pr, std::memory_order_relaxed);
    // Latched exactly when the block reached full scale: the flag only has to
    // survive until the UI clears it, so testing the block peak is the same
    // test as testing every sample.
    if (ReachesClipLevel(pl))
        fClipFlags.fetch_or(kMeterClipL, std::memory_order_relaxed);
    if (ReachesClipLevel(pr))
        fClipFlags.fetch_or(kMeterClipR, std::memory_order_relaxed);
}

// --- disk writer thread -----------------------------------------------

void Recorder::DiskLoop() {
    // Wait for the first buffer to publish the format, then open the files.
    while (fRunning.load() && !fFormatReady.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!fFormatReady.load(std::memory_order_acquire))
        return;   // stopped before any audio arrived

    const int devCh  = fChannels.load();
    const int rate   = (int)(fRate.load() + 0.5f);
    const int nTakes = (int)fTakes.size();
    if (devCh < 1) return;      // the device reported no channels at all

    // Addresses are clamped to what the device actually negotiated, once, here:
    // a track whose interface shrank still lands on the channels there are.
    std::vector<TakeSpec> specs = fTakes;
    for (int t = 0; t < nTakes; t++) ClampToDevice(&specs[(size_t)t], devCh);

    // One file per take. Exclusive: a take must never truncate a file that
    // already exists (the names are picked by scanning the take directory and
    // WavWriter's exclusive create is the backstop). A file that cannot be
    // created stops the whole capture -- the UI reports it and keeps what was
    // written, exactly as it did with the single-take recorder.
    for (int t = 0; t < nTakes; t++) {
        const TakeSpec& s = specs[(size_t)t];
        if (!fWriters[(size_t)t]->OpenFormat(s.path, rate, TakeChannels(s),
                                             s.bits, s.float32,
                                             /*exclusive*/ true)) {
            fprintf(stderr, "Recorder: cannot open '%s'\n", s.path.c_str());
            fError.store(1, std::memory_order_relaxed);
            for (int i = 0; i < nTakes; i++) fWriters[(size_t)i]->Close();
            return;
        }
    }

    float fbuf[kReadFloats + kCarryFloats];
    float takeBuf[kReadFloats * 2];       // one take's de-interleaved block
    float zeroBuf[kReadFloats];
    std::memset(zeroBuf, 0, sizeof(zeroBuf));

    // Per-take envelope accumulation across Read() calls: min/max over the
    // current bucket of THAT take's frames, published on fill.
    std::vector<float> bMin((size_t)nTakes, 0.0f), bMax((size_t)nTakes, 0.0f);
    std::vector<int>   bFrames((size_t)nTakes, 0);
    std::vector<int>   bChan((size_t)nTakes, 0);
    auto publishEnv = [&](int t, float mn, float mx) {
        if (fEnvCount == nullptr || (size_t)t >= fEnvCount->size()) return;
        const size_t cap = fEnvMin[(size_t)t].size();
        const size_t n = (*fEnvCount)[(size_t)t].load(std::memory_order_relaxed);
        if (n < cap) {
            fEnvMin[(size_t)t][n] = mn;
            fEnvMax[(size_t)t][n] = mx;
            (*fEnvCount)[(size_t)t].store(n + 1, std::memory_order_release);
        }
    };

    int carryN = 0;   // floats of a partial frame left by the previous read
    // Drain until stopped AND the ring is empty (flush the tail).
    while (true) {
        // Pad any xrun-dropped samples with silence so each take's length tracks
        // real elapsed time (downstream timeline stays aligned past the glitch).
        // The pad is a whole number of frames per take, so the takes stay
        // aligned with EACH OTHER too.
        if (int64_t drop = fDroppedFloats.exchange(0, std::memory_order_relaxed)) {
            const int64_t padFrames = devCh > 0 ? drop / devCh : 0;
            for (int t = 0; t < nTakes; t++) {
                const int ch = TakeChannels(specs[(size_t)t]);
                int64_t left = padFrames * ch;
                while (left > 0) {
                    const size_t chunk = left > (int64_t)kReadFloats
                                       ? kReadFloats : (size_t)left;
                    if (!fWriters[(size_t)t]->WriteFloat(zeroBuf, chunk, false))
                        fError.store(2, std::memory_order_relaxed);
                    left -= (int64_t)chunk;
                }
                // Advance the waveform envelope over the padded (silent) frames
                // so the UI waveform stays aligned with the audio after an xrun.
                bMin[(size_t)t] = 0.0f; bMax[(size_t)t] = 0.0f;
                bChan[(size_t)t] = 0;
                for (int64_t fr = 0; fr < padFrames; fr++)
                    if (++bFrames[(size_t)t] >= kEnvBucketFrames) {
                        publishEnv(t, 0.0f, 0.0f);
                        bFrames[(size_t)t] = 0;
                    }
            }
        }

        // Read at the carry offset: a frame split across two ring reads is put
        // back together instead of being written as a shifted take.
        const size_t got = fRing.Read(fbuf + carryN, kReadFloats);
        if (got == 0) {
            // Nothing new: stop (the tail is flushed by then) or wait for the
            // hook. A carried PARTIAL frame is not processed on its own -- a
            // half frame is not audio, and spinning on it would busy-loop.
            if (!fRunning.load())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        const size_t total = got + (size_t)carryN;
        const size_t frames = total / (size_t)devCh;
        const size_t used   = frames * (size_t)devCh;
        for (int t = 0; t < nTakes; t++) {
            const TakeSpec& s = specs[(size_t)t];
            const int ch = TakeChannels(s);
            // De-interleave this take's channels out of the shared capture.
            const size_t n = Deinterleave(fbuf, frames, devCh, s.srcL, s.srcR,
                                          s.stereo, takeBuf);
            // One clamp for both formats: a sample past full scale is stored at
            // full scale rather than as a spike. (PCM would clamp it in the
            // writer anyway; doing it here keeps the envelope honest and keeps
            // a float take inside [-1, 1] too.)
            for (size_t i = 0; i < n; i++) {
                float v = takeBuf[i];
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                takeBuf[i] = v;

                // Envelope: min/max across a bucket of this take's frames.
                if (v < bMin[(size_t)t]) bMin[(size_t)t] = v;
                if (v > bMax[(size_t)t]) bMax[(size_t)t] = v;
                if (++bChan[(size_t)t] >= ch) {          // one frame consumed
                    bChan[(size_t)t] = 0;
                    if (++bFrames[(size_t)t] >= kEnvBucketFrames) {
                        publishEnv(t, bMin[(size_t)t], bMax[(size_t)t]);
                        bMin[(size_t)t] = 0.0f; bMax[(size_t)t] = 0.0f;
                        bFrames[(size_t)t] = 0;
                    }
                }
            }
            // No dither: this is a capture, not a truncation of a wider mix --
            // the extra bits a 24-bit take has over a 16-bit device are noise
            // floor, and dither belongs where a signal is narrowed on purpose.
            if (!fWriters[(size_t)t]->WriteFloat(takeBuf, n, false))
                fError.store(2, std::memory_order_relaxed);
        }

        carryN = (int)(total - used);
        // The carry is at most channels-1 floats; past the buffer (a device
        // with more than kCarryFloats channels) the extra is dropped rather
        // than written outside it. Frames after that point shift by a fraction
        // of one sample -- a take from a >128-channel device is already beyond
        // this recorder.
        if (carryN > (int)kCarryFloats) carryN = (int)kCarryFloats;
        if (carryN > 0) std::memmove(fbuf, fbuf + used, (size_t)carryN * sizeof(float));
    }

    for (int t = 0; t < nTakes; t++) fWriters[(size_t)t]->Close();
    if (fXrun.load())
        fprintf(stderr, "Recorder: WARNING ring overflow (dropped input)\n");
}

// --- control ----------------------------------------------------------

status_t Recorder::Start(const std::string& path) {
    // The pre-M5 call: one stereo 16-bit take of the default input's first pair.
    TakeSpec one;
    one.path    = path;
    one.stereo  = true;
    one.srcL    = 0;
    one.srcR    = 1;
    one.bits    = 16;
    one.float32 = false;
    return Start(std::vector<TakeSpec>{one}, std::string());
}

status_t Recorder::OpenInput(const std::string& device) {
    fRoster = BMediaRoster::Roster();
    if (!fRoster) {
        fprintf(stderr, "Recorder: no media roster\n");
        return B_ERROR;
    }

    // The CHOSEN input node (M5). A name that is not live right now -- the
    // interface was unplugged, or the project came from a machine that had it
    // -- falls back to the system default input instead of refusing to record:
    // the take still lands, on the default device.
    media_node input;
    bool chosen = false;
    if (!device.empty())
        chosen = FindAudioInputNode(fRoster, device, &input);
    if (!chosen) {
        status_t err = fRoster->GetAudioInput(&input);
        if (err != B_OK) {
            fprintf(stderr, "Recorder: GetAudioInput failed: %s\n", strerror(err));
            return err;
        }
        if (!device.empty())
            fprintf(stderr, "Recorder: input '%s' is not live; recording from "
                            "the default input\n", device.c_str());
    }

    fRec.reset(new BMediaRecorder("daw_rec", B_MEDIA_RAW_AUDIO));
    status_t err = fRec->InitCheck();
    if (err != B_OK) {
        fprintf(stderr, "Recorder: BMediaRecorder init: %s\n", strerror(err));
        fRec.reset();
        return err;
    }
    fRec->SetHooks(RecordHook, NULL, this);

    // Wildcard format: let the device negotiate its native rate/format/channel
    // count (the recipe that worked in record_probe.sh); the takes then take
    // the channels they asked for out of whatever it negotiated.
    media_format wild;
    memset(&wild, 0, sizeof(wild));
    wild.type = B_MEDIA_RAW_AUDIO;
    wild.u.raw_audio = media_raw_audio_format::wildcard;

    if ((err = fRec->Connect(input, NULL, &wild)) != B_OK) {
        fprintf(stderr, "Recorder: Connect failed: %s\n", strerror(err));
        fRec.reset();
        return err;
    }
    return B_OK;
}

status_t Recorder::Start(const std::vector<TakeSpec>& takes,
                         const std::string& device) {
    if (fRunning.load())
        return B_BUSY;
    if (takes.empty())
        return B_BAD_VALUE;

    status_t err = OpenInput(device);
    if (err != B_OK)
        return err;

    // A take: the capture feeds the disk ring, and the caller decides
    // afterwards whether it also feeds the monitor ring (SetMonitor).
    fTakeMode.store(true, std::memory_order_relaxed);
    fMonitor.store(false, std::memory_order_relaxed);

    // Reset per-take state and start the drain thread before the recorder,
    // so no buffer is missed.
    fTakes = takes;
    fWriters.clear();
    for (size_t i = 0; i < fTakes.size(); i++)
        fWriters.push_back(std::unique_ptr<WavWriter>(new WavWriter()));
    // The meter and the monitor ring follow the first take's pair; the hook
    // clamps it against the negotiated channel count.
    fMonL.store(fTakes[0].srcL, std::memory_order_relaxed);
    fMonR.store(fTakes[0].stereo ? fTakes[0].srcR : fTakes[0].srcL,
                std::memory_order_relaxed);
    fFormatReady.store(false);
    fXrun.store(false);
    fError.store(0, std::memory_order_relaxed);
    fClipFlags.store(0, std::memory_order_relaxed);   // a new take starts clean
    // Preallocate each take's waveform envelope to a fixed capacity so the disk
    // thread writes by index (never reallocates) while the UI reads published
    // buckets. Only the first kMaxLiveEnvTakes get one: a live waveform is a
    // convenience, and each costs ~2.4 MB.
    fEnvMin.assign(fTakes.size(), std::vector<float>());
    fEnvMax.assign(fTakes.size(), std::vector<float>());
    const size_t nEnv = fTakes.size() < kMaxLiveEnvTakes ? fTakes.size()
                                                         : kMaxLiveEnvTakes;
    for (size_t i = 0; i < nEnv; i++) {
        fEnvMin[i].assign(kEnvBuckets, 0.0f);
        fEnvMax[i].assign(kEnvBuckets, 0.0f);
    }
    fEnvCount = std::make_unique<std::vector<std::atomic<size_t>>>(fTakes.size());
    fRunning.store(true);
    fDiskThread = std::thread(&Recorder::DiskLoop, this);

    if ((err = fRec->Start()) != B_OK) {
        fprintf(stderr, "Recorder: Start failed: %s\n", strerror(err));
        Stop();
        return err;
    }
    return B_OK;
}

status_t Recorder::StartMonitor(const std::string& device, int channelL,
                                int channelR) {
    if (fRunning.load())
        return B_BUSY;

    status_t err = OpenInput(device);
    if (err != B_OK)
        return err;

    // No takes: nothing to write, so no writers, no envelopes and no disk
    // thread -- the capture's only consumer is the monitor ring, which is why
    // SetMonitor is on from the first buffer (the per-track I button is the
    // reason this capture exists at all).
    fTakeMode.store(false, std::memory_order_relaxed);
    fTakes.clear();
    fWriters.clear();
    fEnvMin.clear();
    fEnvMax.clear();
    fEnvCount.reset();
    fMonL.store(channelL < 0 ? 0 : channelL, std::memory_order_relaxed);
    fMonR.store(channelR < 0 ? (channelL < 0 ? 0 : channelL) : channelR,
                std::memory_order_relaxed);
    fFormatReady.store(false);
    fXrun.store(false);
    fError.store(0, std::memory_order_relaxed);
    fClipFlags.store(0, std::memory_order_relaxed);
    fMonitor.store(true, std::memory_order_relaxed);
    fRunning.store(true);

    if ((err = fRec->Start()) != B_OK) {
        fprintf(stderr, "Recorder: monitor Start failed: %s\n", strerror(err));
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
            fDiskThread.join();   // flushes the ring tail + closes the WAVs
    }
    fRec.reset();           // disconnects the recorder
}

} // namespace daw
