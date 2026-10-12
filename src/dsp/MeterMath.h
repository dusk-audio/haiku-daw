// MeterMath — the kit-free maths behind every level meter in the app
// (plan M3.3): the dB scale a meter is drawn on, the peak hold, the sliding
// RMS/correlation window, and the clip latch.
//
// What is NOT here: the levels a meter warns and clips at. Those are one set,
// owned by src/ui/Theme.h (kMeterWarn / kMeterClip / MeterColor) since M1.2 —
// a meter that invents its own thresholds is exactly the bug that fix ended.
// This header owns the level -> position mapping and the state machines; the
// only level it names is kMeterFullScale, because digital full scale is a
// definition rather than a taste, and Theme.h's kMeterClip IS that number.
//
// Kit-free (STL only), C++17, host-tested (tests/meter_tests.cpp). The engine
// and the recorder run StereoMeter on the audio thread, so Process() is
// allocation-free, lock-free and branch-cheap once Prepare() has run; PeakHold
// and ClipLatch are UI-side state machines driven by the ~60 Hz meter poll.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace daw {

// --- the dB scale ---------------------------------------------------------

// A meter's bottom and top. -60 dBFS at the bottom leaves the noise floor off
// the scale; +6 dB of headroom above 0 dBFS means a clipped signal still has
// somewhere to go, so "pinned at the top" and "1 dB over" look different.
constexpr float kMeterMinDb = -60.0f;
constexpr float kMeterMaxDb =   6.0f;

// Silence / a non-positive level reads this finite floor, never -inf (the
// same rule the loudness meter follows: Loudness::kSilenceDb).
constexpr float kMeterSilenceDb = -120.0f;

// Digital full scale: the level a clip lamp latches at. The RT side latches
// its flag here and Theme.h's kMeterClip is this same number, so the lamp and
// the meter's red zone can never disagree.
//
// Not quite 1.0: int16 material cannot reach it. Every reader in this app
// scales by 32768 (WavSource, Recorder), so a sample that hit the converter's
// ceiling reads 32767/32768 — and a latch at exactly 1.0 would stay dark for
// precisely the take that clipped. The ceiling of the int16 range is what
// "full scale" means for every source we read (0.0003 dB below 1.0).
constexpr float kMeterFullScale = 1.0f - 1.0f / 32768.0f;

// The clip flags the audio thread publishes: one atomic word, one bit per
// channel. The RT side sets a bit with fetch_or when a block reaches full
// scale; the UI reads the word and clears it with fetch_and, so a clip that
// happens between the read and the clear is not lost.
constexpr uint32_t kMeterClipL    = 1u << 0;
constexpr uint32_t kMeterClipR    = 1u << 1;
constexpr uint32_t kMeterClipBoth = kMeterClipL | kMeterClipR;

// Where the labelled ticks sit on the scale (dB), top to bottom in the arrays
// the views walk. Both the mixer strip's scale and the master meter's draw
// from this one list.
constexpr int   kMeterTickCount = 5;
constexpr float kMeterTicksDb[kMeterTickCount] = { 0.0f, -6.0f, -12.0f,
                                                   -24.0f, -48.0f };

// Linear amplitude -> dBFS. <= 0 (and NaN) reads the finite floor.
inline float LinToDb(float lin) {
    if (!(lin > 0.0f)) return kMeterSilenceDb;
    return 20.0f * std::log10(lin);
}

// dBFS -> meter fraction [0, 1] over [kMeterMinDb, kMeterMaxDb].
inline float DbToFrac(float db) {
    if (!(db > kMeterMinDb)) return 0.0f;   // NaN / below the scale -> empty
    if (db >= kMeterMaxDb) return 1.0f;
    return (db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb);
}

// The one call a view needs: a linear peak -> where the bar ends.
inline float LinToFrac(float lin) { return DbToFrac(LinToDb(lin)); }

// --- typed level entry (M3.4) -------------------------------------------

// A fader's own range, in dB: what a typed level is clamped to. Wider than the
// meter scale on purpose — a fader may push a quiet track up past unity — and
// -inf is expressed as the bottom of it.
constexpr float kFaderMinDb = -96.0f;
constexpr float kFaderMaxDb =  12.0f;

// Linear gain for a dB figure. The floor (and anything below it) is true
// silence, not a very small number: this is what "-inf" in a level field means.
inline float GainFromDb(float db) {
    if (!(db > kFaderMinDb)) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

// Parse what a user typed into a level field: "-6", "+3.5", "0", "-inf",
// "-Inf", "-oo" (they mean silence), or a bare number with blanks around it.
// False means "not a level" — the caller keeps the old value and says so. A
// finite value is clamped to [kFaderMinDb, kFaderMaxDb].
inline bool ParseDecibels(const std::string& text, float* db) {
    // Blanks around the figure are fine; blanks INSIDE it are not ("6 7" is
    // not a level, and collapsing them would make it 67).
    std::string t = text;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
        t.erase(t.begin());
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    if (t.empty()) return false;
    // The inf spellings: "-inf", "-oo", "-∞".
    std::string lower;
    for (char c : t) lower.push_back((char)std::tolower((unsigned char)c));
    if (lower == "-inf" || lower == "-oo"
        || lower == "-\xe2\x88\x9e" || lower == "-infinity") {
        if (db) *db = kFaderMinDb;
        return true;
    }
    if (lower == "+inf" || lower == "inf") return false;   // not a level
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || end == nullptr || *end != '\0') return false;
    if (std::isnan(v)) return false;
    double c = v;
    if (c < (double)kFaderMinDb) c = (double)kFaderMinDb;
    if (c > (double)kFaderMaxDb) c = (double)kFaderMaxDb;
    if (db) *db = (float)c;
    return true;
}

// --- peak hold ------------------------------------------------------------

// A peak-hold marker: it jumps to a new peak, stays there for kHoldUs, then
// falls at kDecayDbPerSec (dB per second is how every hardware meter reads;
// a fraction-per-second rate would move differently at each scale).
class PeakHold {
public:
    static constexpr int64_t kHoldUs        = 1500 * 1000;   // 1.5 s
    static constexpr float   kDecayDbPerSec = 20.0f;

    // One meter-poll tick. `frac` is the bar's current position [0, 1];
    // `nowUs` is a monotonic clock in microseconds (system_time() on Haiku,
    // a counter in the tests -- hence int64_t, not the kit's bigtime_t).
    void Update(float frac, int64_t nowUs) {
        if (!(frac >= 0.0f)) frac = 0.0f;        // NaN -> empty
        if (frac > 1.0f) frac = 1.0f;
        if (frac >= fFrac) {                     // a new peak: jump and hold
            fFrac  = frac;
            fUntil = nowUs + kHoldUs;
            fLast  = nowUs;
            return;
        }
        if (nowUs < fLast) fLast = nowUs;        // clock went backwards
        if (nowUs <= fUntil) { fLast = nowUs; return; }   // still holding
        const int64_t from = std::max(fLast, fUntil);
        const double dt = (double)(nowUs - from) / 1.0e6;
        const float fall = (float)(dt * (double)kDecayDbPerSec
                                   / (double)(kMeterMaxDb - kMeterMinDb));
        fFrac = std::max(frac, fFrac - fall);    // never under the live bar
        fLast = nowUs;
    }

    float Frac() const { return fFrac; }
    void  Reset() { fFrac = 0.0f; fUntil = 0; fLast = 0; }

private:
    float   fFrac  = 0.0f;
    int64_t fUntil = 0;   // hold until this time
    int64_t fLast  = 0;   // last tick (so the decay is tick-rate agnostic)
};

// --- clip latch -----------------------------------------------------------

// The level test a clip lamp latches on: |peak| reached the clip level. The
// audio thread runs this as a float compare against kMeterFullScale before it
// sets its own flag bit; the UI latches the bits it is handed.
inline bool ReachesClipLevel(float peak, float clipLevel = kMeterFullScale) {
    return peak >= clipLevel;
}

// Latches until Reset() — the click on the meter. The UI's latch: the audio
// thread's own flag lives in an atomic word the UI clears.
class ClipLatch {
public:
    void Latch(bool hit) { if (hit) fClipped = true; }
    bool Clipped() const { return fClipped; }
    void Reset() { fClipped = false; }

private:
    bool fClipped = false;
};

// --- sliding RMS + correlation -------------------------------------------

// Stereo RMS over a sliding window, and the correlation coefficient between
// the channels over the same window.
//
// Both come from one pass: the per-block sums of L^2, R^2 and L*R, kept in a
// fixed ring that Prepare() allocates (so Process() only does arithmetic —
// this is the meter the RT callback runs). The window is the sum of the
// blocks still inside it; eviction is per block, so the effective window is
// the requested length rounded up to the block that straddles its end.
//
// Correlation is  Sigma(l*r) / sqrt(Sigma(l^2) * Sigma(r^2)): +1 for a mono
// (perfectly in-phase) signal, -1 for the same signal with one channel
// inverted, ~0 for uncorrelated channels, and 0 when there is no signal at
// all (no division by zero, and "silent" does not pretend to be correlated).
class StereoMeter {
public:
    void Prepare(double sampleRate, float windowSeconds = 0.3f) {
        if (!(sampleRate > 0.0)) sampleRate = 48000.0;
        if (!(windowSeconds > 0.01f)) windowSeconds = 0.3f;
        fSampleRate  = sampleRate;
        fWindowSamps = (long long)(windowSeconds * sampleRate + 0.5);
        // One slot per block that could straddle the window: the smallest
        // block the engine uses is far bigger than 64 frames, so 64 slots is
        // a generous ceiling over any sane window/block pair, and the ring is
        // sized from the window anyway.
        std::size_t slots = (std::size_t)(fWindowSamps / 64) + 4;
        if (slots < 8)    slots = 8;
        if (slots > 4096) slots = 4096;
        fRing.assign(slots, Block{});
        Reset();
    }

    void Reset() {
        for (Block& b : fRing) b = Block{};
        fHead = 0; fCount = 0;
        fSumL = fSumR = fSumLR = 0.0;
        fSamps = 0;
        fRmsL = fRmsR = 0.0f;
        fCorr = 0.0f;
    }

    // One interleaved-stereo block. RT-safe: no allocation, no locks, no I/O.
    void Process(const float* stereo, int frames) {
        if (stereo == nullptr || frames <= 0) return;
        if (fRing.empty()) Prepare(fSampleRate, 0.3f);   // never prepared
        Block b;
        for (int i = 0; i < frames; i++) {
            const double l = stereo[i * 2 + 0];
            const double r = stereo[i * 2 + 1];
            b.l2 += l * l;
            b.r2 += r * r;
            b.lr += l * r;
        }
        b.n = frames;

        // Evict from the BACK of the window (the oldest block) while the
        // window is over its length.
        while (fCount > 0 && fSamps + (long long)frames > fWindowSamps) {
            const int tail = (fHead - fCount + (int)fRing.size())
                             % (int)fRing.size();
            const Block& old = fRing[(std::size_t)tail];
            fSumL -= old.l2; fSumR -= old.r2; fSumLR -= old.lr;
            fSamps -= old.n;
            fCount--;
        }
        fRing[(std::size_t)fHead] = b;
        fHead = (fHead + 1) % (int)fRing.size();
        fCount++;
        fSumL += b.l2; fSumR += b.r2; fSumLR += b.lr;
        fSamps += frames;

        fRmsL = fSamps > 0 ? (float)std::sqrt(std::max(0.0, fSumL / (double)fSamps)) : 0.0f;
        fRmsR = fSamps > 0 ? (float)std::sqrt(std::max(0.0, fSumR / (double)fSamps)) : 0.0f;

        const double denom = std::sqrt(std::max(0.0, fSumL * fSumR));
        if (denom > 0.0) {
            double c = fSumLR / denom;
            if (c >  1.0) c =  1.0;
            if (c < -1.0) c = -1.0;
            fCorr = (float)c;
        } else {
            fCorr = 0.0f;
        }
    }

    float RmsL() const { return fRmsL; }
    float RmsR() const { return fRmsR; }
    float Correlation() const { return fCorr; }

    // The window actually held, in samples (host tests + diagnostics).
    long long WindowSamples() const { return fSamps; }

private:
    struct Block {
        double l2 = 0.0, r2 = 0.0, lr = 0.0;
        int    n  = 0;
    };

    std::vector<Block> fRing;
    int  fHead  = 0;      // next slot to write
    int  fCount = 0;      // blocks in the window
    double fSumL = 0.0, fSumR = 0.0, fSumLR = 0.0;
    long long fSamps = 0;      // samples in the window
    long long fWindowSamps = 0;
    double fSampleRate = 48000.0;
    float fRmsL = 0.0f, fRmsR = 0.0f, fCorr = 0.0f;
};

} // namespace daw
