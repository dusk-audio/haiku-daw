// Stretch — streaming WSOLA time-stretch (M7.2), kit-free.
//
// Plays a clip slower or faster without changing its pitch: the SAME samples
// are emitted, but the read pointer walks through the source at a different
// speed, and the seams between segments are crossfaded inside the overlap-add.
// WSOLA (waveform-similarity OLA, Verhelst & Roelands 1993) rather than a phase
// vocoder because it keeps percussive attacks intact and needs no FFT — the
// design decision is pinned in docs/agent-prompts/06-timestretch.md.
//
// It is a PUSH-MODEL streamer: the caller pushes source frames, pulls output
// frames, and may chunk both however it likes — the output does not depend on
// the chunking. The live engine runs one on its disk thread (never on the audio
// thread: the search is real work, and its lookahead is fine off-RT), and the
// exporter runs the identical class offline, which is what makes a bounce and
// playback agree.
//
// ALGORITHM CONSTANTS (all scaled by the sample rate, so 44.1 k and 96 k behave
// the same):
//
//   window      L  = 50 ms   (2400 frames @ 48 k, forced even)
//   synthesis hop Hs = L / 2 (25 ms; 50 % overlap)
//   search      R  = 10 ms   (480 frames @ 48 k, +/-)
//
// Two overlapping Hann windows a synthesis hop apart sum to EXACTLY 1 (a
// periodic Hann, not the symmetric one, and hop = L/2 — that is why the hop is
// pinned to half the window), so the overlap-add is unity-gain by construction
// and there is nothing to normalise.
//
// THE GRID. Synthesis frame k covers output frames [k*Hs, k*Hs + L) and reads
// the source from
//
//     Q(k) = round(k * Hs / ratio)          the frame's NOMINAL read start
//     a(k) = Q(k) + delta(k)                where it actually reads
//
// with delta(k) in [-R, +R] chosen by cross-correlation against the PREVIOUS
// FRAME'S ACTUAL READ advanced by one synthesis hop, x[a(k-1) + Hs ...) — i.e.
// against the samples the previous frame really did place in the overlap. That
// is what makes the crossfade coherent: the falling half of frame k-1 and the
// rising half of frame k carry the same waveform, so they sum to the source
// rather than to whatever two mismatched phases sum to.
//
// (The alternative — matching each candidate against the previous frame's
// NOMINAL continuation, x[Q(k-1) + Hs ...] — makes every correction a function
// of the frame index alone, which is exactly reproducible after a seek. It is
// also audibly wrong: measured on a 440 Hz sine at ratio 0.8, the overlap
// regions cancel, the window RMS dipping to 0.31 and to 0.25 at ratio 2.0
// against the source's 0.354, a wobble at the hop rate. So the correction is
// chained, as it is in every working WSOLA implementation.)
//
// The grid itself is anchored at the STREAM ORIGIN (source frame 0 == output
// frame 0) and is what Reset(sourcePos) restores: the next output frame is the
// one source position sourcePos maps to, round(sourcePos * ratio), and the
// frames built from there are the same grid frames continuous playback would
// have built — no restart of the hop phase, no drift against the timeline. What
// a reset re-seeds rather than reproduces is the CHAINED correction above: a
// resumed run's segment choices are re-aligned from the grid. They stay within
// the search radius of the frames continuous playback read (every frame reads
// within R of its nominal, by construction), so the material is the same to
// within R — and a run that starts at the clip origin, which is both the
// exporter's and the engine's common case, is bit-identical to any other run
// that starts there.
//
// A reset also cannot invent the tail of the frame before it (that frame's
// samples are behind the pushed input), so a reset run ramps in over the
// overlap (L - Hs = 25 ms) unless the caller supplies the earlier material —
// see WarmupFrames() for how a caller that wants a gapless resume does that.
//
// Threading: Prepare() allocates and is off-RT; Process()/Flush() do arithmetic
// on preallocated state only (the engine calls them on a disk thread, never in
// the audio callback). No locks, no I/O anywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace daw {

class Stretcher {
public:
    static constexpr int kMaxChannels = 2;

    // What one Process() call did: input frames taken, output frames written.
    struct Result {
        int inConsumed = 0;
        int outProduced = 0;
    };

    Stretcher() = default;

    // Size the stream and fix the ratio. `ratio` is an OUTPUT DURATION
    // multiplier: 2.0 = twice as long (half speed), 0.5 = twice as fast; 1.0 is
    // an exact copy path (see IsIdentity). Not reentrant with Process().
    void Prepare(double sampleRate, int channels, double ratio);

    // Begin a run. `sourcePos` is the stream-relative source frame the first
    // input frame pushed afterwards will be — i.e. the caller's decode start —
    // and the next output frame produced is the one for source position
    // `sourcePos` (output index round(sourcePos * ratio), so a seek lands on
    // the timeline frame it asked for). Everything before it is dropped: this
    // is not a rewind, it is "as if playback had just reached here".
    void Reset(int64_t sourcePos);

    // Push up to `inFrames` (interleaved) and pull up to `outCap` frames into
    // `out` (interleaved, same channel count). Both may be 0. Whatever is not
    // consumed must be pushed again — the internal history is bounded (it holds
    // the search window plus the frame being built), so a caller with more
    // input than that loops. Returning 0 produced while consuming everything
    // means "needs more input".
    Result Process(const float* in, int inFrames, float* out, int outCap);

    // No more input will arrive: the material ends here, and anything the
    // remaining frames' windows still reach for reads as silence. That is what
    // gives the stream a defined end (the last frame whose nominal read start
    // is inside the material, plus the pending tail — the output length lands
    // within one synthesis hop of in * ratio).
    void Flush();

    // --- what the stream is doing ---------------------------------------

    double Ratio() const { return fRatio; }
    bool   IsIdentity() const { return fIdentity; }
    int    Channels() const { return fChannels; }
    // Stream-relative index of the next output frame to be produced.
    int64_t OutputIndex() const { return fNextOut; }
    // Highest stream-relative source frame read so far (one past the last frame
    // looked at). A caller that must not read past a clip's trimmed span can
    // watch this; the engine's span cap uses its own fed-frame count.
    int64_t ReadPosition() const { return fReadHi; }
    bool   Flushed() const { return fFlushed; }

    // The synthesis constants, in frames, at the prepared sample rate.
    int HopFrames() const { return fHop; }
    int WindowFrames() const { return fWindow; }
    int SearchFrames() const { return fSearch; }

    // How many output frames a caller should start BEFORE the position it wants
    // its resumed run to begin at, when it can push input that far back (a seek
    // into a clip whose earlier material is still there). Reset() at
    // (target - WarmupFrames()) makes the stretcher build the frame before the
    // target's, so the run resumes with a real OLA tail instead of the ramp-in
    // Reset() alone would give, and the caller drops the frames before its
    // target. Costs one extra synthesis hop of work, once.
    int64_t WarmupFrames() const;

private:
    // The nominal read start of grid frame k.
    int64_t GridPos(int64_t k) const;

    // Whether grid frame k can be built yet: its whole search window is in the
    // history (or the stream is flushed, when the rest reads as silence).
    bool FrameReady(int64_t k) const;

    // The offset in [-R, R] this frame reads at: the candidate whose overlap
    // best matches the previous frame's own samples one hop on (fPrevRead +
    // Hs), or 0 when there is no previous frame / not enough of the target to
    // judge.
    int ChooseDelta(int64_t k);

    // Drop history the pending frames can no longer read.
    void Prune();

    // One history sample (0 outside it: before the pushed start and past the
    // end both read as silence).
    float Sample(int c, int64_t at) const;

    // Build the next synthesis frame into fHopBuf (its hop) and fTail (the
    // overlap the next hop needs); `skipAll` builds it for its tail alone.
    void BuildFrame(int64_t k, bool skipAll);
    bool BuildNext();                     // one step of the emit state machine
    int  Drain(float* out, int cap);      // copy out what is built and ready

    // config
    double  fRatio    = 1.0;
    int     fChannels = 2;
    bool    fIdentity = true;
    bool    fPrepared = false;

    // synthesis constants
    int fWindow = 0;    // L
    int fHop    = 0;    // Hs = L/2
    int fSearch = 0;    // R
    int fOverlap = 0;   // L - Hs
    std::vector<float> fWin;   // periodic Hann, length L

    // input history: stream-relative frames [fHistBase, fHistBase + fCount)
    std::vector<float> fHist;
    int     fHistCap  = 0;    // frames it can hold
    size_t  fCount    = 0;    // frames held
    int64_t fHistBase = 0;
    int64_t fInputEnd = 0;    // one past the last frame pushed
    int64_t fPushed   = 0;    // frames pushed since Reset
    bool    fFlushed  = false;

    // output state
    int64_t fNextOut  = 0;    // stream-relative output index of the next frame
    int64_t fBuildIdx = 0;    // next grid frame to build
    int     fPendingSkip = 0; // frames of the next built hop to drop (mid-hop reset)
    bool    fWarm     = false;// build the frame before fBuildIdx first, for its tail
    bool    fDone     = false;// the stream is spent (nothing more to produce)
    int64_t fPrevRead = 0;    // where the last built frame actually read from
    std::vector<float> fTail; // pending OLA tail: hop frames x channels
    std::vector<float> fHopBuf;   // the hop being drained: hop frames x channels
    int     fHopFilled  = 0;
    int     fHopEmitted = 0;

    int64_t fReadHi = 0;      // telemetry (tests): highest source frame read
};

} // namespace daw
