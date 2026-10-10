// Engine — milestone 2 playback engine.
//
// Pulls decoded audio from disk (any IAudioSource: WAV, AIFF, FLAC, Ogg
// Vorbis) through a lock-free RingBuffer
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
// Haiku-only: depends on the Media Kit (BSoundPlayer).
#pragma once

#include "RingBuffer.h"
#include "AudioFormats.h"   // OpenAudioSource (WAV / AIFF / FLAC / Ogg)
#include "Resampler.h"
#include "FrameDelay.h"
#include "InsertSlot.h"
#include "../model/Project.h"
#include "../model/RoutingGraph.h"
#include "../model/Pdc.h"
#include "../dsp/IEffect.h"
#include "../dsp/Loudness.h"
#include "../synth/IInstrument.h"
#include "../synth/InstrumentFactory.h"
#include "../midi/IMidiInput.h"
#include "../midi/MidiRouting.h"
#include "Metronome.h"
#include "IMonitorSource.h"

#include <SoundPlayer.h>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>
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
                bool audible, Frame seekProjectDelta, float outputRate,
                Frame fadeIn, Frame fadeOut, float clipGain = 1.0f);
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

    // Update only the gain/pan (equal-power), leaving audibility as-is. Called
    // per block by automation from the RT thread.
    void SetGainPan(float gain, float pan);

    // Update only audibility, leaving gain/pan as-is (used by UpdateMix for
    // automated tracks, where automation owns the gain/pan).
    void SetAudible(bool audible);

    TrackId Track() const { return fTrackId; }
    Frame EndFrame() const { return fStart + fLength; }
    bool  Valid() const { return fSource && fSource->IsValid(); }
    float SourceRate() const { return fSource ? fSource->FrameRate() : 0.0f; }

private:
    void DiskLoop();             // producer thread body

    TrackId     fTrackId;
    std::string fPath;
    Frame       fStart;
    Frame       fLength;
    Frame       fSourceOffset;  // source-frame offset into the file
    Frame       fSeekDelta;     // output frames into the clip to skip on start
    float       fOutputRate;    // engine output rate to resample to
    Frame       fFadeIn;        // fade-in length (timeline frames)
    Frame       fFadeOut;       // fade-out length (timeline frames)
    float       fClipGain = 1.0f;  // per-clip linear gain
    int64_t     fSkipDebt = 0;     // ring frames to drop to resync after xrun
    std::atomic<float> fGainL{0.0f};   // per-channel gain after equal-power pan
    std::atomic<float> fGainR{0.0f};
    std::atomic<bool>  fAudible{true};

    // The factory's reader for whatever the clip's file is: WAV, AIFF, FLAC or
    // Ogg Vorbis. Owned (the concrete readers are not copyable and their
    // lifetime is per-clip-stream), and null until Prepare() opens it.
    std::unique_ptr<IAudioSource> fSource;
    std::unique_ptr<Resampler> fResampler;   // source rate -> output rate
    std::vector<float>         fResampled;    // disk-thread scratch buffer
    RingBuffer  fRing;
    std::thread fDiskThread;
    std::atomic<bool> fRunning{false};
};

class Engine {
public:
    Engine() {
        for (int i = 0; i < kMeterFxMax; i++)
            fMeterGr[i].store(0.0f, std::memory_order_relaxed);
        // A watch slot that has published nothing yet -- which is what
        // WatchedFxParams reports as "0 values, generation untouched" rather
        // than "0 values, generation 0".
        for (int s = 0; s < kWatchSlots; s++)
            fWatchN[s].store(-1, std::memory_order_relaxed);
    }
    ~Engine();

    // Build streams from the project's audio clips and open the output.
    // Playback (and each clip's source) is aligned to start at `startFrame`,
    // so seeking is just a reload at a new start. `minEndFrame` extends the
    // playback end past the last clip/note (used for looping past content, so
    // the playhead keeps advancing through silence up to the loop point).
    // Returns B_ENTRY_NOT_FOUND when there is nothing to play and the caller
    // did not extend the range -- that is not an error to show anyone; every
    // other failure is the output device.
    status_t Load(const Project& project, Frame startFrame = 0,
                  Frame minEndFrame = 0);

    void Start();
    void Stop();

    // Recompute every stream's gain/pan/audibility from the model, live,
    // without rebuilding the graph. Safe to call from the UI thread while
    // playing (writes atomics the RT callback reads).
    void UpdateMix(const Project& project);

    // Push effect-parameter edits into the running graph without a rebuild, so
    // knob tweaks take effect during playback. Only chains whose structure still
    // matches (same effect types, same count) are updated via SetParam (RT-safe);
    // add/remove/reorder still needs the next Load. Returns true if every chain
    // matched and was synced; false if any chain's structure changed (the caller
    // should rebuild). Safe to call from the UI thread while playing.
    bool SyncFx(const Project& project);

    // Tell every effect the current tempo (for tempo-synced params, e.g. a
    // delay locked to note divisions). Off-RT; call at Load + on tempo change.
    void SetFxTempo(double bpm);

    // Push a single effect parameter into the running graph live (for smooth
    // knob-drag response during playback, before the edit is committed). RT-safe
    // SetParam; `master` targets the master chain, else the track's node.
    void SetFxParamLive(TrackId track, bool master, int fxIndex, int slot,
                        float value);

    // Publish a watched insert's live parameter values for its open native
    // editor. Addressed exactly like SetFxParamLive (same track/master/fxIndex),
    // and resolved against the RUNNING chain on every block, so a rebuild cannot
    // leave it pointing at a freed instance. fxIndex < 0 stops watching `slot`.
    //
    // Several editors can be open at once (one per insert, in different
    // windows), so watches are slots the caller allocates: the same insert may
    // not be watched twice, and a slot is reusable once stopped.
    //
    // This exists because automation drives the plugin's control ports from the
    // audio thread and never touches the model, so the engine's copy is the only
    // place those values can be read from -- and a UI must not read them off the
    // instance the audio thread is running.
    static constexpr int kWatchSlots = 4;
    void SetFxWatch(int slot, TrackId track, bool master, int fxIndex) {
        if (slot < 0 || slot >= kWatchSlots) return;
        // Forget what was published: a NEW watched insert whose values happen to
        // match the old one's would otherwise change nothing and never publish,
        // leaving the freshly opened editor with no frame at all.
        fWatchN[slot].store(-1, std::memory_order_relaxed);
        fWatchMaster[slot].store(master, std::memory_order_relaxed);
        fWatchFx[slot].store(fxIndex, std::memory_order_relaxed);
        fWatchTrack[slot].store(fxIndex < 0 ? kInvalidTrackId : track,
                                std::memory_order_relaxed);
    }

    // Publish every watched insert's values NOW, from the calling thread, for
    // when there is no audio block to do it: the transport is stopped and a
    // live parameter write just changed something (the generic panel with a
    // native editor open). Returns immediately while the audio callback runs --
    // then the block publishes on its own, and this must not race it.
    void PublishFxWatchNow();

    static constexpr int kWatchMax = 64;
    // One watched insert's parameter values as of the last audio block. Written
    // by the RT thread, read here; the array is bracketed by a seqlock
    // generation (odd = mid-write) so a reader cannot see a torn frame. Returns
    // the count copied and the generation, which the caller compares against its
    // previous call: the same value means nothing changed and the caller can
    // skip the frame entirely. 0 values means this slot is not watching, or the
    // watched insert is not in the running chain.
    int WatchedFxParams(int slot, float* out, int maxSlots,
                        uint32_t* generation) const {
        if (slot < 0 || slot >= kWatchSlots) return 0;
        uint32_t g0 = 0, g1 = 0;
        int c = 0;
        do {
            g0 = fWatchGen[slot].load(std::memory_order_acquire);
            if (g0 & 1u) continue;              // mid-write; retry
            const int n = fWatchN[slot].load(std::memory_order_relaxed);
            // Nothing has ever been published for this slot (no watch, or the
            // watched insert is not in the running chain). 0 values, and the
            // caller's generation is left alone rather than clobbered.
            if (n < 0) return 0;
            c = n < maxSlots ? n : maxSlots;
            for (int i = 0; i < c; i++) out[i] = fWatchValues[slot][i];
            // The fence goes AFTER the copy. The closing load is an acquire, and
            // acquire orders nothing before it -- so without this the value
            // loads may sit above it, and the g0 == g1 test would then accept a
            // torn frame (visible on a weakly-ordered machine only, which is
            // exactly why it must not be left to a reader to notice).
            std::atomic_thread_fence(std::memory_order_acquire);
            g1 = fWatchGen[slot].load(std::memory_order_acquire);
        } while ((g0 & 1u) || g0 != g1);
        if (generation) *generation = g0;
        return c;
    }

    // Toggle the metronome click (RT-safe atomic).
    void SetMetronome(bool on) { fMetronomeOn.store(on); }

    // Monitor section (RT-safe atomics). These affect only what is played out,
    // NOT the peak meters (which reflect the true mix). Dim drops the monitor
    // ~20 dB; Mono sums L+R to both channels for mono/phase checking.
    void SetMonitorDim(bool on)  { fMonitorDim.store(on); }
    void SetMonitorMono(bool on) { fMonitorMono.store(on); }

    // Input monitoring: mix a live input source into the output. The source is
    // only mixed when its rate matches the engine's output rate (no RT-side
    // resampling). Both are RT-safe atomic stores.
    void SetMonitorSource(IMonitorSource* src) { fMonSource.store(src); }
    void SetInputMonitor(bool on) { fInputMonitor.store(on); }

    // Live MIDI monitoring: while recording, incoming events from this input are
    // synthesized in real time through each armed MIDI track's instrument, so
    // the player hears themselves. RT-safe atomic; nullptr disables. The events
    // come from MidiInputPort::MonitorInput() (a ring separate from the record
    // drain, so the RT thread is the sole consumer).
    void SetLiveMidi(IMidiInput* in) { fLiveMidi.store(in); }

    // Per-track live-input routing. The caller (MainWindow) resolves each MIDI
    // track's endpoint NAME to a Midi Kit producer id when it opens the input
    // and pushes the result here; the RT thread then filters purely on ids. A
    // track with no entry stays permissive (hears every source), so this is a
    // no-op until inputs are actually assigned. Safe to call while playing.
    void SetMidiRoutes(const std::vector<MidiInputRoute>& routes);
private:
    void ApplyMidiRoutes();   // push fMidiRoutes onto the current buses
public:

    // After detaching a monitor/live-MIDI source (SetMonitorSource(nullptr) or
    // SetLiveMidi(nullptr)), call this before destroying that source object.
    // Returns true once no in-flight RT callback can still dereference the old
    // pointer (immediately so when the player isn't running). Returns false if
    // quiescence could NOT be confirmed within a bounded wait — in that case the
    // caller must NOT free the source; it must Stop() the engine first (Stop
    // blocks until the RT thread truly quiesces) and only then destroy it.
    // Closes the teardown-ordering use-after-free window on those raw pointers.
    bool QuiesceMonitorInput();

    // Monitor-only: run the output for live MIDI monitoring WITHOUT playing the
    // project (no clips, no playhead advance) — used to hear an armed MIDI track
    // while idle. Set before Load()/Start(). The RT callback renders only the
    // live voices through each armed MIDI track's instrument + fader.
    void SetMonitorOnly(bool on) { fMonitorOnly.store(on); }

    // Output buffer size in frames (per channel); applied at the next Load.
    // Smaller = lower latency, higher xrun risk. Call before Load().
    void SetBufferFrames(size_t n) { if (n >= 32) fBufferFrames = n; }

    // True once the playhead has passed the end of all clips.
    bool  IsFinished() const { return fFinished.load(); }
    Frame Playhead() const { return fPlayhead.load(); }

    // Master output peak (abs) of the last mixed block, per channel, in
    // [0, 1+]. Written by the RT callback, read by the UI meter poll.
    float PeakL() const { return fPeakL.load(); }
    float PeakR() const { return fPeakR.load(); }

    // Per-track output peak (post-FX node level) for track meters. 0 if the
    // track isn't a live node or nothing is playing.
    float TrackPeakL(TrackId id) const;
    float TrackPeakR(TrackId id) const;

    // Master loudness of the mix, updated per block by the RT callback. LUFS
    // (momentary 400 ms / short-term 3 s) and true peak in dBTP. Silence reads
    // Loudness::kSilenceLufs / kSilenceDb.
    float LufsMomentary() const { return fLufsM.load(); }
    float LufsShort()     const { return fLufsS.load(); }
    float TruePeakDb()    const { return fTpDb.load(); }

    float OutputRate() const { return fOutputRate; }

    // Effect metering for the editor. The UI names one track to watch; the RT
    // thread copies each of that track's effects' meter (scalar) + the spectrum
    // of one analyzing effect (e.g. the EQ) into flat storage the UI reads, so
    // the UI never touches the RT-owned effect objects. ~0 = master chain,
    // kInvalidTrackId = disabled.
    void SetMeterFocus(TrackId track) {
        fMeterTrack.store(track, std::memory_order_relaxed);
    }
    static constexpr int kMeterFxMax = 16;
    float MeterGrDb(int fxIndex) const {
        return (fxIndex >= 0 && fxIndex < kMeterFxMax)
               ? fMeterGr[fxIndex].load(std::memory_order_relaxed) : 0.0f;
    }
    // Spectrum of the analyzing effect + which fx index it belongs to. The
    // spectrum array is published with a seqlock: the RT thread rewrites it
    // every block, so the UI retries its copy until it reads a stable frame
    // (avoids a torn mix of two frames — a data race on the plain-float array).
    int MeterSpectrum(float* out, int maxBins, int* fxIndex) const {
        uint32_t g0, g1;
        int c = 0;
        do {
            g0 = fMeterSpecGen.load(std::memory_order_acquire);
            if (g0 & 1u) continue;   // odd = mid-write; retry
            const int n = fMeterSpecN.load(std::memory_order_relaxed);
            c = n < maxBins ? n : maxBins;
            for (int i = 0; i < c; i++) out[i] = fMeterSpec[i];
            g1 = fMeterSpecGen.load(std::memory_order_acquire);
        } while ((g0 & 1u) || g0 != g1);
        if (fxIndex) *fxIndex = fMeterSpecFx.load(std::memory_order_relaxed);
        return c;
    }

private:
    // RT helper: copy the given chain's meters into the flat UI storage. The
    // spectrum write is bracketed by a seqlock generation bump (odd while
    // writing) so the UI reader can detect a torn copy and retry.
    template <class Chain>
    void CaptureFxMeters(const Chain& fx) {
        fMeterSpecGen.fetch_add(1, std::memory_order_acq_rel);   // -> odd
        int specFx = -1, specN = 0;
        for (int i = 0; i < (int)fx.size() && i < kMeterFxMax; i++) {
            if (!fx[i]) { fMeterGr[i].store(0.0f, std::memory_order_relaxed); continue; }
            fMeterGr[i].store(fx[i]->MeterDb(), std::memory_order_relaxed);
            if (specFx < 0) {
                const int n = fx[i]->Spectrum(fMeterSpec, kMeterSpecMax);
                if (n > 0) { specFx = i; specN = n; }
            }
        }
        fMeterSpecN.store(specN, std::memory_order_relaxed);
        fMeterSpecFx.store(specFx, std::memory_order_relaxed);
        fMeterSpecGen.fetch_add(1, std::memory_order_acq_rel);   // -> even
    }

    // RT helper: publish the values of every watch slot that addresses THIS
    // chain. Skipped per slot when nothing changed, so the generation the UI
    // compares is a CHANGE counter and a still insert costs it nothing.
    template <class Chain>
    void CaptureFxWatches(TrackId track, bool masterChain, const Chain& fx) {
        for (int s = 0; s < kWatchSlots; s++) {
            if (fWatchMaster[s].load(std::memory_order_relaxed) != masterChain)
                continue;
            if (!masterChain
                && fWatchTrack[s].load(std::memory_order_relaxed) != track)
                continue;
            const int idx = fWatchFx[s].load(std::memory_order_relaxed);
            if (idx < 0 || idx >= (int)fx.size() || !fx[(size_t)idx]) continue;
            float tmp[kWatchMax];
            const int n = fx[(size_t)idx]->ControlValues(tmp, kWatchMax);
            if (n <= 0) continue;
            if (n == fWatchN[s].load(std::memory_order_relaxed)) {
                bool same = true;
                for (int i = 0; i < n; i++)
                    if (tmp[i] != fWatchValues[s][i]) { same = false; break; }
                if (same) continue;
            }
            fWatchGen[s].fetch_add(1, std::memory_order_acq_rel);   // -> odd
            for (int i = 0; i < n; i++) fWatchValues[s][i] = tmp[i];
            fWatchN[s].store(n, std::memory_order_relaxed);
            fWatchGen[s].fetch_add(1, std::memory_order_acq_rel);   // -> even
        }
    }

    static void PlayTrampoline(void* cookie, void* buffer, size_t size,
                               const media_raw_audio_format& format);
    void FillBuffer(float* out, size_t frames);

    // Drain the live-MIDI ring, advance the held-voice pool, and rebuild
    // fLiveNotes for this block. RT-only; called once per FillBuffer.
    void UpdateLiveVoices(Frame blockStart);

    // A live-monitored note held on the keyboard (or in its release tail).
    struct LiveVoice {
        bool    active    = false;
        bool    releasing = false;   // note-off seen; ringing out the tail
        uint8_t pitch     = 0;
        uint8_t vel       = 0;
        uint8_t channel   = 0;
        Frame   start     = 0;       // engine frame the note-on landed on
        Frame   off       = 0;       // engine frame of note-off (if releasing)
        int32_t source    = 0;       // endpoint the note-on arrived from
    };
    static constexpr int kMaxLiveVoices = 64;

    // One mix bus per audio track: its clips' streams summed into a scratch
    // buffer, then the track's effect chain applied, then added to master.
    // One mix node per track (audio / midi / bus). It sums its content (and,
    // for a bus, its upstream inputs), applies its FX, and routes into `output`
    // (another bus) or the master.
    struct Bus {
        TrackId                               id;
        TrackId                               output = kInvalidTrackId;  // 0 = master
        std::vector<TrackStream*>             streams;   // audio, owned by fStreams
        std::vector<MidiNote>                 notes;     // MIDI (empty otherwise)
        std::vector<MidiClipEvent>            events;    // MIDI CC/PB (channel)
        std::unique_ptr<IInstrument>          instrument;// voice (MIDI)
        // Live mix params: written by the UI thread (UpdateMix) and read by the
        // RT callback (FillBuffer). Atomic + relaxed so a concurrent fader move
        // can't tear a float into a NaN/garbage gain. Independent scalars with no
        // cross-field invariant, so relaxed ordering is sufficient.
        std::atomic<float>                    midiGainL{1.0f};  // equal-power
        std::atomic<float>                    midiGainR{1.0f};
        std::atomic<float>                    busGainL{1.0f};   // bus fader
        std::atomic<float>                    busGainR{1.0f};
        bool                                  isBus   = false;
        std::atomic<bool>                     audible{true};
        bool                                  liveMonitor = false;  // armed MIDI: synth live input
        // Live-input route for this track: which endpoint (Midi Kit producer
        // id, 0 = any) and which MIDI channel (1..16, 0 = all) its monitored
        // notes may come from, so two keyboards drive two tracks independently.
        // Atomic because arming/assignment can change while the player runs.
        std::atomic<int32_t>                  inEndpoint{0};
        std::atomic<int>                      inChannel{0};
        // This bus's share of the live voices, rebuilt per block on the RT
        // thread from the shared voice pool. Reserved at Load so the RT rebuild
        // never allocates.
        std::vector<MidiNote>                 liveNotes;
        std::vector<std::unique_ptr<IEffect>> fx;
        std::vector<EffectType>               fxTypes;  // parallel to fx (SyncFx match)
        // Per-insert slot state, parallel to fx (see model/Effect.h). Atomic
        // because the UI thread pushes them through SyncFx while the RT callback
        // reads them per block — like the fader gains above: independent scalars
        // with no cross-field invariant, so relaxed ordering is sufficient. Held
        // as arrays rather than std::vector<std::atomic<>> (which cannot grow,
        // an atomic being neither copyable nor movable), sized once at Load, the
        // same shape as fNodePeakL/R.
        std::unique_ptr<std::atomic<bool>[]>  fxBypass;
        std::unique_ptr<std::atomic<float>[]> fxMix;
        // Per-insert dry-path delay of that effect's own LatencySamples(). Both
        // the soft-bypass path and the wet/dry blend need the dry signal delayed
        // by exactly that, and they are mutually exclusive, so one line serves
        // both. Length 0 (a plain accumulate, no cost) for every zero-latency
        // effect — which is every built-in but the look-ahead limiter.
        std::vector<FrameDelay>               fxDryDelay;
        // Aux sends: destination node index into fBuses, linear level, and a PDC
        // delay line aligning this send to the dest's input latency. Taps this
        // node's post-FX output. Dest node indices are resolved at Load, so the
        // RT callback does no id lookups. The topo order (built over output +
        // send edges) guarantees each dest is processed after us.
        struct SendTarget {
            size_t     dest;
            float      level;
            FrameDelay delay;   // PDC: aligns this send to dest.inputLatency
        };
        std::vector<SendTarget>               sendTargets;
        // PDC delay line on this node's OUTPUT edge (into `output` or the master
        // sink), aligning it to that destination's input latency. Zero-length
        // (a plain accumulate) when nothing on the path is latent.
        FrameDelay                            outDelay;
        // Gain/pan automation (RT-owned snapshot, copied from the Track at
        // Load). When a lane has points, the engine drives this node's gain/pan
        // per block from the lane (absolute, overriding the static fader);
        // UpdateMix leaves automated nodes alone.
        AutomationLane                        gainAuto;
        AutomationLane                        panAuto;
        std::vector<FxAutoLane>               fxAuto;   // effect-param automation
        float                                 statGain = 1.0f;  // ValueAt default
        float                                 statPan  = 0.0f;
        bool                                  hasAuto  = false;
        // Last MIDI channel gains (CC7 x CC11, placed by CC10) applied at the end
        // of the previous block. The next block ramps from here to its own target
        // so a stepped controller glides instead of clicking. Negative = not yet
        // established (first block after Load/seek), which snaps instead of
        // gliding from a stale value.
        float                                 chanL    = -1.0f;
        float                                 chanR    = -1.0f;

        // The atomic members make Bus non-copyable and suppress the implicit
        // move, but fBuses is a std::vector<Bus> that moves on growth/erase.
        // Hand-write a noexcept move that transfers the atomics by value (all
        // moves happen at Load, single-threaded, before the RT thread starts).
        Bus() = default;
        Bus(Bus&& o) noexcept
            : id(o.id), output(o.output),
              streams(std::move(o.streams)), notes(std::move(o.notes)),
              events(std::move(o.events)),
              instrument(std::move(o.instrument)),
              midiGainL(o.midiGainL.load(std::memory_order_relaxed)),
              midiGainR(o.midiGainR.load(std::memory_order_relaxed)),
              busGainL(o.busGainL.load(std::memory_order_relaxed)),
              busGainR(o.busGainR.load(std::memory_order_relaxed)),
              isBus(o.isBus),
              audible(o.audible.load(std::memory_order_relaxed)),
              liveMonitor(o.liveMonitor),
              inEndpoint(o.inEndpoint.load(std::memory_order_relaxed)),
              inChannel(o.inChannel.load(std::memory_order_relaxed)),
              liveNotes(std::move(o.liveNotes)),
              fx(std::move(o.fx)), fxTypes(std::move(o.fxTypes)),
              fxBypass(std::move(o.fxBypass)), fxMix(std::move(o.fxMix)),
              fxDryDelay(std::move(o.fxDryDelay)),
              sendTargets(std::move(o.sendTargets)),
              outDelay(std::move(o.outDelay)),
              gainAuto(std::move(o.gainAuto)), panAuto(std::move(o.panAuto)),
              fxAuto(std::move(o.fxAuto)),
              statGain(o.statGain), statPan(o.statPan), hasAuto(o.hasAuto),
              chanL(o.chanL), chanR(o.chanR) {}
        Bus& operator=(Bus&& o) noexcept {
            if (this == &o) return *this;
            id = o.id; output = o.output;
            streams = std::move(o.streams); notes = std::move(o.notes);
            events = std::move(o.events);
            instrument = std::move(o.instrument);
            midiGainL.store(o.midiGainL.load(std::memory_order_relaxed), std::memory_order_relaxed);
            midiGainR.store(o.midiGainR.load(std::memory_order_relaxed), std::memory_order_relaxed);
            busGainL.store(o.busGainL.load(std::memory_order_relaxed), std::memory_order_relaxed);
            busGainR.store(o.busGainR.load(std::memory_order_relaxed), std::memory_order_relaxed);
            isBus = o.isBus;
            audible.store(o.audible.load(std::memory_order_relaxed), std::memory_order_relaxed);
            liveMonitor = o.liveMonitor;
            inEndpoint.store(o.inEndpoint.load(std::memory_order_relaxed), std::memory_order_relaxed);
            inChannel.store(o.inChannel.load(std::memory_order_relaxed), std::memory_order_relaxed);
            liveNotes = std::move(o.liveNotes);
            fx = std::move(o.fx); fxTypes = std::move(o.fxTypes);
            fxBypass = std::move(o.fxBypass); fxMix = std::move(o.fxMix);
            fxDryDelay = std::move(o.fxDryDelay);
            sendTargets = std::move(o.sendTargets);
            outDelay = std::move(o.outDelay);
            gainAuto = std::move(o.gainAuto); panAuto = std::move(o.panAuto);
            fxAuto = std::move(o.fxAuto);
            statGain = o.statGain; statPan = o.statPan; hasAuto = o.hasAuto;
            chanL = o.chanL; chanR = o.chanR;
            return *this;
        }
    };

    std::unique_ptr<BSoundPlayer>             fPlayer;
    std::vector<std::unique_ptr<TrackStream>> fStreams;
    std::vector<Bus>                          fBuses;
    std::vector<std::vector<float>>           fNodeBufs;  // one mix buffer per node
    std::vector<size_t>                       fOrder;     // node indices, topo order
    std::unique_ptr<std::atomic<float>[]>     fNodePeakL; // per-node output peak
    std::unique_ptr<std::atomic<float>[]>     fNodePeakR;
    std::vector<std::unique_ptr<IEffect>>     fMasterFx;  // master bus chain
    std::vector<EffectType>                   fMasterFxTypes;  // parallel (SyncFx)
    // Master-chain insert state, same shape and rules as Bus::fxBypass/fxMix/
    // fxDryDelay above. Note fMasterFx DROPS effects that fail to build, so
    // these are parallel to fMasterFx, not to project.masterFx.
    std::unique_ptr<std::atomic<bool>[]>      fMasterFxBypass;
    std::unique_ptr<std::atomic<float>[]>     fMasterFxMix;
    std::vector<FrameDelay>                   fMasterFxDelay;
    // One block of dry signal, held while an insert's wet leg is computed in
    // place. Preallocated at Load (RT never allocates); nodes and the master run
    // sequentially inside one callback, so a single buffer serves them all.
    std::vector<float>                        fScratch;
    std::atomic<IMidiInput*>                  fLiveMidi{nullptr};  // live-monitor input
    // Live-input routes, kept so a rebuild re-applies them. Loop-record restarts
    // the engine at the loop seam, which rebuilds every Bus; without this the
    // demux would silently revert to "every track hears everything" mid-take.
    std::vector<MidiInputRoute>               fMidiRoutes;
    std::atomic<bool>                         fMonitorOnly{false}; // idle monitor mode
    Frame                                     fMonFrame = 0;       // free-running monitor clock
    LiveVoice                                 fVoices[kMaxLiveVoices];
    std::vector<MidiNote>                     fLiveNotes; // rebuilt each block (RT)
    Metronome                                 fMetronome; // click generator
    TempoMap                                  fTempoMap;  // for tempo-synced fx
    double                                     fLastFxBpm = 0.0;  // last pushed bpm
    Loudness                                  fLoudness;  // master BS.1770 meter
    std::atomic<bool>                         fMetronomeOn{false};
    std::atomic<bool>                         fMonitorDim{false};
    std::atomic<bool>                         fMonitorMono{false};
    std::atomic<IMonitorSource*>              fMonSource{nullptr};
    std::atomic<bool>                         fInputMonitor{false};
    // Bumped once per completed RT callback; QuiesceMonitorInput() waits on it
    // to bound the monitor/live-MIDI source teardown UAF window.
    std::atomic<uint64_t>                     fCallbackGen{0};
    // True only while the BSoundPlayer is actually running: set BEFORE
    // fPlayer->Start() and cleared AFTER fPlayer->Stop() returns (Stop blocks
    // until the last callback exits). Unlike fPlaying — which Stop() clears
    // before the player has drained — this is a sound "no callback in flight"
    // signal for QuiesceMonitorInput()'s fast path.
    std::atomic<bool>                         fPlayerRunning{false};
    std::vector<float>                        fMonBuf;   // RT scratch for monitor reads
    // Resampled-monitor state (when the monitor rate != output rate): a linear
    // pull-resampler carried across blocks. RT-only.
    std::vector<float>                        fMonSrc;   // source-rate scratch
    double                                    fMonPhase = 0.0;
    float                                     fMonPrevL = 0.0f, fMonPrevR = 0.0f;
    std::atomic<float>                        fLufsM{Loudness::kSilenceLufs};
    std::atomic<float>                        fLufsS{Loudness::kSilenceLufs};
    std::atomic<float>                        fTpDb{Loudness::kSilenceDb};

    std::atomic<Frame> fPlayhead{0};
    std::atomic<bool>  fPlaying{false};
    std::atomic<bool>  fFinished{false};
    std::atomic<float> fPeakL{0.0f};
    std::atomic<float> fPeakR{0.0f};
    std::atomic<float> fMasterGain{1.0f};

    // Effect meter focus + flat storage (RT writes, UI reads).
    std::atomic<TrackId> fMeterTrack{kInvalidTrackId};
    std::atomic<float>   fMeterGr[kMeterFxMax];   // per-fx scalar meter (GR dB)
    static constexpr int kMeterSpecMax = 256;
    float                fMeterSpec[kMeterSpecMax] = {};
    std::atomic<int>     fMeterSpecN{0};
    std::atomic<int>     fMeterSpecFx{-1};
    std::atomic<uint32_t> fMeterSpecGen{0};   // seqlock generation (odd = writing)

    // Watched inserts (the open native editors' inserts) -- see SetFxWatch.
    // fWatchN < 0 means nothing has been published yet, so the first block
    // after a watch is set always publishes and the editor is never left
    // showing a value the engine does not have.
    // Zero-initialised, which is already "no watch": fWatchTrack 0 is
    // kInvalidTrackId (no bus has it) and fWatchMaster false (the master chain
    // is not scanned for it). SetFxWatch is what puts a real watch in a slot.
    std::atomic<TrackId>  fWatchTrack[kWatchSlots] {};
    std::atomic<bool>     fWatchMaster[kWatchSlots] {};
    std::atomic<int>      fWatchFx[kWatchSlots] {};
    float                 fWatchValues[kWatchSlots][kWatchMax] = {};
    std::atomic<int>      fWatchN[kWatchSlots] {};
    std::atomic<uint32_t> fWatchGen[kWatchSlots] {};   // seqlock (odd = writing)
    Frame  fStartFrame = 0;   // playhead position playback begins at
    Frame  fEndFrame   = 0;
    float  fOutputRate = 48000.0f;
    size_t fBufferFrames = 512;   // output buffer frames/channel (~10.7ms@48k)
};

} // namespace daw
