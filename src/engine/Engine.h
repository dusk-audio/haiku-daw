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
#include "../model/RoutingGraph.h"
#include "../dsp/IEffect.h"
#include "../dsp/Loudness.h"
#include "../synth/Synth.h"
#include "../midi/IMidiInput.h"
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
    Frame       fFadeIn;        // fade-in length (timeline frames)
    Frame       fFadeOut;       // fade-out length (timeline frames)
    float       fClipGain = 1.0f;  // per-clip linear gain
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
    Engine() {
        for (int i = 0; i < kMeterFxMax; i++)
            fMeterGr[i].store(0.0f, std::memory_order_relaxed);
    }
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
        Instrument                            instrument;// synth voice (MIDI)
        float                                 midiGainL = 1.0f;  // equal-power
        float                                 midiGainR = 1.0f;
        float                                 busGainL  = 1.0f;  // bus fader
        float                                 busGainR  = 1.0f;
        bool                                  isBus   = false;
        bool                                  audible = true;
        bool                                  liveMonitor = false;  // armed MIDI: synth live input
        std::vector<std::unique_ptr<IEffect>> fx;
        std::vector<EffectType>               fxTypes;  // parallel to fx (SyncFx match)
        // Aux sends: (destination node index into fBuses, linear level). Taps
        // this node's post-FX output. Dest node indices are resolved at Load,
        // so the RT callback does no id lookups. The topo order (built over
        // output + send edges) guarantees each dest is processed after us.
        std::vector<std::pair<size_t, float>> sendTargets;
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
    std::vector<float>                        fScratch;   // (unused after routing)
    Synth                                     fSynth;     // MIDI voice renderer
    std::atomic<IMidiInput*>                  fLiveMidi{nullptr};  // live-monitor input
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
    Frame  fStartFrame = 0;   // playhead position playback begins at
    Frame  fEndFrame   = 0;
    float  fOutputRate = 48000.0f;
    size_t fBufferFrames = 512;   // output buffer frames/channel (~10.7ms@48k)
};

} // namespace daw
