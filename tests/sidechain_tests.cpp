// Host-buildable tests for external sidechain routing (package 05): a track's
// post-fader output keying another track's Compressor or Gate insert.
//
// Everything here runs through the offline Exporter, which builds the same
// graph, taps the same post-fader point and solves the same PDC latencies as
// the RT engine — the engine's own block loop is Haiku-only, so the bounce is
// where this feature's correctness is executable. Bounces are 32-bit float and
// are compared sample-exactly wherever the two renders are structurally
// identical (extKey off vs on with no source, a muted key, a limiter on the
// key path), and against hand-computed one-pole arithmetic where the envelope
// IS the point.
//
// The signals are deliberately level steps rather than tones: the detectors
// read |peak|, so a constant level makes the hand-built expectation exact
// (detDb = 20*log10(0.5) = -6.0206 dB) and reading a gain envelope back out of
// the bounce is a division, not a filter. The kick is panned hard LEFT so the
// bass — centre-panned — can be read alone on the RIGHT channel; that is what
// makes "the level the envelope is applied to" measurable at all.

#include "../src/dsp/Compressor.h"
#include "../src/dsp/EffectFactory.h"
#include "../src/dsp/Gate.h"
#include "../src/dsp/SidechainKey.h"
#include "../src/engine/Exporter.h"
#include "../src/engine/WavSource.h"
#include "../src/engine/WavWriter.h"
#include "../src/model/Effect.h"
#include "../src/model/Project.h"
#include "../src/model/ProjectIO.h"
#include "../src/model/RoutingGraph.h"   // kRoutingMaster

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static const double  SR    = 48000.0;
static const int64_t N     = 24000;    // 0.5 s of timeline
static const int64_t K     = 6000;     // kick onset
static const int64_t KDUR  = 2400;     // kick length (50 ms)
static const int64_t ATT   = 48;       // 1 ms attack tau, in frames
static const int64_t REL   = 960;      // 20 ms release tau, in frames

// A WAV holding `level` on both channels over frames [from, to), silence
// outside. 32-bit float, so a level round-trips exactly.
static bool WriteLevelWav(const std::string& path, int rate, int64_t total,
                          float level, int64_t from, int64_t to) {
    std::vector<float> d((size_t)total * 2, 0.0f);
    for (int64_t i = std::max<int64_t>(0, from); i < std::min(total, to); i++) {
        d[i * 2 + 0] = level;
        d[i * 2 + 1] = level;
    }
    WavWriter w;
    if (!w.OpenFormat(path, rate, 2, 32, /*floatOut=*/true)) return false;
    if (!w.WriteFloat(d.data(), d.size(), /*dither=*/false)) return false;
    return w.Close();
}

static std::vector<float> ReadAll(const std::string& path, int64_t* frames) {
    std::vector<float> out;
    WavSource s;
    if (!s.Open(path)) { *frames = 0; return out; }
    const float* c = nullptr; size_t f = 0;
    while (s.ReadChunk(&c, &f)) out.insert(out.end(), c, c + f * 2);
    *frames = (int64_t)(out.size() / 2);
    return out;
}

static std::string KickWav() { return "/tmp/haiku_daw_sc_kick.wav"; }
static std::string BassWav() { return "/tmp/haiku_daw_sc_bass.wav"; }

// Bass level: the right channel, where the hard-left kick never appears.
static float RightAt(const std::vector<float>& d, int64_t i) {
    return (i >= 0 && i * 2 + 1 < (int64_t)d.size()) ? d[i * 2 + 1] : 0.0f;
}
static float LeftAt(const std::vector<float>& d, int64_t i) {
    return (i >= 0 && i * 2 + 1 < (int64_t)d.size()) ? d[i * 2] : 0.0f;
}

// The compressor's own arithmetic, written out so the expectations below are
// visibly the documented law and not magic constants.
static double DuckGain(double peak, double threshDb, double ratio) {
    const double detDb = 20.0 * std::log10(peak);
    double redDb = 0.0;
    if (detDb > threshDb) redDb = (detDb - threshDb) * (1.0 - 1.0 / ratio);
    return std::pow(10.0, -redDb / 20.0);
}

// A compressor with the appended extKey slot set, and (optionally) a source to
// wire once the project's track ids exist. Threshold -30 dB: the kick's
// mono-summed key (0.5, see below) is 13.98 dB over it and the bass's own
// post-fader level (0.5*0.7071 = 0.3536) is 9.03 dB over it, so keyed and
// internal detection are both deep AND different — a test that confused the
// two could not pass.
static EffectDesc Comp(bool extKey) {
    EffectDesc d = CompressorDesc(-30.0f, 4.0f, 1.0f, 20.0f, 0.0f);
    if (extKey) {
        d.params.resize((size_t)kExtKeySlot + 1, 0.0f);
        d.params[(size_t)kExtKeySlot] = 1.0f;
    }
    return d;
}
static EffectDesc GateKeyed(EffectDesc d) {
    d.params.resize((size_t)kExtKeySlot + 1, 0.0f);
    d.params[(size_t)kExtKeySlot] = 1.0f;
    return d;
}

// The standard two-track project: "bass" carries `bassFx` and is keyed from
// "kick" (created SECOND, so nothing here can lean on track order to get the
// ordering right — the key edge has to).
//
// The kick is a 0.5 step panned HARD LEFT: the bass can then be read alone on
// the right channel, where the key contributes exactly zero (0.5 is also what
// keeps it a pure delay under a look-ahead limiter, which is what the PDC case
// below needs). Its DETECTED level is not 0.5 but 0.25 — the key reaches the
// detector through the track's post-fader (panned) output, and the detector
// mono-sums it (dsp/SidechainKey.h: 0.5*(0.5 + 0) = 0.25). Keying from an
// off-centre track therefore reads 6 dB below the track's own level; that is
// the documented consequence, and the hand-built numbers below use the 0.25.
struct Duo {
    Project p;
    TrackId bass = 0, kick = 0;
};

static void EnsureFixtures() {
    static bool wrote = false;
    if (wrote) return;
    WriteLevelWav(KickWav(), (int)SR, N, 0.5f, K, K + KDUR);   // -> key 0.25
    WriteLevelWav(BassWav(), (int)SR, N, 0.5f, 0, N);
    wrote = true;
}

static Duo MakeDuo(std::vector<EffectDesc> bassFx = {},
                   bool muteKick = false) {
    EnsureFixtures();
    Duo d;
    d.p.sampleRate = SR;
    d.p.masterGain = 1.0f;
    d.bass = d.p.NextTrackId();
    d.kick = d.p.NextTrackId();
    for (auto id : { d.bass, d.kick }) {
        Track t;
        t.id = id; t.type = TrackType::Audio;
        t.name = (id == d.bass) ? "bass" : "kick";
        t.gain = 1.0f;
        t.pan  = (id == d.bass) ? 0.0f : -1.0f;   // kick hard left
        Clip c;
        c.id = d.p.NextClipId();
        c.startFrame = 0; c.lengthFrames = N; c.sourceOffset = 0;
        c.sourcePath = (id == d.bass) ? BassWav() : KickWav();
        t.clips.push_back(c);
        d.p.AddTrack(t);
    }
    if (Track* b = d.p.FindTrack(d.bass)) b->fx = std::move(bassFx);
    if (Track* k = d.p.FindTrack(d.kick)) k->muted = muteKick;
    return d;
}

// Point the chain's first insert at the kick track as its key.
static void WireKey(Duo& d, size_t fxIndex = 0) {
    Track* b = d.p.FindTrack(d.bass);
    if (b && fxIndex < b->fx.size()) b->fx[fxIndex].sidechainSource = d.kick;
}

static std::vector<float> Bounce(const Project& p, const std::string& tag) {
    const std::string path = "/tmp/haiku_daw_sc_out_" + tag + ".wav";
    std::remove(path.c_str());
    std::vector<float> out;
    if (!ExportWav(p, path, SR, {ExportFormat{32}})) return out;
    int64_t n = 0;
    out = ReadAll(path, &n);
    std::remove(path.c_str());
    return out;
}

static bool SameSamples(const std::vector<float>& a,
                        const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i] != b[i]) return false;
    return true;
}

// First frame in [from, to) where the right channel drops below `frac * ref`.
static int64_t FirstBelow(const std::vector<float>& d, int64_t from, int64_t to,
                          float ref, float frac) {
    for (int64_t i = std::max<int64_t>(0, from); i < to; i++)
        if (std::fabs(RightAt(d, i)) < ref * frac) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// 1. The feature: a kick on track A ducks a compressor on track B.
// ---------------------------------------------------------------------------
static void TestKickDucksBass() {
    std::printf("-- kick keys the bass compressor\n");
    const Duo dryDuo = MakeDuo();
    const std::vector<float> dry = Bounce(dryDuo.p, "dry");
    CHECK(!dry.empty());
    const float L = RightAt(dry, 1000);            // bass alone: 0.5*0.7071
    CHECK(std::fabs(L - 0.353553f) < 1e-4f);
    CHECK(LeftAt(dry, K + KDUR / 2) > L + 0.4f);   // the kick, left, while it plays

    Duo duo = MakeDuo({ Comp(true) });
    WireKey(duo);
    const std::vector<float> out = Bounce(duo.p, "keyed");
    CHECK(!out.empty());
    if (out.empty()) return;

    // Hand-built expectation: the detector's key is the kick's post-fader
    // mono sum, 0.5*(0.5 + 0.0) = 0.25 -> -12.04 dB; the threshold is -30 dB,
    // so the reduction is (−12.04 + 30) * 0.75 = 13.47 dB and the gain
    // 10^-(13.47/20) = 0.2122.
    const double g = DuckGain(0.25, -30.0, 4.0);
    CHECK(std::fabs(g - 0.2122) < 0.0005);         // what the law says

    // (a) Before the kick the key is silent, the compressor does not reduce,
    // and the envelope never moves off exactly 1.0 — so the bass in that
    // stretch is bit-identical to the dry render's.
    bool sameBefore = true;
    for (int64_t i = 0; i < K - 2; i++)
        if (RightAt(out, i) != RightAt(dry, i)) { sameBefore = false; break; }
    CHECK(sameBefore);
    CHECK(RightAt(out, K + KDUR / 2) < L * 0.5f);   // ...and the kick DOES duck

    // (b) Timing: the duck starts with the kick (not before it, and within one
    // attack tau), holds through it, recovers with the release tau.
    const int64_t onset = FirstBelow(out, 0, K + KDUR, L, 0.9f);
    CHECK(onset >= K - 2);
    CHECK(onset <= K + ATT + 8);
    CHECK(RightAt(out, K + KDUR + ATT) < L * 0.9f);        // still ducked after
    CHECK(RightAt(out, K + KDUR + 3 * REL) > L * 0.9f);    // released by 3 taus

    // (c) The envelope's VALUE. Past the attack (5 taus in) it sits on the
    // target gain; during the first milliseconds it is the donor's one-pole
    // gain smoothing exactly — fEnv = target + (1-target)*coef^n with
    // coef = exp(-1/(t*Fs)) — not a fitted curve.
    CHECK(std::fabs(RightAt(out, K + KDUR - 480) / L - g) < 0.005);
    const double coef = std::exp(-1.0 / (1.0 * 0.001 * SR));
    for (int64_t n : { ATT, 2 * ATT, 5 * ATT }) {
        const double want = g + (1.0 - g) * std::pow(coef, (double)n);
        CHECK(std::fabs(RightAt(out, K + n) / L - want) < 2e-3);
    }
}

// ---------------------------------------------------------------------------
// 2. extKey on with NO source: internal detection, bit for bit.
// ---------------------------------------------------------------------------
static void TestNoSourceIsInternal() {
    std::printf("-- extKey on, no source = internal\n");
    const std::vector<float> internal = Bounce(MakeDuo({ Comp(false) }).p,
                                               "internal");
    // The descriptor below HAS extKey=1 but names no source: the insert must
    // fall back to internal detection, not to silence and not to a stale key.
    const std::vector<float> nosrc = Bounce(MakeDuo({ Comp(true) }).p, "nosrc");
    CHECK(!internal.empty() && !nosrc.empty());
    CHECK(SameSamples(internal, nosrc));

    // ...and that shared render IS internal ducking throughout, so the
    // comparison above is not two no-ops. The internal detector sees the
    // bass's own POST-FADER level (0.5 * 0.7071 = 0.3536, 9.03 dB over the
    // -30 dB threshold) — the same mono/path a key would be read at.
    const float L = RightAt(Bounce(MakeDuo().p, "dry2"), 1000);
    const double g = DuckGain(L, -30.0, 4.0);
    CHECK(std::fabs(g - 0.16353) < 0.0005);
    CHECK(std::fabs(RightAt(nosrc, 1000) / L - g) < 0.005);          // before
    CHECK(std::fabs(RightAt(nosrc, K + KDUR / 2) / L - g) < 0.005);  // during

    // A source naming a track that is not in the project is unrouted the same
    // way — fail soft, not a failed render.
    Duo d = MakeDuo({ Comp(true) });
    Track* b = d.p.FindTrack(d.bass);
    CHECK(b && !b->fx.empty());
    if (b && !b->fx.empty()) b->fx[0].sidechainSource = 9999;
    CHECK(SameSamples(internal, Bounce(d.p, "missing")));
}

// ---------------------------------------------------------------------------
// 3. The key taps POST-fader: mute, solo and the key track's fader decide.
// ---------------------------------------------------------------------------
static void TestKeyIsPostFader() {
    std::printf("-- the key tap is post-fader\n");
    const float L = RightAt(Bounce(MakeDuo().p, "dry3"), 1000);

    // A MUTED key track keys with silence, and a silent key means no
    // reduction: the bass is untouched, bit for bit the same as the same
    // project with the compressor removed. (Post-fader tap: the mute silences
    // the key — it does not hold the last value, and it is not "no key".)
    Duo m = MakeDuo({ Comp(true) }, /*muteKick=*/true);
    WireKey(m);
    const std::vector<float> muted = Bounce(m.p, "muted");
    Duo mDry = m;
    mDry.p.FindTrack(mDry.bass)->fx.clear();
    CHECK(!muted.empty());
    CHECK(SameSamples(muted, Bounce(mDry.p, "muteddry")));

    // Same for a solo-EXCLUDED key track: the soloed bass plays, the kick does
    // not, so the key path carries nothing to detect on.
    Duo s = m;
    for (Track& t : s.p.Tracks()) {
        t.muted = false;
        if (t.id == s.bass) t.soloed = true;
    }
    const std::vector<float> soloed = Bounce(s.p, "solo");
    Duo sDry = s;
    sDry.p.FindTrack(sDry.bass)->fx.clear();
    CHECK(SameSamples(soloed, Bounce(sDry.p, "solodry")));

    // The key track's own FADER is in the tap: at -40 dB the key falls to
    // 0.005 (-46 dB), below the -30 dB threshold, so the duck disappears even
    // though the kick still plays — proof the tap is after the fader, not
    // before it.
    Duo f = MakeDuo({ Comp(true) });
    WireKey(f);
    f.p.FindTrack(f.kick)->gain = 0.01f;
    const std::vector<float> faded = Bounce(f.p, "faded");
    Duo fDry = f;
    fDry.p.FindTrack(fDry.bass)->fx.clear();
    CHECK(SameSamples(faded, Bounce(fDry.p, "fadeddry")));

    // Control: with the fader back at unity the same setup ducks again.
    Duo k = MakeDuo({ Comp(true) });
    WireKey(k);
    CHECK(RightAt(Bounce(k.p, "keyed3"), K + KDUR / 2) < L * 0.6f);
}

// ---------------------------------------------------------------------------
// 4. PDC: a look-ahead limiter upstream on the KEY path must not move the duck.
// ---------------------------------------------------------------------------
static void TestKeyPdcAlignment() {
    std::printf("-- PDC: a latent key path keeps the duck in place\n");
    Duo plain = MakeDuo({ Comp(true) });
    WireKey(plain);
    const std::vector<float> ref = Bounce(plain.p, "pdc_ref");
    CHECK(!ref.empty());
    if (ref.empty()) return;

    // The key path now carries THE built-in latent effect: 5 ms of look-ahead
    // (240 frames at 48 k). The kick is well under its 0 dB ceiling, so the
    // limiter is a pure 240-frame delay of the key — and an unaligned key
    // would start the duck 240 frames EARLY. The whole bounce must stay
    // identical, not just the onset: the key has to land on the frame the
    // bass's own signal does.
    Duo lat = plain;
    lat.p.FindTrack(lat.kick)->fx.push_back(LimiterDesc(0.0f, 5.0f, 60.0f, 0.0f));
    const std::vector<float> withLim = Bounce(lat.p, "pdc_lim");
    CHECK(!withLim.empty());
    CHECK(SameSamples(ref, withLim));

    // Named explicitly, so a future change that shifted BOTH renders the same
    // way (and so still passed SameSamples) is caught here.
    const float L = RightAt(Bounce(MakeDuo().p, "dry4"), 1000);
    const int64_t onsetRef = FirstBelow(ref, 0, K + KDUR, L, 0.9f);
    const int64_t onsetLim = FirstBelow(withLim, 0, K + KDUR, L, 0.9f);
    CHECK(onsetRef >= K - 2 && onsetRef <= K + ATT + 8);
    CHECK(onsetLim == onsetRef);
}

// ---------------------------------------------------------------------------
// 4b. The key's alignment delay is real: a keyed insert on a BUS fed by a
//     latent track needs the key shifted by that latency.
// ---------------------------------------------------------------------------
// The case above has EdgeDelay(source, consumer) == 0 — the key source is the
// slowest path, so nothing has to move it. Here it is not: the consumer is a
// bus (the natural "duck the whole drum bus with the kick" routing) whose only
// feeder is a track with 5 ms of look-ahead, so the bus's input sits 240
// frames late and the key has to be delayed by exactly EdgeDelay to stay with
// it. Without that shift the duck would start 240 frames early.
static void TestKeyEdgeDelay() {
    std::printf("-- PDC: a keyed bus with a latent feeder\n");
    EnsureFixtures();
    // kick (hard left, straight to the master) + pad (hard right) routed into
    // a bus whose compressor is keyed from the kick. Nothing else is in the
    // mix, so the right channel is exactly the pad through the bus.
    auto make = [](bool latent) {
        Project p;
        p.sampleRate = SR;
        p.masterGain = 1.0f;
        const TrackId kick = p.NextTrackId();
        const TrackId pad  = p.NextTrackId();
        const TrackId bus  = p.NextTrackId();
        for (TrackId id : { kick, pad }) {
            Track t;
            t.id = id; t.type = TrackType::Audio;
            t.name = (id == kick) ? "kick" : "pad";
            t.gain = 1.0f;
            t.pan  = (id == kick) ? -1.0f : 1.0f;   // kick left, pad right
            if (id == pad) t.output = bus;
            Clip c;
            c.id = p.NextClipId();
            c.startFrame = 0; c.lengthFrames = N; c.sourceOffset = 0;
            c.sourcePath = (id == kick) ? KickWav() : BassWav();
            t.clips.push_back(c);
            if (id == pad && latent)
                t.fx.push_back(LimiterDesc(0.0f, 5.0f, 60.0f, 0.0f));
            p.AddTrack(t);
        }
        Track b;
        b.id = bus; b.type = TrackType::Bus; b.name = "bus";
        b.gain = 1.0f; b.pan = 0.0f;
        b.fx = { Comp(true) };
        b.fx[0].sidechainSource = kick;
        p.AddTrack(b);
        return p;
    };
    const std::vector<float> flat = Bounce(make(false), "busedge_flat");
    const std::vector<float> lat  = Bounce(make(true), "busedge_lat");
    CHECK(!flat.empty() && !lat.empty());
    if (flat.empty() || lat.empty()) return;
    // The limiter is transparent for this material, so the two renders are the
    // same audio — and getting there required the key's 240-frame shift.
    CHECK(SameSamples(flat, lat));

    // The right channel is the pad through the bus (0.5 * centre pan 0.7071),
    // so the hand-built duck can be read there.
    const float L = RightAt(flat, 1000);
    CHECK(std::fabs(L - 0.353553f) < 1e-4f);
    const double g = DuckGain(0.25, -30.0, 4.0);          // the key, 0.25
    const double gi = DuckGain(L, -30.0, 4.0);            // if it were internal
    CHECK(std::fabs(RightAt(flat, K + KDUR - 480) / L - g) < 0.005);
    CHECK(std::fabs(g - gi) > 0.02);                      // the two are distinguishable
    const int64_t onsetFlat = FirstBelow(flat, 0, K + KDUR, L, 0.9f);
    const int64_t onsetLat  = FirstBelow(lat, 0, K + KDUR, L, 0.9f);
    CHECK(onsetFlat >= K - 2 && onsetFlat <= K + ATT + 8);
    CHECK(onsetLat == onsetFlat);
}

// ---------------------------------------------------------------------------
// 5. The keyed GATE: the same routing, the opposite gain law.
// ---------------------------------------------------------------------------
static void TestKeyedGate() {
    std::printf("-- keyed gate opens on the key\n");
    Duo d = MakeDuo({ GateKeyed(GateDesc(-30.0f, 8.0f, 1.0f, 20.0f, 60.0f)) });
    WireKey(d);
    const std::vector<float> out = Bounce(d.p, "gate");
    const float L = RightAt(Bounce(MakeDuo().p, "dry5"), 1000);
    CHECK(!out.empty());
    if (out.empty()) return;

    // The gate is shut without a key (a 60 dB range => the floor is 1e-3) and
    // open while the kick holds the key above -30 dB. The closing time is the
    // release: from open, the gain reaches the floor's neighbourhood after
    // ~5 taus (the one-pole has to travel the whole 60 dB), so that is where
    // the "shut" assertion sits.
    CHECK(std::fabs(RightAt(out, K + KDUR / 2) / L - 1.0) < 0.01);  // open
    CHECK(RightAt(out, K + KDUR + 5 * REL) < L * 0.01f);            // shut
    CHECK(RightAt(out, K + KDUR + 5 * REL) > L * 0.0005f);          // at the floor
    const int64_t shut = FirstBelow(out, 0, K, L, 0.9f);
    CHECK(shut >= 0 && shut <= 4 * REL);   // closes on the silent key from 0
}

// ---------------------------------------------------------------------------
// 5b. A cycle involving a key abandons keys, exactly as it abandons sends.
// ---------------------------------------------------------------------------
static void TestKeyCycleFailsSoft() {
    std::printf("-- a key cycle falls back to internal detection\n");
    const std::vector<float> internal = Bounce(MakeDuo({ Comp(false) }).p,
                                               "cyc_internal");
    // The bass both keys off the kick AND feeds it: no order exists. The whole
    // edge set (outputs + sends + keys) fails to sort, so the graph falls back
    // to flat and NO key is routed — every keyed insert detects internally.
    // Partial routing would mean an effect whose detector depends on track
    // creation order, which is exactly what this policy refuses.
    Duo d = MakeDuo({ Comp(true) });
    WireKey(d);
    CHECK(d.p.FindTrack(d.bass)->output == kRoutingMaster);
    d.p.FindTrack(d.bass)->output = d.kick;
    const std::vector<float> cyc = Bounce(d.p, "cycle");
    CHECK(!cyc.empty());
    CHECK(SameSamples(internal, cyc));

    // Control: without the routing edge the same routing ducks, so the
    // equality above is the cycle's doing and not the key being ignored.
    Duo ok = MakeDuo({ Comp(true) });
    WireKey(ok);
    CHECK(!SameSamples(internal, Bounce(ok.p, "cycle_ok")));
}

// ---------------------------------------------------------------------------
// 6. Persistence: `fxsc` / `masterfxsc` round-trip and compat.
// ---------------------------------------------------------------------------
static std::string ReadFile(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void WriteFile(const std::string& path, const std::string& text) {
    std::ofstream f(path);
    f << text;
}

static void TestProjectIo() {
    std::printf("-- ProjectIO: fxsc / masterfxsc round-trip + compat\n");
    const std::string path = "/tmp/haiku_daw_sidechain_io.dawproj";

    Duo d = MakeDuo({ Comp(true) });
    WireKey(d);
    d.p.FindTrack(d.kick)->fx = { GateDesc() };        // a second, unkeyed chain
    EffectDesc mg = GateKeyed(GateDesc());
    mg.sidechainSource = d.kick;
    d.p.masterFx = { mg };

    CHECK(ProjectIO::Save(d.p, path));
    const std::string text = ReadFile(path);
    CHECK(text.find("fxsc 0 " + std::to_string(d.kick)) != std::string::npos);
    CHECK(text.find("masterfxsc 0 " + std::to_string(d.kick))
          != std::string::npos);

    Project b;
    CHECK(ProjectIO::Load(b, path));
    const Track* bb = b.FindTrack(d.bass);
    const Track* kb = b.FindTrack(d.kick);
    CHECK(bb && !bb->fx.empty());
    if (bb && !bb->fx.empty()) {
        CHECK(bb->fx[0].sidechainSource == d.kick);
        CHECK(bb->fx[0].params.size() > (size_t)kExtKeySlot);
        CHECK(bb->fx[0].p((size_t)kExtKeySlot) == 1.0f);
        // The appended slot rides along: the effect's own params are untouched.
        CHECK(bb->fx[0].p(0) == -30.0f && bb->fx[0].p(1) == 4.0f);
        CHECK(bb->fx[0].p(2) == 1.0f && bb->fx[0].p(3) == 20.0f);
        CHECK(bb->fx[0].p(4) == 0.0f);
    }
    CHECK(kb && kb->fx.size() == 1 && kb->fx[0].sidechainSource == 0);
    CHECK(b.masterFx.size() == 1);
    if (!b.masterFx.empty()) {
        CHECK(b.masterFx[0].sidechainSource == d.kick);
        CHECK(b.masterFx[0].p((size_t)kExtKeySlot) == 1.0f);
    }

    // The loaded project RENDERS the same as the one saved — the round trip is
    // not just a field surviving, it is the same audio.
    CHECK(SameSamples(Bounce(d.p, "io_saved"), Bounce(b, "io_loaded")));

    // An older build's file has no `fxsc` at all: the insert keeps the default
    // (no source, internal detection) and the chain still loads. That is what
    // "append-only" buys — a new line must never be required.
    {
        std::string old = text;
        // Searched with the leading newline: the master line is
        // "masterfxsc 0 <id>", which CONTAINS "fxsc 0 <id>".
        const std::string line = "\nfxsc 0 " + std::to_string(d.kick) + "\n";
        const size_t at = old.find(line);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) old.erase(at, line.size() - 1);
        WriteFile(path, old);
        Project c;
        CHECK(ProjectIO::Load(c, path));
        const Track* cb = c.FindTrack(d.bass);
        CHECK(cb && !cb->fx.empty() && cb->fx[0].sidechainSource == 0);
        CHECK(cb && !cb->fx.empty() && cb->fx[0].p((size_t)kExtKeySlot) == 1.0f);
    }
    // Garbage in the new line (an index that cannot address a slot, a source
    // of 0) is skipped like a malformed `fxin`, not a failed load: the rest of
    // a recoverable project survives.
    {
        std::string g = text;
        const std::string line = "\nfxsc 0 " + std::to_string(d.kick) + "\n";
        const size_t at = g.find(line);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) g.replace(at, line.size(), "\nfxsc 99 0\n");
        WriteFile(path, g);
        Project e;
        CHECK(ProjectIO::Load(e, path));
        const Track* eb = e.FindTrack(d.bass);
        CHECK(eb && !eb->fx.empty() && eb->fx[0].sidechainSource == 0);
    }
    // The same text also loads through an older build's eyes for every OTHER
    // line (the file is a normal DAW 1 file the whole way), and a project that
    // uses no sidechain writes no fxsc line at all.
    {
        Duo n = MakeDuo({ Comp(false) });
        CHECK(ProjectIO::Save(n.p, path));
        CHECK(ReadFile(path).find("fxsc") == std::string::npos);
        CHECK(ReadFile(path).find("masterfxsc") == std::string::npos);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 7. The DSP contract both hosts rely on: one block, or many.
// ---------------------------------------------------------------------------
// The RT engine runs an insert in ~512-frame blocks and the Exporter in
// 8192-frame ones, and the bounce is required to be sample-identical to
// playback — with the key handed over per block, exactly as SetSidechain
// specifies. This is the host-testable half of that promise (the engine's own
// callback is Haiku-only); the other half is that both hosts hand the slot to
// the same RunInsertSlot.
template <class Fx>
static void TestBlockInvariance(const char* name) {
    std::printf("-- %s: block size does not change a sample\n", name);
    const int NF = 20000;
    std::vector<float> audio((size_t)NF * 2, 0.0f);
    std::vector<float> key((size_t)NF * 2, 0.0f);
    for (int i = 0; i < NF; i++) {
        const float a = (i % 700 < 300) ? 0.7f : 0.15f;
        const float k = (i % 4000 < 900) ? 0.9f : 0.0f;
        audio[i * 2] = audio[i * 2 + 1] = a;
        key[i * 2]   = key[i * 2 + 1]   = k;
    }
    auto run = [&](int block) {
        Fx fx;
        fx.Prepare(SR);
        fx.SetParam(kExtKeySlot, 1.0f);
        std::vector<float> buf = audio;
        for (int off = 0; off < NF; off += block) {
            const int n = std::min(block, NF - off);
            fx.SetSidechain(key.data() + (size_t)off * 2, n);
            fx.Process(buf.data() + (size_t)off * 2, n);
        }
        return buf;
    };
    const std::vector<float> one   = run(NF);    // a single call
    const std::vector<float> small = run(512);   // the engine's block size
    const std::vector<float> big   = run(8192);  // the exporter's
    CHECK(one.size() == audio.size());
    CHECK(one == small);
    CHECK(one == big);

    // ...and the key really did change the render, so the equalities above are
    // not six seconds of the same pass-through.
    Fx alone;
    alone.Prepare(SR);
    std::vector<float> ref = audio;
    for (int off = 0; off < NF; off += 512)
        alone.Process(ref.data() + (size_t)off * 2, std::min(512, NF - off));
    CHECK(one != ref);

    // The fail-soft rule at the effect's own level: extKey on with no key ever
    // supplied is the same render as extKey never set.
    Fx unkeyed;
    unkeyed.Prepare(SR);
    unkeyed.SetParam(kExtKeySlot, 1.0f);
    std::vector<float> buf2 = audio;
    for (int off = 0; off < NF; off += 512)
        unkeyed.Process(buf2.data() + (size_t)off * 2, std::min(512, NF - off));
    CHECK(buf2 == ref);

    // A key is one block's: a block that is handed none must not detect on the
    // previous block's key. The first block is keyed, the rest are not — the
    // tail must match a run that was never keyed at all.
    Fx stale;
    stale.Prepare(SR);
    stale.SetParam(kExtKeySlot, 1.0f);
    std::vector<float> buf3 = audio;
    stale.SetSidechain(key.data(), 512);
    stale.Process(buf3.data(), 512);
    for (int off = 512; off < NF; off += 512)
        stale.Process(buf3.data() + (size_t)off * 2,
                      std::min(512, NF - off));
    // A tolerance, not equality: the two runs' envelopes started from
    // different states, so they converge towards each other rather than
    // snapping onto the same bits. A leaked key would be off by far more than
    // this — the whole point of the case.
    // The release time constant is 20 ms (960 samples), so a state difference
    // decays as exp(-n/960): by frame 12000 (13.5 taus past the keyed block)
    // anything a leaked key would have caused is far below the tolerance,
    // while a key still being detected would be off by ~0.5.
    float tail = 0.0f;
    for (int i = 12000; i < NF; i++)
        tail = std::max(tail, std::fabs(buf3[(size_t)i * 2] - ref[(size_t)i * 2]));
    CHECK(tail < 1e-4f);
}

// ---------------------------------------------------------------------------
// 8. A descriptor from before this feature existed: five params, no source.
// ---------------------------------------------------------------------------
static void TestPreFeatureDescriptor() {
    std::printf("-- a pre-sidechain descriptor stays internal\n");
    Duo d = MakeDuo({ CompressorDesc(-20.0f, 4.0f, 1.0f, 20.0f, 0.0f) });
    Track* b = d.p.FindTrack(d.bass);
    CHECK(b && !b->fx.empty());
    if (!b || b->fx.empty()) return;

    // MakeEffect applies exactly what the descriptor holds: with five slots
    // the appended one reads 0 (extKey off), so a key handed over anyway is
    // ignored and the processor detects internally.
    std::unique_ptr<IEffect> e = MakeEffect(b->fx[0], SR);
    CHECK(e != nullptr);
    if (!e) return;
    e->Prepare(SR);
    const int n = 2000;
    std::vector<float> buf((size_t)n * 2, 0.0f);
    std::vector<float> key((size_t)n * 2, 0.0f);
    for (int i = 0; i < n; i++) {
        buf[i * 2] = buf[i * 2 + 1] = 0.5f;
        key[i * 2] = key[i * 2 + 1] = 0.5f;
    }
    e->SetSidechain(key.data(), n);          // offered, but extKey is off
    e->Process(buf.data(), n);
    CHECK(std::fabs(buf[(size_t)(n - 1) * 2] / 0.5f - DuckGain(0.5, -20.0, 4.0))
          < 0.005);

    // And the render of that project equals the same project with extKey
    // explicitly 0 — the slot's absence and its zero are the same thing.
    const std::vector<float> a = Bounce(d.p, "prefeat_a");
    Duo z = d;
    z.p.FindTrack(z.bass)->fx[0].params.resize((size_t)kExtKeySlot + 1, 0.0f);
    CHECK(SameSamples(a, Bounce(z.p, "prefeat_b")));
}

int main() {
    TestKickDucksBass();
    TestNoSourceIsInternal();
    TestKeyIsPostFader();
    TestKeyPdcAlignment();
    TestKeyEdgeDelay();
    TestKeyedGate();
    TestKeyCycleFailsSoft();
    TestProjectIo();
    TestBlockInvariance<Compressor>("Compressor");
    TestBlockInvariance<Gate>("Gate");
    TestPreFeatureDescriptor();

    std::printf("sidechain_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
