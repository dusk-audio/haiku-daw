#include "Engine.h"

#include "../dsp/EffectFactory.h"

#include <MediaDefs.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace daw {

// Ring holds ~2 seconds of stereo audio at 48k: plenty of slack so the
// disk thread stays ahead of the RT callback without hoarding memory.
static constexpr size_t kRingFramesPerStream = 48000 * 2;   // frames
static constexpr size_t kRingFloats = kRingFramesPerStream * 2;

// Equal-power pan: pan -1 = hard left, 0 = center (-3 dB each), +1 = hard
// right. Folds the track gain into the returned per-channel gains.
static void EqualPowerGains(float gain, float pan, float* outL, float* outR) {
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    const float theta = (pan * 0.5f + 0.5f) * float(M_PI) * 0.5f;
    *outL = gain * std::cos(theta);
    *outR = gain * std::sin(theta);
}

// --- TrackStream ------------------------------------------------------

TrackStream::TrackStream(TrackId track, const std::string& path,
                         Frame startFrame, Frame lengthFrames,
                         Frame sourceOffset, float gain, float pan,
                         bool audible, Frame seekProjectDelta, float outputRate,
                         Frame fadeIn, Frame fadeOut)
    : fTrackId(track), fPath(path), fStart(startFrame), fLength(lengthFrames),
      fSourceOffset(sourceOffset), fSeekDelta(seekProjectDelta),
      fOutputRate(outputRate), fFadeIn(fadeIn), fFadeOut(fadeOut),
      fRing(kRingFloats) {
    float gl, gr;
    EqualPowerGains(gain, pan, &gl, &gr);
    fGainL.store(gl);
    fGainR.store(gr);
    fAudible.store(audible);
}

void TrackStream::SetMix(float gain, float pan, bool audible) {
    float gl, gr;
    EqualPowerGains(gain, pan, &gl, &gr);
    fGainL.store(gl, std::memory_order_relaxed);
    fGainR.store(gr, std::memory_order_relaxed);
    fAudible.store(audible, std::memory_order_relaxed);
}

TrackStream::~TrackStream() {
    StopThread();
}

status_t TrackStream::Prepare() {
    if (!fSource.Open(fPath))
        return B_ERROR;

    const float srcRate = fSource.FrameRate();
    fResampler.reset(new Resampler(srcRate, fOutputRate));

    // Seek the source to match the start playhead. fSeekDelta is in output
    // frames; convert to source frames by the rate ratio.
    if (fSeekDelta > 0 && fOutputRate > 0) {
        const Frame srcSkip = static_cast<Frame>(
            fSeekDelta * (double)srcRate / fOutputRate + 0.5);
        fSource.Seek(fSourceOffset + srcSkip);
    } else if (fSourceOffset > 0) {
        fSource.Seek(fSourceOffset);
    }

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
    const float* chunk = nullptr;   // points into fResampled
    size_t chunkFloats = 0;         // remaining floats to push
    size_t chunkOffset = 0;

    while (fRunning.load()) {
        if (chunkFloats == 0) {
            const float* src = nullptr;
            size_t frames = 0;
            if (!fSource.ReadChunk(&src, &frames)) {
                // End of file: nothing more to push. Idle until stopped.
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            // Resample source-rate audio up/down to the output rate so the RT
            // callback drains at 1 ring frame == 1 timeline frame.
            fResampled.clear();
            fResampler->Process(src, frames, fResampled);
            if (fResampled.empty())
                continue;   // produced nothing this pass (heavy downsample)
            chunk = fResampled.data();
            chunkFloats = fResampled.size();
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
    // Snapshot the live params once per block.
    const float gl  = fGainL.load(std::memory_order_relaxed);
    const float gr  = fGainR.load(std::memory_order_relaxed);
    const bool  aud = fAudible.load(std::memory_order_relaxed);

    for (size_t i = 0; i < frames; i++) {
        const Frame ph = blockStart + static_cast<Frame>(i);
        if (ph < fStart || ph >= clipEnd)
            continue;   // outside the clip -> contribute nothing

        float lr[2];
        if (fRing.Read(lr, 2) < 2)
            continue;   // underrun -> silence for this frame

        // Always consume the ring above; only sum when audible so an unmute
        // resumes in sample-sync rather than replaying buffered audio.
        if (aud) {
            // Linear fade-in/out envelope (combined as a min).
            float fade = 1.0f;
            const Frame rel = ph - fStart;
            if (fFadeIn > 0 && rel < fFadeIn)
                fade = (float)rel / (float)fFadeIn;
            if (fFadeOut > 0 && rel >= fLength - fFadeOut) {
                float fo = (float)(fLength - rel) / (float)fFadeOut;
                if (fo < fade) fade = fo;
            }
            out[i * 2 + 0] += lr[0] * gl * fade;
            out[i * 2 + 1] += lr[1] * gr * fade;
        }
    }
}

// --- Engine -----------------------------------------------------------

Engine::~Engine() {
    Stop();
    fStreams.clear();   // joins disk threads via ~TrackStream
    fPlayer.reset();
}

status_t Engine::Load(const Project& project, Frame startFrame,
                      Frame minEndFrame) {
    fStartFrame = startFrame;
    // Open the output first so we know the real output rate.
    media_raw_audio_format format = media_raw_audio_format::wildcard;
    format.frame_rate    = project.sampleRate;
    format.channel_count = 2;
    format.format        = media_raw_audio_format::B_AUDIO_FLOAT;
    format.byte_order    = B_MEDIA_HOST_ENDIAN;
    // Request an explicit buffer size (frames * channels * 4 bytes) instead of
    // letting media_server suggest one. Smaller = lower latency, more xrun risk.
    format.buffer_size   = fBufferFrames * 2 * sizeof(float);

    fPlayer.reset(new BSoundPlayer(&format, "haiku_daw", PlayTrampoline,
                                   nullptr, this));
    status_t err = fPlayer->InitCheck();
    if (err != B_OK) {
        fprintf(stderr, "Engine: BSoundPlayer init failed: %s\n", strerror(err));
        return err;
    }
    fOutputRate = fPlayer->Format().frame_rate;
    fSynth.SetSampleRate(fOutputRate);
    fMetronome = Metronome(fOutputRate, project.tempoBPM,
                           project.timeSig.numerator);
    fMasterGain.store(project.masterGain);

    // Solo overrides mute: if any track (audio or MIDI) is soloed, only
    // soloed (non-muted) tracks play.
    bool anySolo = false;
    for (const Track& t : project.Tracks())
        if (t.soloed && !t.muted)
            anySolo = true;

    // Build one stream per audio clip. All clips get a stream (even muted /
    // non-soloed) so mute/solo can be toggled live; audibility is a per-stream
    // flag the RT mix honors, not a build-time filter.
    fEndFrame = 0;
    for (const Track& t : project.Tracks()) {
        if (t.type != TrackType::Audio)
            continue;
        const bool audible = !t.muted && (!anySolo || t.soloed);
        for (const Clip& c : t.clips) {
            if (c.sourcePath.empty())
                continue;
            // How far into the clip (in timeline frames) playback starts.
            const Frame seekDelta =
                (startFrame > c.startFrame) ? startFrame - c.startFrame : 0;
            auto s = std::make_unique<TrackStream>(
                t.id, c.sourcePath, c.startFrame, c.lengthFrames,
                c.sourceOffset, t.gain, t.pan, audible, seekDelta, fOutputRate,
                c.fadeInFrames, c.fadeOutFrames);
            if (s->Prepare() != B_OK || !s->Valid()) {
                fprintf(stderr, "Engine: skipping clip '%s'\n",
                        c.sourcePath.c_str());
                continue;
            }
            // Sources are resampled to the output rate on their disk thread.
            if (s->EndFrame() > fEndFrame)
                fEndFrame = s->EndFrame();
            fStreams.push_back(std::move(s));
        }
    }

    // Group audio streams into per-track buses.
    fBuses.clear();
    for (auto& s : fStreams) {
        Bus* bus = nullptr;
        for (Bus& b : fBuses)
            if (b.id == s->Track()) { bus = &b; break; }
        if (!bus) {
            fBuses.push_back(Bus{});
            fBuses.back().id = s->Track();
            bus = &fBuses.back();
        }
        bus->streams.push_back(s.get());
    }

    // Add a bus per audible MIDI track: snapshot its notes for the synth to
    // render (rebuild-on-play, so MIDI edits apply on the next Start).
    for (const Track& t : project.Tracks()) {
        if (t.type != TrackType::Midi || t.notes.empty())
            continue;
        const bool audible = !t.muted && (!anySolo || t.soloed);
        if (!audible)
            continue;
        Bus b;
        b.id    = t.id;
        b.notes = t.notes;
        EqualPowerGains(t.gain, t.pan, &b.midiGainL, &b.midiGainR);
        fBuses.push_back(std::move(b));
        for (const MidiNote& n : t.notes)
            if (n.startFrame + n.lengthFrames > fEndFrame)
                fEndFrame = n.startFrame + n.lengthFrames;
    }

    // No audio content is fine only if the caller extends the range (e.g. to
    // run the metronome / a loop over silence); otherwise there's nothing.
    if (fBuses.empty() && minEndFrame <= 0) {
        fprintf(stderr, "Engine: nothing to play\n");
        return B_ERROR;
    }

    // Extend the play range past content for looping / a metronome-only run.
    if (minEndFrame > fEndFrame)
        fEndFrame = minEndFrame;

    // Build each bus's effect chain from its track's descriptors.
    for (Bus& b : fBuses) {
        const Track* t = project.FindTrack(b.id);
        if (!t) continue;
        for (const EffectDesc& d : t->fx) {
            auto fx = MakeEffect(d);
            if (!fx) continue;
            fx->Prepare(fOutputRate);
            b.fx.push_back(std::move(fx));
        }
    }

    // Master bus effect chain (applied to the summed output).
    fMasterFx.clear();
    for (const EffectDesc& d : project.masterFx) {
        auto fx = MakeEffect(d);
        if (!fx) continue;
        fx->Prepare(fOutputRate);
        fMasterFx.push_back(std::move(fx));
    }

    // Per-bus mix scratch, sized to the output buffer (generous floor).
    size_t maxFrames = (size_t)(fPlayer->Format().buffer_size
                                / (sizeof(float) * 2));
    if (maxFrames < 8192) maxFrames = 8192;
    fScratch.assign(maxFrames * 2, 0.0f);

    return B_OK;
}

void Engine::UpdateMix(const Project& project) {
    fMasterGain.store(project.masterGain, std::memory_order_relaxed);
    bool anySolo = false;
    for (const Track& t : project.Tracks())
        if (t.type == TrackType::Audio && t.soloed && !t.muted)
            anySolo = true;

    for (const Track& t : project.Tracks()) {
        if (t.type != TrackType::Audio)
            continue;
        const bool audible = !t.muted && (!anySolo || t.soloed);
        for (auto& s : fStreams)
            if (s->Track() == t.id)
                s->SetMix(t.gain, t.pan, audible);
    }
}

void Engine::Start() {
    fPlayhead.store(fStartFrame);
    fFinished.store(fStartFrame >= fEndFrame);
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

    if (!fPlaying.load()) {
        fPeakL.store(0.0f); fPeakR.store(0.0f);
        return;
    }

    const Frame blockStart = fPlayhead.load();
    if (blockStart >= fEndFrame) {
        fFinished.store(true);   // main thread will Stop(); RT stays silent
        fPeakL.store(0.0f); fPeakR.store(0.0f);
        return;
    }

    // Mix each bus into scratch, run its effect chain, sum into master.
    const size_t nfloats = frames * 2;
    float* sc = fScratch.data();
    for (Bus& b : fBuses) {
        std::memset(sc, 0, nfloats * sizeof(float));
        for (TrackStream* s : b.streams)
            s->Mix(sc, frames, blockStart);
        if (!b.notes.empty()) {
            // Render dry, then apply equal-power gain/pan (a MIDI bus is
            // synth-only, so scaling the whole scratch is correct) — same law
            // as audio streams, including the -3 dB center attenuation.
            fSynth.Render(b.notes, sc, frames, blockStart, 1.0f);
            for (size_t i = 0; i < frames; i++) {
                sc[i * 2 + 0] *= b.midiGainL;
                sc[i * 2 + 1] *= b.midiGainR;
            }
        }
        for (auto& fx : b.fx)
            fx->Process(sc, static_cast<int>(frames));
        for (size_t i = 0; i < nfloats; i++)
            out[i] += sc[i];
    }

    // Master bus FX on the summed output (before gain/metering).
    for (auto& fx : fMasterFx)
        fx->Process(out, static_cast<int>(frames));

    // Master gain on the summed output (before metering so the meter reflects
    // what actually leaves the engine).
    const float mg = fMasterGain.load(std::memory_order_relaxed);
    if (mg != 1.0f)
        for (size_t i = 0; i < nfloats; i++)
            out[i] *= mg;

    // Metronome click on top of the mix (not affected by master gain).
    if (fMetronomeOn.load(std::memory_order_relaxed))
        fMetronome.Render(out, frames, blockStart, 0.3f);

    // Block peak per channel for the UI meters (arithmetic only, RT-safe).
    float pl = 0.0f, pr = 0.0f;
    for (size_t i = 0; i < frames; i++) {
        const float l = std::fabs(out[i * 2 + 0]);
        const float r = std::fabs(out[i * 2 + 1]);
        if (l > pl) pl = l;
        if (r > pr) pr = r;
    }
    fPeakL.store(pl);
    fPeakR.store(pr);

    fPlayhead.store(blockStart + static_cast<Frame>(frames));
}

} // namespace daw
